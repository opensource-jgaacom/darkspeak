#include "ds/transfermanager.h"
#include "ds/file.h"
#include "ds/dsengine.h"
#include "ds/filemanager.h"
#include "ds/incomingtransfer.h"
#include "ds/outgoingtransfer.h"
#include "logfault/logfault.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QTimer>
#include <QThread>
#include <QVariant>
#include <algorithm>
#include <set>
#include <utility>

namespace ds::core {

dsfm::Result<void> TransferManager::openWorkStore(QSqlDatabase db) {
    if (QThread::currentThread()!=thread()) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    if (work_store_) return dsfm::Error{dsfm::ErrorCode::Busy};
    auto store = std::make_unique<TransferWorkStore>(std::move(db));
    auto opened = store->open(); if (!opened) return opened.error();
    work_store_ = std::move(store);
    connect(this,&TransferManager::attemptFinished,this,[this](dsfm::TransferKey key) {
        auto work=work_store_->find(key);
        if(work && work_files_.contains(work.value().file_id)) projectWork(*work_files_.at(work.value().file_id).file);
        requestWorkDispatch();
    });
    return {};
}

dsfm::Result<void> TransferManager::bindWork(std::shared_ptr<File> file, std::optional<TransferPurpose> purpose) {
    if (QThread::currentThread()!=thread() || !work_store_ || !file || file->thread()!=thread() || file->getSize()<0) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    const auto key=file->checkpointKey(); if (!key) return key.error();
    // Enrollment alone is never acceptance or feature release (e.g. GET stages
    // its source before sending the correlated result).
    const bool accepted=false;
    const bool existing=bool(work_store_->find(key.value()));
    auto enrolled=work_store_->enroll(file->getId(),key.value(),purpose.value_or(file->isThumbnail()?TransferPurpose::Preview:TransferPurpose::Attachment),quint64(file->getSize()),file->getHash(),accepted);
    if (!enrolled) return enrolled.error();
    if (purpose) {
        auto classified=work_store_->classify(key.value(),*purpose);
        if(!classified) return classified.error();
    }
    if (!work_files_.contains(file->getId())) {
        if(work_files_.size()>=4096) return dsfm::Error{dsfm::ErrorCode::QueueFull};
        auto &binding=work_files_[file->getId()];
        binding.file=file;
        const auto weak=std::weak_ptr<File>(file);
        binding.state=connect(file.get(),&File::stateChanged,this,[this,weak] {
            if(auto f=weak.lock()) projectWork(*f);
        });
        binding.recovery=connect(file.get(),&File::recoveryFinished,this,[this,weak] {
            if(auto f=weak.lock()) projectWork(*f);
        });
    }
    projectWork(*file);
    if(!existing) requestWorkDispatch();
    return {};
}

dsfm::Result<void> TransferManager::acceptWork(File &file) {
    auto bound=bindWork(file.shared_from_this());
    if(!bound) return bound;
    auto key=file.checkpointKey(); if(!key) return key.error();
    auto accepted=work_store_->accept(key.value());
    if(accepted && file.getDirection()==File::OUTGOING) projectWork(file);
    return accepted;
}

void TransferManager::projectWork(File &file) {
    const auto key=file.checkpointKey(); if(!key || !work_store_) return;
    if (file.recoveryPending()) return;
    if (attemptStarted(key.value())) return; // Live attempt alone owns its lifecycle.
    const auto previous=work_store_->find(key.value());
    if(previous && previous.value().state==WorkState::Cancelled && file.getState()!=File::FS_DONE) {
        // Resume/late model notifications cannot override an explicit cancel.
        if(auto session=find(key.value())) session->stop(TransferSession::Stop::Cancelled);
        else if(file.getState()==File::FS_CANCELLED || file.getState()==File::FS_REJECTED) forgetWork(file.getId());
        return;
    }
    WorkState state=WorkState::Queued;
    bool eligible=false;
    switch(file.getState()) {
    case File::FS_DONE: state=WorkState::Completed; break;
    case File::FS_REJECTED: case File::FS_CANCELLED: state=WorkState::Cancelled; break;
    case File::FS_FAILED: state=WorkState::Failed; break;
    case File::FS_OFFERED:
        if(file.getDirection()==File::INCOMING) state=WorkState::AwaitingAcceptance;
        else { state=WorkState::Paused; eligible=file.recoveryValidated(); } // Do not replay an offer while awaiting its answer.
        break;
    case File::FS_WAITING: case File::FS_QUEUED: eligible=true; break;
    case File::FS_PAUSED:
        state=WorkState::Paused;
        eligible=file.recoveryValidated() && (file.pauseReason()==int(dsfm::TransferPauseReason::Disconnected) ||
            (previous && previous.value().resume_requested));
        // A durable queued item may have been accepted before any attempt or
        // journal was created. It starts at zero, never claims a retained prefix.
        if(!file.recoveryPending() && !file.recoveryValidated() && previous &&
           previous.value().state==WorkState::Queued && file.pendingId().isNull()) {
            state=WorkState::Queued; eligible=true;
        }
        break;
    default: state=WorkState::Attention; break;
    }
    if(previous && previous.value().state==WorkState::AwaitingAcceptance &&
       state!=WorkState::Completed && state!=WorkState::Failed && state!=WorkState::Cancelled) {
        state=WorkState::AwaitingAcceptance; eligible=false;
    }
    if(file.failureReason()!=int(dsfm::TransferFailureReason::None)) { state=WorkState::Failed; eligible=false; }
    if(file.recoveryPending()) eligible=false;
    if(eligible && file.getDirection()==File::OUTGOING && !file.outgoingSourceCurrent()) {
        state=WorkState::Attention; eligible=false;
    }
    const bool terminal=state==WorkState::Completed || state==WorkState::Failed || state==WorkState::Cancelled;
    if(previous && previous.value().state==state && previous.value().eligible==(eligible && !previous.value().user_paused)) {
        if(terminal) forgetWork(file.getId());
        return;
    }
    auto projected=work_store_->project(key.value(),state,eligible);
    if(!projected) { LFLOG_WARN << "Transfer work projection deferred; code=" << int(projected.error().code); return; }
    requestWorkDispatch();
    if(terminal) forgetWork(file.getId());
}

std::shared_ptr<File> TransferManager::boundWorkFile(int file_id) const {
    const auto it=work_files_.find(file_id);
    return it==work_files_.end() ? nullptr : it->second.file;
}

void TransferManager::forgetWork(int file_id) {
    auto it=work_files_.find(file_id); if(it==work_files_.end()) return;
    disconnect(it->second.state); disconnect(it->second.recovery); work_files_.erase(it);
    requestWorkDispatch();
}

dsfm::Result<void> TransferManager::pauseQueuedWork(dsfm::TransferKey key,bool paused) {
    if(QThread::currentThread()!=thread() || !work_store_) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    if(attemptStarted(key)) return dsfm::Error{dsfm::ErrorCode::Busy}; // Active cooperative pause is a later item.
    auto saved=work_store_->setPaused(key,paused); if(!saved) return saved;
    auto work=work_store_->find(key);
    if(work && work_files_.contains(work.value().file_id)) projectWork(*work_files_.at(work.value().file_id).file);
    schedule();
    requestWorkDispatch();
    return {};
}
dsfm::Result<void> TransferManager::reorderWork(dsfm::TransferKey key,dsfm::TransferKey before) {
    if(QThread::currentThread()!=thread() || !work_store_) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    auto saved=work_store_->reorder(key,before); if(saved) { schedule(); requestWorkDispatch(); } return saved;
}
dsfm::Result<void> TransferManager::setWorkPriority(dsfm::TransferKey key, TransferPriority priority) {
    if(QThread::currentThread()!=thread() || !work_store_) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    auto saved=work_store_->setPriority(key,priority);
    if(saved) { requestSchedule(); requestWorkDispatch(); }
    return saved;
}
dsfm::Result<void> TransferManager::cancelWork(dsfm::TransferKey key) {
    if(QThread::currentThread()!=thread() || !work_store_) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    auto work=work_store_->find(key); if(!work) return work.error();
    if(work.value().state==WorkState::Completed || work.value().state==WorkState::Failed || work.value().state==WorkState::Cancelled)
        return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    // Fence durable intent before signalling the sole attempt owner. A crash
    // between databases cannot turn cancellation back into eligible work.
    auto saved=work_store_->project(key,WorkState::Cancelled,false); if(!saved) return saved;
    if(auto session=find(key)) session->stop(TransferSession::Stop::Cancelled);
    else if(auto it=work_files_.find(work.value().file_id);it!=work_files_.end()) it->second.file->cancel();
    requestSchedule(); requestWorkDispatch();
    return {};
}
void TransferManager::requestWorkDispatch() {
    if(work_wakeup_ || budgetClosing()) return;
    work_wakeup_=true;
    QTimer::singleShot(0,this,[this] {
        work_wakeup_=false;
        emit workChanged();
        dispatchWork();
    });
}
void TransferManager::dispatchWork() {
    if(QThread::currentThread()!=thread() || !work_store_ || budgetClosing()) return;
    // A finite pass over at most 4096 value rows, yielding every 100. Sessions
    // remain capped globally and per contact. Offline rows allocate no session.
    if(dispatch_running_) { dispatch_again_=true; return; }
    dispatch_running_=true;
    dispatch_candidates_.clear();
    ++dispatch_round_;
    dispatchPage(0);
}
void TransferManager::dispatchPage(int offset) {
    if(!work_store_ || budgetClosing()) { dispatch_candidates_.clear(); dispatch_running_=false; requestSchedule(); return; }
    auto page=work_store_->page(offset,100); if(!page) { dispatch_candidates_.clear(); dispatch_running_=false; requestSchedule(); return; }
    for(const auto &work:page.value()) {
        if(!work.eligible || work.user_paused || find(work.key)) continue;
        dispatch_candidates_.push_back(work);
        waiting_round_.try_emplace(work.id,dispatch_round_);
    }
    if(page.value().size()==100 && offset+100<4096) {
        QTimer::singleShot(0,this,[this,offset] { dispatchPage(offset+100); });
        return;
    }
    // Finite, bounded snapshot: age also applies before session registration,
    // so a stream of new conversations cannot hide old bulk beyond its cap.
    std::stable_sort(dispatch_candidates_.begin(),dispatch_candidates_.end(),[this](const auto &a,const auto &b) {
        const auto rank=[this](const TransferWork &w) {
            const auto since=waiting_round_.at(w.id);
            const bool aged=dispatch_round_-since>=8;
            return std::tuple{aged?0:1,aged?since:quint64(0),aged?0:int(schedulingClass(w.purpose)),aged?0:int(w.priority),w.position,w.id};
        };
        return rank(a)<rank(b);
    });
    dispatchCandidates(0);
}
void TransferManager::dispatchCandidates(int offset) {
    if(budgetClosing()) { dispatch_candidates_.clear(); dispatch_running_=false; requestSchedule(); return; }
    const int end=std::min(offset+100,int(dispatch_candidates_.size()));
    for(int i=offset;i<end;++i) {
        const auto &work=dispatch_candidates_[i];
        if(work.purpose==TransferPurpose::Preview && work.key.side==dsfm::TransferSide::Sender) continue;
        const auto it=work_files_.find(work.file_id); if(it==work_files_.end()) continue;
        const auto file=it->second.file;
        auto *contact=file->getContact();
        if(!contact || !contact->isOnline() || contact->getState()!=Contact::ACCEPTED || contact->iBlocked() || file->recoveryPending()) continue;
        if(file->getDirection()==File::OUTGOING) contact->startOutgoingFile(*file,
            (file->getState()==File::FS_PAUSED || file->getState()==File::FS_OFFERED) && file->recoveryValidated()
                ? OutgoingTransferStart::Recover : OutgoingTransferStart::Offer);
        else contact->startIncomingFile(*file,file->pendingId().isNull() && file->storageId().isNull()
                    ? IncomingTransferStart::Accept : IncomingTransferStart::Resume);
    }
    if(end<dispatch_candidates_.size()) {
        QTimer::singleShot(0,this,[this,end] { dispatchCandidates(end); });
        return;
    }
    std::set<QUuid> waiting;
    for(const auto &work:dispatch_candidates_) if(!find(work.key)) waiting.insert(work.id);
    std::erase_if(waiting_round_,[&](const auto &item) { return !waiting.contains(item.first); });
    dispatch_candidates_.clear();
    dispatch_running_=false;
    requestSchedule();
    if(std::exchange(dispatch_again_,false)) requestWorkDispatch();
}
void TransferManager::restoreWork() {
    if(QThread::currentThread()!=thread() || restore_started_ || !work_store_ || budgetClosing()) return;
    restore_started_=true;
    restorePage(0,0);
}
void TransferManager::restorePage(int after,int count) {
    if(budgetClosing()) return;
    // Existing file rows are feature-owned migration inputs, not another queue.
    QSqlQuery q;
    q.prepare("SELECT id FROM file WHERE id>? AND (id IN(SELECT file_id FROM transfer_work_v1) OR state NOT IN(6,7,8,9)) ORDER BY id LIMIT 100");
    q.addBindValue(after);
    if(!q.exec()) { LFLOG_TRACE << "Transfer work recovery failed stage=feature-page native-code=" << q.lastError().nativeErrorCode().toStdString(); return; }
    QVector<int> ids; while(q.next()) ids.push_back(q.value(0).toInt()); q.finish();
    for(int id:ids) {
        if(count++>=4096) { LFLOG_WARN << "Transfer work recovery reached local registry bound"; return; }
        try {
            auto file=DsEngine::instance().getFileManager()->getFile(id);
            // Preparation has not established immutable size/hash yet.
            if(file->getHash().size()==32) {
                const auto key=file->checkpointKey();
                const bool known=key && bool(work_store_->find(key.value()));
                auto bound=bindWork(file);
                if(bound && !known && (file->getState()!=File::FS_OFFERED || file->getDirection()==File::OUTGOING)) {
                    auto accepted=work_store_->accept(key.value());
                    if(accepted) projectWork(*file);
                }
                if(bound && known) {
                    auto work=work_store_->find(key.value());
                    if(work && work.value().state==WorkState::Cancelled && file->getState()!=File::FS_DONE)
                        file->cancel(); // Replay durable intent through the sole cleanup owner.
                }
                if(!bound) LFLOG_DEBUG << "Transfer work recovery binding declined; code=" << int(bound.error().code);
            }
        } catch(...) { LFLOG_WARN << "Transfer work recovery failed stage=feature-binding"; }
    }
    if(ids.size()==100) QTimer::singleShot(0,this,[this,after=ids.back(),count] { restorePage(after,count); });
}
} // namespace ds::core
