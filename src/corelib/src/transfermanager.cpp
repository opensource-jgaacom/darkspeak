#include "ds/transfermanager.h"

#include <QDebug>
#include <QCoroSignal>
#include <QCoroTimer>
#include <QPointer>
#include <QThread>
#include <algorithm>
#include <vector>
#include <set>
#include <stdexcept>
#include <limits>

namespace ds::core {

struct TransferReservation::Budget {
    std::set<std::tuple<QUuid, QUuid, QUuid, dsfm::TransferSide>> occupied;
    QPointer<TransferManager> manager;
    bool closing = false;
};
struct TransferManager::Registry {
    std::map<Key, Entry> sessions;
    std::deque<Key> queued;
    quint64 admissions = 0;
    std::map<std::pair<QUuid,QUuid>,quint64> last_contact;
    std::map<QUuid,quint64> last_identity;
};

TransferManager::Key TransferManager::index(const dsfm::TransferKey &key) {
    return {key.local_scope, key.remote_scope, key.transfer, key.side};
}
TransferReservation::TransferReservation(std::shared_ptr<Budget> budget, dsfm::TransferKey key)
    : budget_{std::move(budget)}, key_{key} {}
TransferReservation::~TransferReservation() {
    budget_->occupied.erase({key_.local_scope, key_.remote_scope, key_.transfer, key_.side});
    if (budget_->manager) {
        const auto manager = budget_->manager;
        QMetaObject::invokeMethod(manager, [manager] { if (manager) manager->schedule(); }, Qt::QueuedConnection);
    }
}

TransferManager::TransferManager(QObject *parent)
    : QObject{parent}, budget_{std::make_shared<TransferReservation::Budget>()}, registry_{std::make_shared<Registry>()} {
    budget_->manager = this;
    scheduling_clock_.start();

}
TransferManager::~TransferManager() {
    budget_->closing = true;
    budget_->manager = nullptr;
    for (const auto &[key, entry] : registry_->sessions) {
        Q_UNUSED(key);
        entry.session->stop(TransferSession::Stop::Disconnected);
    }
    // Running roots retain the registry until their awaited I/O/finalization
    // returns. Normal shutdown awaits them before destroying application services.
}

bool TransferManager::budgetClosing() const { return budget_->closing; }
bool TransferManager::attemptStarted(const dsfm::TransferKey &key) const {
    const auto it = registry_->sessions.find(index(key));
    return it != registry_->sessions.end() && it->second.root.has_value();
}

size_t TransferManager::active(QUuid local_scope, QUuid remote_scope) const {
    size_t count = 0;
    for (const auto &key : budget_->occupied)
        if (std::get<0>(key) == local_scope && std::get<1>(key) == remote_scope) ++count;
    return count;
}

dsfm::Result<std::shared_ptr<TransferReservation>> TransferManager::reserve(const dsfm::TransferKey &key) {
    if (budget_->closing) return dsfm::Error{dsfm::ErrorCode::ShuttingDown};
    return reserveActive(key);
}

dsfm::Result<std::shared_ptr<TransferReservation>> TransferManager::reserveActive(const dsfm::TransferKey &key) {
    if (QThread::currentThread() != thread() || key.local_scope.isNull() || key.remote_scope.isNull() || key.transfer.isNull())
        return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    const auto id = index(key);
    if (budget_->occupied.size() >= 8 || budget_->occupied.contains(id) || active(key.local_scope, key.remote_scope) >= 2)
        return dsfm::Error{dsfm::ErrorCode::Busy};
    budget_->occupied.insert(id);
    return std::shared_ptr<TransferReservation>{new TransferReservation{budget_, key}};
}

dsfm::Result<void> TransferManager::submit(std::shared_ptr<TransferSession> session) {
    if (!session || QThread::currentThread() != thread() || session->thread() != thread() ||
        session->parent() || session->manager_ || session->phase() != TransferSession::Phase::Preparing ||
        (session->connection().isNull() && session->accepting()))
        return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    if (budget_->closing) return dsfm::Error{dsfm::ErrorCode::ShuttingDown};
    const auto &key = session->key();
    if (key.local_scope.isNull() || key.remote_scope.isNull() || key.transfer.isNull())
        return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    const auto id = index(key);
    if (work_store_ && session->accepting()) {
        auto work = work_store_->find(key);
        if (!work || work.value().file_id != session->localFile()) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
        if (work.value().user_paused || (!work.value().eligible && !session->reconciliationOnly())) return dsfm::Error{dsfm::ErrorCode::Busy};
    }
    if (registry_->sessions.size() >= 256) return dsfm::Error{dsfm::ErrorCode::QueueFull};
    if (registry_->sessions.contains(id)) return dsfm::Error{dsfm::ErrorCode::Busy};
    size_t registered = 0;
    for (const auto &[pending, entry] : registry_->sessions) {
        Q_UNUSED(entry);
        if (std::get<0>(pending) == key.local_scope && std::get<1>(pending) == key.remote_scope) ++registered;
    }
    if (registered >= 64 + 2) return dsfm::Error{dsfm::ErrorCode::QueueFull};
    session->manager_ = this;
    connect(session.get(), &TransferSession::phaseChanged, this, [this] { requestSchedule(); emit workChanged(); });
    registry_->sessions.emplace(id, Entry{std::move(session), {}, {}, registry_->admissions});
    registry_->queued.push_back(id);
    if (work_store_) requestSchedule(); else schedule();
    return {};
}

std::shared_ptr<TransferSession> TransferManager::find(const dsfm::TransferKey &key) const {
    const auto it = registry_->sessions.find(index(key));
    return it == registry_->sessions.end() ? nullptr : it->second.session;
}

std::shared_ptr<TransferSession> TransferManager::findLocalFile(int local_file) const {
    if (local_file <= 0) return {};
    for (const auto &[key, entry] : registry_->sessions) {
        Q_UNUSED(key);
        if (entry.session->localFile() == local_file) return entry.session;
    }
    return {};
}

void TransferManager::requestSchedule() {
    if (schedule_wakeup_) return;
    schedule_wakeup_=true;
    QTimer::singleShot(0,this,[this] { schedule_wakeup_=false; schedule(); });
}

void TransferManager::schedule() {
    // Collect a complete dispatch pass before admission; a bulk row encountered
    // on the first page cannot overtake a conversation on a later page.
    if (dispatch_running_ && !budget_->closing) return;
    std::map<Key,TransferWork> work;
    for (const auto &id : registry_->queued) {
        auto &entry=registry_->sessions.at(id);
        if (work_store_) {
            if (auto row=work_store_->find(entry.session->key())) work.emplace(id,row.value());
        }
    }
    std::vector<Key> ready;
    while (!registry_->queued.empty()) {
        auto best=registry_->queued.end();
        const auto rank=[&](const Key &id) {
            const auto &entry=registry_->sessions.at(id);
            const auto row=work.find(id);
            const bool aged=registry_->admissions-entry.waiting_since>=16;
            const auto scope=std::pair{std::get<0>(id),std::get<1>(id)};
            const auto identity=registry_->last_identity.find(scope.first);
            const auto contact=registry_->last_contact.find(scope);
            return std::tuple{entry.session->accepting()?1:0, aged?0:1,
                aged?entry.waiting_since:quint64(0),
                aged?0:(row==work.end()?int(TransferClass::Bulk):int(schedulingClass(row->second.purpose))),
                aged?0:(row==work.end()?int(TransferPriority::Normal):int(row->second.priority)),
                identity==registry_->last_identity.end()?quint64(0):identity->second,
                contact==registry_->last_contact.end()?quint64(0):contact->second,
                row==work.end()?std::numeric_limits<qint64>::max():row->second.position,id};
        };
        for (auto it=registry_->queued.begin();it!=registry_->queued.end();++it) {
            const auto &entry=registry_->sessions.at(*it);
            if (budget_->occupied.size()>=8 || active(std::get<0>(*it),std::get<1>(*it))>=2) continue;
            if (entry.session->accepting()) {
                if (budget_->closing) continue;
                if (work_store_) {
                    const auto row=work.find(*it);
                    if (row==work.end() || row->second.user_paused ||
                        (!row->second.eligible && !entry.session->reconciliationOnly())) continue;
                }
            }
            if (best==registry_->queued.end() || rank(*it)<rank(*best)) best=it;
        }
        if (best==registry_->queued.end()) break;
        const auto id=*best;
        auto &entry=registry_->sessions.at(id);
        // Cleanup owners bypass eligibility, while retaining the same finite
        // I/O reservations until finalization joins all outstanding operations.
        auto lease=reserveActive(entry.session->key());
        if (!lease) break;
        if (entry.session->accepting()) {
            if (work_store_ && !work_store_->project(entry.session->key(),WorkState::Active,true)) break;
            ++registry_->admissions;
            registry_->last_identity[std::get<0>(id)]=registry_->admissions;
            registry_->last_contact[{std::get<0>(id),std::get<1>(id)}]=registry_->admissions;
            // Keep scheduling history bounded to registered scopes.
            if (registry_->last_contact.size()>256) registry_->last_contact.erase(registry_->last_contact.begin());
            if (registry_->last_identity.size()>256) registry_->last_identity.erase(registry_->last_identity.begin());
        }
        entry.reservation=std::move(lease).value();
        entry.started_at=scheduling_clock_.elapsed();
        registry_->queued.erase(best);
        ready.push_back(id);
    }
    for (const auto &id : ready) {
        auto &entry=registry_->sessions.at(id);
        if (!entry.root) entry.root.emplace(run(registry_,id,this));
    }
    preemptForConversation();
    emit admissionChanged();
}

void TransferManager::preemptForConversation() {
    if (!work_store_ || budget_->closing) return;
    const auto now=scheduling_clock_.elapsed();
    qint64 retry=30000;
    bool delayed=false;
    for (const auto &waiting : registry_->queued) {
        const auto &candidate=registry_->sessions.at(waiting).session;
        auto work=work_store_->find(candidate->key());
        if (!candidate->accepting() || !work || !work.value().eligible || work.value().user_paused ||
            schedulingClass(work.value().purpose)!=TransferClass::Conversation) continue;
        const auto local=std::get<0>(waiting), remote=std::get<1>(waiting);
        const bool contact_full=active(local,remote)>=2;
        if (!contact_full && budget_->occupied.size()<8) continue;
        const auto relevant=[&](const Key &id) {
            return !contact_full || (std::get<0>(id)==local && std::get<1>(id)==remote);
        };
        // One outstanding negotiation per limiting resource. Never free more
        // slots than the eligible conversation can actually use.
        bool pending=false;
        for (const auto &[id,entry] : registry_->sessions)
            if (relevant(id) && entry.reservation && entry.session->cooperativePausePending()) pending=true;
        if (pending) continue;
        for (auto &[id,entry] : registry_->sessions) {
            if (!relevant(id) || !entry.reservation || !entry.root || !entry.session->accepting() ||
                entry.session->phase()!=TransferSession::Phase::Transferring || !entry.session->supportsCooperativePause()) continue;
            auto victim=work_store_->find(entry.session->key());
            if (!victim || victim.value().user_paused || schedulingClass(victim.value().purpose)!=TransferClass::Bulk) continue;
            const auto history=preemption_after_.find(id);
            const auto earliest=std::max(entry.started_at+2000,history==preemption_after_.end()?qint64(0):history->second);
            if (now<earliest) { delayed=true; retry=std::min(retry,earliest-now); continue; }
            if (entry.session->requestCooperativePause()) {
                preemption_after_[id]=now+30000;
                if (preemption_after_.size()>4096) preemption_after_.erase(preemption_after_.begin());
                emit workChanged();
                break;
            }
        }
    }
    if (delayed && !preemption_wakeup_) {
        preemption_wakeup_=true;
        QTimer::singleShot(int(std::max(qint64(1),retry)),this,[this] {
            preemption_wakeup_=false; requestSchedule();
        });
    }
}

void TransferManager::park(const dsfm::TransferKey &key) {
    registry_->sessions.at(index(key)).reservation.reset();
}

QCoro::Task<void> TransferManager::acquire(const dsfm::TransferKey &key) {
    const auto id = index(key);
    if (registry_->sessions.at(id).reservation) co_return;
    if (std::find(registry_->queued.begin(), registry_->queued.end(), id) == registry_->queued.end())
        registry_->queued.push_back(id);
    registry_->sessions.at(id).waiting_since=registry_->admissions;
    schedule();
    while (!registry_->sessions.at(id).reservation) co_await qCoro(this, &TransferManager::admissionChanged);
}

QCoro::Task<void> TransferManager::run(std::shared_ptr<Registry> registry, Key key, QPointer<TransferManager> manager) {
    auto session = registry->sessions.at(key).session;
    // QCoro tasks start eagerly. Queue execution so the retained root is in
    // the registry before model notifications can reenter application shutdown.
    QTimer launch;
    launch.setSingleShot(true);
    launch.start(0);
    co_await qCoro(launch).waitForTimeout();
    co_await session->run();
    if (manager && manager->work_store_ && session->cooperativePauseCompleted() && session->outcome() &&
        session->outcome()->kind==TransferSession::Outcome::Kind::Paused &&
        session->outcome()->pause==dsfm::TransferPauseReason::FeaturePaused) {
        auto saved=manager->work_store_->resumeAfterPreemption(session->key());
        if (!saved) qWarning() << "Transfer automatic resume preference could not be persisted, code" << int(saved.error().code);
    }
    registry->sessions.at(key).reservation.reset();
    // Reap the retained root after it returns, not from its own stack.
    QTimer::singleShot(0, [registry, key, transfer_key = session->key(), manager] {
        registry->sessions.erase(key);
        if (manager) emit manager->attemptFinished(transfer_key);
    });
}

void TransferManager::retireConnection(QUuid connection) {
    for (const auto &[key, entry] : registry_->sessions) {
        Q_UNUSED(key);
        if (entry.session->connection() == connection) entry.session->stop(TransferSession::Stop::Disconnected);
    }
}

QCoro::Task<void> TransferManager::shutdown() {
    budget_->closing = true;
    for (const auto &[key, entry] : registry_->sessions) {
        Q_UNUSED(key);
        entry.session->stop(TransferSession::Stop::Disconnected);
    }
    // Keep each root alive while awaiting it; queued reaping cannot invalidate it.
    while (!registry_->sessions.empty()) {
        schedule();
        auto it = std::find_if(registry_->sessions.begin(), registry_->sessions.end(), [](const auto &entry) {
            return entry.second.root.has_value();
        });
        if (it == registry_->sessions.end()) {
            co_await qCoro(this, &TransferManager::admissionChanged);
            continue;
        }
        const auto key = it->first;
        auto root = std::move(*it->second.root);
        co_await root;
        registry_->sessions.erase(key);
    }
}

} // namespace ds::core
