#include <ds/transferqueuemodel.h>
#include <QTimer>
#include <QSet>
#include <QCoroTimer>
#include <algorithm>

namespace ds::models {
using namespace core;
namespace {
QString safeText(QString text) {
    text=text.left(256);
    for(auto &ch:text) if(ch.isNull() || !ch.isPrint() || ch.category()==QChar::Other_Format) ch=QChar(' ');
    return text;
}
bool terminal(const TransferWork &w) { return w.state==WorkState::Completed || w.state==WorkState::Failed || w.state==WorkState::Cancelled; }
bool movable(const TransferWork &w) { return w.state==WorkState::Queued || w.state==WorkState::Paused; }
bool peers(const TransferWork &a,const TransferWork &b) {
    return a.key.local_scope==b.key.local_scope && a.key.remote_scope==b.key.remote_scope &&
        schedulingClass(a.purpose)==schedulingClass(b.purpose) && a.priority==b.priority && movable(a) && movable(b);
}
QString stateText(const TransferWork &w) {
    if(w.user_paused && !terminal(w)) return TransferQueueModel::tr("Paused by you");
    switch(w.state) {
    case WorkState::AwaitingAcceptance: return TransferQueueModel::tr("Needs approval");
    case WorkState::Queued: return w.eligible?TransferQueueModel::tr("Waiting"):TransferQueueModel::tr("Waiting for contact or feature");
    case WorkState::Active: return TransferQueueModel::tr("Transferring / preparing");
    case WorkState::Paused: return TransferQueueModel::tr("Paused");
    case WorkState::Completed: return TransferQueueModel::tr("Completed");
    case WorkState::Failed: return TransferQueueModel::tr("Failed");
    case WorkState::Cancelled: return TransferQueueModel::tr("Cancelled");
    case WorkState::Attention: return TransferQueueModel::tr("Needs attention");
    }
    return {};
}
QString purposeText(TransferPurpose p) {
    switch(p) {
    case TransferPurpose::Attachment:return TransferQueueModel::tr("Conversation");
    case TransferPurpose::Preview:return TransferQueueModel::tr("Preview");
    case TransferPurpose::Catalog:return TransferQueueModel::tr("Directory listing");
    case TransferPurpose::FileShare:return TransferQueueModel::tr("FileShare");
    }
    return {};
}
}
TransferQueueModel::TransferQueueModel(TransferManager &manager,Reader reader,Decision decision,QObject *parent)
    :QAbstractListModel(parent),manager_(&manager),reader_(std::move(reader)),decision_(std::move(decision)) {
    timer_.setSingleShot(true);
    timer_.setInterval(1000); // coalesces flow/checkpoint updates, including active journal changes
    connect(&timer_,&QTimer::timeout,this,&TransferQueueModel::refresh);
    connect(&manager,&TransferManager::workChanged,this,[this] { if(!timer_.isActive()) timer_.start(); });
    timer_.start();
    QTimer::singleShot(0,this,&TransferQueueModel::refresh);
}
TransferQueueModel::~TransferQueueModel() = default;
int TransferQueueModel::rowCount(const QModelIndex &parent) const { return parent.isValid()?0:int(rows_.size()); }
QVariant TransferQueueModel::data(const QModelIndex &index,int role) const {
    if(!index.isValid() || index.column()!=0 || index.row()<0 || index.row()>=rows_.size() || role!=Qt::UserRole) return {};
    return rows_[index.row()].display;
}
QHash<int,QByteArray> TransferQueueModel::roleNames() const { return {{Qt::UserRole,"entry"}}; }
bool TransferQueueModel::canFetchMore(const QModelIndex &parent) const { return !parent.isValid() && more_ && !loading_ && limit_<4096; }
void TransferQueueModel::fetchMore(const QModelIndex &parent) { if(canFetchMore(parent)) { limit_=std::min(limit_+100,4096); invalidate(); } }
void TransferQueueModel::setError(QString error) { if(error_!=error) { error_=std::move(error); emit errorChanged(); } }
bool TransferQueueModel::setScope(QUuid identity,QUuid contact,QString identity_name,QString contact_name) {
    if(!contact.isNull() && identity.isNull()) { setError(tr("Select a local identity for this contact.")); return false; }
    setError({});
    filter_.identity=identity; filter_.contact=contact;
    scope_label_=tr("All transfers");
    if(!identity.isNull()) scope_label_+=" → "+(identity_name.isEmpty()?identity.toString(QUuid::WithoutBraces):safeText(identity_name));
    if(!contact.isNull()) scope_label_+=" → "+(contact_name.isEmpty()?contact.toString(QUuid::WithoutBraces):safeText(contact_name));
    limit_=100; more_=false;
    // Clear old context immediately, including while an asynchronous journal read is pending.
    beginResetModel(); rows_.clear(); endResetModel(); counts_.clear(); emit countsChanged();
    emit scopeChanged(); invalidate(); return true;
}
void TransferQueueModel::setCategory(int category) {
    if(category<0 || category>4 || category==filter_.category) return;
    filter_.category=category; limit_=100; emit filtersChanged(); invalidate();
}
void TransferQueueModel::setDirection(int direction) {
    if(direction< -1 || direction>1 || direction==filter_.direction) return;
    filter_.direction=direction; limit_=100; emit filtersChanged(); invalidate();
}
void TransferQueueModel::invalidate() { ++generation_; refresh(); }
void TransferQueueModel::refresh() {
    if(loading_) { again_=true; return; }
    reload();
}
QCoro::Task<void> TransferQueueModel::reload() {
    if(!manager_ || !manager_->workStore()) co_return;
    loading_=true;
    timer_.stop();
    const auto guard=QPointer(this); const auto generation=generation_; const auto filter=filter_; const int limit=limit_;
    auto reader=reader_; // survives model destruction during an asynchronous read
    QVector<Row> rows; bool more=false, failed=false;
    for(int offset=0;offset<limit;offset+=100) {
        auto page=manager_->workStore()->viewPage(filter,offset,std::min(100,limit-offset));
        if(!page) { failed=true; break; }
        more=page.value().size()==std::min(100,limit-offset);
        for(const auto &w:page.value()) {
            auto meta=co_await reader(w);
            if(!guard) co_return;
            if(!manager_) { loading_=false; co_return; }
            if(generation!=generation_) break;
            const bool send=w.key.side==dsfm::TransferSide::Sender;
            const auto class_name=schedulingClass(w.purpose)==TransferClass::Conversation?tr("Conversation first"):
                schedulingClass(w.purpose)==TransferClass::Utility?tr("Utility"):tr("Bulk");
            const auto session=manager_->find(w.key);
            const auto state_name=session && session->pausingForPriority()?tr("Pausing for higher-priority transfer"):stateText(w);
            QVariantMap d{{"workId",w.id},{"identityId",w.key.local_scope},{"contactId",w.key.remote_scope},
                {"identity",safeText(meta.identity)},{"contact",safeText(meta.contact)},
                {"title",w.purpose==TransferPurpose::Catalog?tr("Directory listing"):safeText(meta.title)},
                {"direction",send?tr("Sending"):tr("Receiving")},{"purpose",purposeText(w.purpose)},
                {"state",state_name},{"reason",safeText(meta.reason)},{"priority",int(w.priority)},
                {"priorityClass",class_name},{"position",w.position+1},{"visiblePosition",rows.size()+1},
                {"totalBytes",QVariant::fromValue(w.size)},{"provisionalBytes",QVariant::fromValue(std::min(w.size,meta.provisional))},
                {"verifiedBytes",QVariant::fromValue(std::min(w.size,meta.verified))},{"journalKnown",meta.journal_known},
                {"canPause",movable(w) && !w.user_paused},{"canResume",movable(w) && w.user_paused},
                {"canCancel",!terminal(w)},{"canPrioritize",!terminal(w)},
                {"canAccept",bool(decision_) && w.state==WorkState::AwaitingAcceptance && !send && meta.can_accept},
                {"canReject",bool(decision_) && w.state==WorkState::AwaitingAcceptance && !send}};
            rows.push_back({w,std::move(d)});
        }
        if(generation!=generation_ || !more) break;
        // Explicit event-loop yield even for synchronous metadata fixtures.
        QTimer yield; yield.setSingleShot(true); yield.start(0); co_await yield;
        if(!guard) co_return;
    }
    if(generation==generation_) {
        if(!failed) {
            auto totals=manager_->workStore()->viewCounts(filter);
            if(totals) {
                const auto &c=totals.value();
                QVariantMap counts{{"total",c.total},{"active",c.active},{"queued",c.queued},{"attention",c.attention},
                    {"completed",c.completed},{"sending",c.sending},{"receiving",c.receiving}};
                if(counts!=counts_) { counts_=counts; emit countsChanged(); }
            } else failed=true;
        }
        if(!failed) {
            auto global=manager_->workStore()->viewCounts({});
            if(global) {
                QVariantMap c{{"active",global.value().active},{"queued",global.value().queued}};
                if(c!=application_counts_) { application_counts_=c; emit countsChanged(); }
            }
            more_=more; publish(std::move(rows));
        }
        const auto read_error=tr("Transfer status could not be read. Try again.");
        if(failed) setError(read_error);
        else if(error_==read_error) setError({});
    }
    loading_=false;
    timer_.start();
    if(again_ || generation!=generation_) { again_=false; QTimer::singleShot(0,this,&TransferQueueModel::refresh); }
}
void TransferQueueModel::publish(QVector<Row> rows) {
    // Stable IDs preserve delegates/focus; progress never resets the whole list.
    QSet<QUuid> retained;
    for(const auto &row:rows) retained.insert(row.work.id);
    for(int i=int(rows_.size())-1;i>=0;--i) {
        if(!retained.contains(rows_[i].work.id)) {
            beginRemoveRows({},i,i); rows_.removeAt(i); endRemoveRows();
        }
    }
    for(int i=0;i<rows.size();++i) {
        int at=i; while(at<rows_.size() && rows_[at].work.id!=rows[i].work.id) ++at;
        if(at==rows_.size()) { beginInsertRows({},i,i); rows_.insert(i,rows[i]); endInsertRows(); }
        else {
            if(at!=i) { beginMoveRows({},at,at,{},i); rows_.move(at,i); endMoveRows(); }
            const bool changed=rows_[i].display!=rows[i].display; rows_[i]=rows[i];
            if(changed) emit dataChanged(index(i),index(i),{Qt::UserRole});
        }
    }
}
void TransferQueueModel::command(QUuid id,QString action,int value) {
    if(applying_ || !manager_ || !manager_->workStore()) return;
    const auto it=std::find_if(rows_.begin(),rows_.end(),[&](const Row &r){return r.work.id==id;});
    if(it==rows_.end()) { setError(tr("This transfer is outside the current view.")); emit commandFinished(id,int(dsfm::ErrorCode::PermissionDenied)); return; }
    const auto work=it->work; const auto generation=generation_;
    applying_=true; emit busyChanged();
    QTimer::singleShot(0,this,[this,id,work,action,value,generation] {
        dsfm::Result<void> result=dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        if(manager_ && manager_->workStore() && generation==generation_) {
            const auto current=manager_->workStore()->find(work.key);
            if(current && current.value().id==id) {
                const auto &w=current.value();
                if(action=="priority") result=manager_->setWorkPriority(w.key,TransferPriority(value));
                else if(action=="pause" || action=="resume") result=manager_->pauseQueuedWork(w.key,action=="pause");
                else if(action=="cancel") result=manager_->cancelWork(w.key);
                else if((action=="accept" || action=="reject") && decision_ && w.state==WorkState::AwaitingAcceptance && w.key.side==dsfm::TransferSide::Receiver)
                    result=decision_(w,action=="accept");
                else if((action=="up" || action=="down") && movable(w)) {
                    const int at=int(std::distance(rows_.begin(),std::find_if(rows_.begin(),rows_.end(),[&](const Row &r){return r.work.id==id;})));
                    const int step=action=="up"?-1:1;
                    for(int i=at+step;i>=0 && i<rows_.size();i+=step) {
                        if(!peers(w,rows_[i].work)) continue;
                        // Down is implemented by moving its next visible peer before it.
                        result=step<0?manager_->reorderWork(w.key,rows_[i].work.key):manager_->reorderWork(rows_[i].work.key,w.key);
                        break;
                    }
                }
            }
        }
        applying_=false; emit busyChanged();
        setError(result?QString{}:tr("Action unavailable (%1). Reorder within the same contact, class and priority; active pause requires a safe checkpoint.").arg(int(result.error().code)));
        emit commandFinished(id,result?0:int(result.error().code)); invalidate();
    });
}
}
