#include "ds/transferwork.h"
#include "logfault/logfault.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <limits>

namespace ds::core {
namespace {
using dsfm::Error;
using dsfm::ErrorCode;
constexpr auto columns = "work_id,local_scope,remote_scope,transfer_id,side,file_id,purpose,state,position,user_paused,eligible,size,sha256,resume_requested,COALESCE((SELECT priority FROM transfer_work_priority_v1 p WHERE p.work_id=transfer_work_v1.work_id),1)";
void bindKey(QSqlQuery &q, const dsfm::TransferKey &key) {
    q.addBindValue(key.local_scope.toRfc4122()); q.addBindValue(key.remote_scope.toRfc4122());
    q.addBindValue(key.transfer.toRfc4122()); q.addBindValue(int(key.side));
}
constexpr auto whereKey = "local_scope=? AND remote_scope=? AND transfer_id=? AND side=?";
Error sqlFailure(const char *stage, const QSqlQuery &q) {
    // Deliberately exclude query text, bound values and driver error text.
    LFLOG_TRACE << "Transfer work SQL failure stage=" << stage
                << " native-code=" << q.lastError().nativeErrorCode().toStdString();
    return Error{ErrorCode::IoError};
}
TransferWork read(const QSqlQuery &q) {
    TransferWork w;
    w.id = QUuid::fromRfc4122(q.value(0).toByteArray());
    w.key = {QUuid::fromRfc4122(q.value(1).toByteArray()), QUuid::fromRfc4122(q.value(2).toByteArray()),
             QUuid::fromRfc4122(q.value(3).toByteArray()), dsfm::TransferSide(q.value(4).toInt())};
    w.file_id=q.value(5).toInt(); w.purpose=TransferPurpose(q.value(6).toInt());
    w.state=WorkState(q.value(7).toInt()); w.position=q.value(8).toLongLong();
    w.user_paused=q.value(9).toBool(); w.eligible=q.value(10).toBool();
    w.size=q.value(11).toULongLong(); w.sha256=q.value(12).toByteArray(); w.resume_requested=q.value(13).toBool(); w.priority=TransferPriority(q.value(14).toInt()); return w;
}
bool valid(const dsfm::TransferKey &k) {
    return !k.local_scope.isNull() && !k.remote_scope.isNull() && !k.transfer.isNull() &&
        (k.side==dsfm::TransferSide::Sender || k.side==dsfm::TransferSide::Receiver);
}
}
TransferWorkStore::TransferWorkStore(QSqlDatabase db) : db_{std::move(db)} {}
dsfm::Result<void> TransferWorkStore::open() {
    QSqlQuery q(db_);
    // Version is explicit in the table name. Current format only; no fallback.
    if (!q.exec("CREATE TABLE IF NOT EXISTS transfer_work_v1 ("
        "work_id BLOB PRIMARY KEY NOT NULL CHECK(length(work_id)=16),"
        "local_scope BLOB NOT NULL CHECK(length(local_scope)=16),remote_scope BLOB NOT NULL CHECK(length(remote_scope)=16),"
        "transfer_id BLOB NOT NULL CHECK(length(transfer_id)=16),side INTEGER NOT NULL CHECK(side IN (0,1)),"
        "file_id INTEGER NOT NULL UNIQUE REFERENCES file(id) ON DELETE CASCADE,purpose INTEGER NOT NULL CHECK(purpose BETWEEN 0 AND 3),"
        "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 7),position INTEGER NOT NULL CHECK(position>=0),"
        "user_paused INTEGER NOT NULL DEFAULT 0 CHECK(user_paused IN(0,1)),eligible INTEGER NOT NULL DEFAULT 0 CHECK(eligible IN(0,1)),"
        "size INTEGER NOT NULL CHECK(size>=0),sha256 BLOB NOT NULL CHECK(length(sha256)=32),"
        "resume_requested INTEGER NOT NULL DEFAULT 0 CHECK(resume_requested IN(0,1)),UNIQUE(local_scope,remote_scope,transfer_id,side))")) return sqlFailure("create-v1",q);
    if (!q.exec("CREATE INDEX IF NOT EXISTS transfer_work_order_v1 ON transfer_work_v1(position,work_id)")) return sqlFailure("index-v1",q);
    if (!q.exec("CREATE INDEX IF NOT EXISTS transfer_work_scope_v1 ON transfer_work_v1(local_scope,remote_scope,state,side,position)")) return sqlFailure("scope-index-v1",q);
    // Independent current-version local preferences; no journal or wire changes.
    if (!q.exec("CREATE TABLE IF NOT EXISTS transfer_work_priority_v1 ("
        "work_id BLOB PRIMARY KEY REFERENCES transfer_work_v1(work_id) ON DELETE CASCADE,"
        "priority INTEGER NOT NULL CHECK(priority BETWEEN 0 AND 2))")) return sqlFailure("priority-v1",q);
    // Runtime eligibility is never authority after restart. Feature/journal
    // reconciliation explicitly re-enables it; user choices survive untouched.
    if (!q.exec("UPDATE transfer_work_v1 SET eligible=0,state=CASE WHEN state=2 THEN 3 ELSE state END")) return sqlFailure("startup-fence",q);
    return {};
}
dsfm::Result<TransferWork> TransferWorkStore::find(dsfm::TransferKey key) const {
    if (!valid(key)) return Error{ErrorCode::InvalidArgument};
    QSqlQuery q(db_); q.prepare(QString("SELECT %1 FROM transfer_work_v1 WHERE %2").arg(columns,whereKey)); bindKey(q,key);
    if (!q.exec()) return sqlFailure("find",q);
    if (!q.next()) return Error{ErrorCode::MissingEntry};
    return read(q);
}
dsfm::Result<TransferWork> TransferWorkStore::enroll(int file_id, dsfm::TransferKey key, TransferPurpose purpose,
        quint64 size, QByteArray sha256, bool accepted) {
    if (file_id<=0 || !valid(key) || int(purpose)<0 || int(purpose)>3 || sha256.size()!=32 ||
        size>quint64(std::numeric_limits<qint64>::max())) return Error{ErrorCode::InvalidArgument};
    auto existing=find(key);
    if (existing) {
        const auto &w=existing.value();
        if (w.file_id!=file_id || w.size!=size || w.sha256!=sha256) return Error{ErrorCode::Conflict};
        return w; // Idempotency never changes trusted purpose or user decisions.
    }
    if (existing.error().code!=ErrorCode::MissingEntry) return existing.error();
    QSqlQuery q(db_);
    q.prepare("INSERT INTO transfer_work_v1 SELECT ?,?,?,?,?,?,?,?,COALESCE(MAX(position)+1,0),0,0,?,?,0 FROM transfer_work_v1 "
        "HAVING COUNT(*)<4096 AND (SELECT COUNT(*) FROM transfer_work_v1 WHERE local_scope=? AND remote_scope=? AND state NOT IN(4,5,6))<128");
    q.addBindValue(QUuid::createUuid().toRfc4122()); bindKey(q,key); q.addBindValue(file_id); q.addBindValue(int(purpose));
    q.addBindValue(int(accepted?WorkState::Queued:WorkState::AwaitingAcceptance));
    q.addBindValue(qlonglong(size)); q.addBindValue(sha256); q.addBindValue(key.local_scope.toRfc4122()); q.addBindValue(key.remote_scope.toRfc4122());
    if (!q.exec()) return sqlFailure("enroll",q);
    if (q.numRowsAffected()!=1) return Error{ErrorCode::QueueFull};
    LFLOG_DEBUG << "Registered durable transfer work";
    return find(key);
}
dsfm::Result<QVector<TransferWork>> TransferWorkStore::page(int offset,int limit) const {
    if (offset<0 || offset>4096 || limit<1 || limit>100) return Error{ErrorCode::InvalidArgument};
    QSqlQuery q(db_); q.prepare(QString("SELECT %1 FROM transfer_work_v1 ORDER BY position,work_id LIMIT ? OFFSET ?").arg(columns));
    q.addBindValue(limit); q.addBindValue(offset); if (!q.exec()) return sqlFailure("page",q);
    QVector<TransferWork> result; while(q.next()) result.push_back(read(q)); return result;
}
namespace {
bool validFilter(const TransferWorkFilter &f) {
    return (f.contact.isNull() || !f.identity.isNull()) && f.category>=0 && f.category<=4 && f.direction>=-1 && f.direction<=1;
}
QString viewWhere(const TransferWorkFilter &f) {
    QString where=" WHERE 1=1";
    if(!f.identity.isNull()) { where+=" AND local_scope=?"; }
    if(!f.contact.isNull()) { where+=" AND remote_scope=?"; }
    if(f.direction!=-1) { where+=" AND side=?"; }
    switch(f.category) {
    case 1: where+=" AND state=2 AND user_paused=0"; break;
    case 2: where+=" AND state=1 AND user_paused=0"; break;
    case 3: where+=" AND (state IN(0,3,7) OR (user_paused=1 AND state NOT IN(4,5,6)))"; break;
    case 4: where+=" AND state IN(4,5,6)"; break;
    }
    return where;
}
void bindView(QSqlQuery &query, const TransferWorkFilter &f) {
    if(!f.identity.isNull()) query.addBindValue(f.identity.toRfc4122());
    if(!f.contact.isNull()) query.addBindValue(f.contact.toRfc4122());
    if(f.direction!=-1) query.addBindValue(f.direction);
}
}
dsfm::Result<QVector<TransferWork>> TransferWorkStore::viewPage(TransferWorkFilter filter,int offset,int limit) const {
    if(!validFilter(filter) || offset<0 || offset>4096 || limit<1 || limit>100) return Error{ErrorCode::InvalidArgument};
    QSqlQuery q(db_);
    const auto where=viewWhere(filter);
    // This is the durable class/priority order, not a promise about live fairness
    // or contact capacity. The scheduler remains the sole admission authority.
    q.prepare(QString("SELECT %1 FROM transfer_work_v1").arg(columns)+where+
        " ORDER BY CASE purpose WHEN 2 THEN 1 WHEN 3 THEN 2 ELSE 0 END,15,position,work_id LIMIT ? OFFSET ?");
    bindView(q,filter); q.addBindValue(limit); q.addBindValue(offset);
    if(!q.exec()) return sqlFailure("view-page",q);
    QVector<TransferWork> result; while(q.next()) result.push_back(read(q)); return result;
}
dsfm::Result<TransferWorkCounts> TransferWorkStore::viewCounts(TransferWorkFilter scope) const {
    if(!validFilter(scope)) return Error{ErrorCode::InvalidArgument};
    scope.category=0; scope.direction=-1;
    QSqlQuery q(db_);
    q.prepare("SELECT state,user_paused,side,COUNT(*) FROM transfer_work_v1"+viewWhere(scope)+" GROUP BY state,user_paused,side");
    bindView(q,scope); if(!q.exec()) return sqlFailure("view-counts",q);
    TransferWorkCounts c;
    while(q.next()) {
        const int state=q.value(0).toInt(), n=q.value(3).toInt(); c.total+=n;
        if(q.value(2).toInt()==int(dsfm::TransferSide::Sender)) c.sending+=n; else c.receiving+=n;
        if(state==4 || state==5 || state==6) c.completed+=n;
        else if(q.value(1).toBool() || state==0 || state==3 || state==7) c.attention+=n;
        else if(state==2) c.active+=n; else c.queued+=n;
    }
    return c;
}
dsfm::Result<void> TransferWorkStore::project(dsfm::TransferKey key,WorkState state,bool eligible) {
    if (!valid(key) || int(state)<0 || int(state)>7) return Error{ErrorCode::InvalidArgument};
    QSqlQuery q(db_); q.prepare(QString("UPDATE transfer_work_v1 SET state=?,eligible=? AND user_paused=0,resume_requested=CASE WHEN ?=2 THEN 0 ELSE resume_requested END WHERE %1").arg(whereKey));
    q.addBindValue(int(state)); q.addBindValue(eligible); q.addBindValue(int(state)); bindKey(q,key);
    if (!q.exec()) return sqlFailure("project",q);
    if (q.numRowsAffected()!=1) return Error{ErrorCode::MissingEntry}; return {};
}
dsfm::Result<void> TransferWorkStore::accept(dsfm::TransferKey key) {
    auto w=find(key); if (!w) return w.error();
    if (w.value().state!=WorkState::AwaitingAcceptance) return {};
    return project(key,WorkState::Queued,true);
}
dsfm::Result<void> TransferWorkStore::classify(dsfm::TransferKey key,TransferPurpose purpose) {
    auto w=find(key); if(!w) return w.error();
    if(int(purpose)<0 || int(purpose)>3) return Error{ErrorCode::InvalidArgument};
    if(w.value().purpose==purpose) return {};
    if(w.value().state!=WorkState::AwaitingAcceptance) return Error{ErrorCode::Conflict};
    QSqlQuery q(db_); q.prepare(QString("UPDATE transfer_work_v1 SET purpose=? WHERE %1").arg(whereKey));
    q.addBindValue(int(purpose)); bindKey(q,key); if(!q.exec()) return sqlFailure("trusted-purpose",q); return {};
}
dsfm::Result<void> TransferWorkStore::setPaused(dsfm::TransferKey key,bool paused) {
    auto w=find(key); if (!w) return w.error();
    if (w.value().state==WorkState::Active) return Error{ErrorCode::Busy};
    if (w.value().state!=WorkState::Queued && w.value().state!=WorkState::Paused) return Error{ErrorCode::InvalidArgument};
    QSqlQuery q(db_); q.prepare(QString("UPDATE transfer_work_v1 SET user_paused=?,resume_requested=?,eligible=0 WHERE %1").arg(whereKey));
    q.addBindValue(paused); q.addBindValue(!paused); bindKey(q,key); if (!q.exec()) return sqlFailure("user-pause",q); return {};
}
dsfm::Result<void> TransferWorkStore::resumeAfterPreemption(dsfm::TransferKey key, bool enabled) {
    // Durable automatic preference, separate from an explicit user pause.
    QSqlQuery q(db_);
    q.prepare(QString("UPDATE transfer_work_v1 SET resume_requested=? WHERE %1 AND (?=0 OR (user_paused=0 AND state=2))").arg(whereKey));
    q.addBindValue(enabled); bindKey(q,key); q.addBindValue(enabled);
    if (!q.exec()) return sqlFailure("preemption-resume",q);
    return {};
}
dsfm::Result<void> TransferWorkStore::setPriority(dsfm::TransferKey key, TransferPriority priority) {
    if (int(priority)<0 || int(priority)>2) return Error{ErrorCode::InvalidArgument};
    auto w=find(key); if(!w) return w.error();
    if (w.value().state==WorkState::Completed || w.value().state==WorkState::Failed || w.value().state==WorkState::Cancelled)
        return Error{ErrorCode::InvalidArgument};
    QSqlQuery q(db_);
    q.prepare("INSERT INTO transfer_work_priority_v1(work_id,priority) VALUES(?,?) "
              "ON CONFLICT(work_id) DO UPDATE SET priority=excluded.priority");
    q.addBindValue(w.value().id.toRfc4122()); q.addBindValue(int(priority));
    if(!q.exec()) return sqlFailure("priority-save",q);
    LFLOG_DEBUG << "Updated local transfer priority";
    return {};
}
dsfm::Result<void> TransferWorkStore::reorder(dsfm::TransferKey key,dsfm::TransferKey before) {
    auto w=find(key), target=find(before); if (!w) return w.error(); if (!target) return target.error();
    if (key==before) return {};
    // Reorder is local to one identity/contact and scheduling class. Moving in
    // a filtered view must never shift hidden contacts or cross class policy.
    const auto same_scope=[&](const TransferWork &item) {
        return item.key.local_scope==key.local_scope && item.key.remote_scope==key.remote_scope &&
            schedulingClass(item.purpose)==schedulingClass(w.value().purpose) && item.priority==w.value().priority;
    };
    const auto movable=[](const TransferWork &item) {
        return item.state==WorkState::Queued || item.state==WorkState::Paused;
    };
    if (!same_scope(target.value()) || !movable(w.value()) || !movable(target.value())) return Error{ErrorCode::InvalidArgument};
    if (!db_.transaction()) return Error{ErrorCode::IoError};
    QSqlQuery q(db_);
    // At most 4096 rows. Reuse only visible-scope positions, preserving hidden rows.
    q.prepare(QString("SELECT %1 FROM transfer_work_v1 ORDER BY position,work_id").arg(columns));
    if (!q.exec()) { db_.rollback(); return sqlFailure("reorder-read",q); }
    QVector<QUuid> ids; QVector<qint64> positions;
    while(q.next()) { auto item=read(q); if(same_scope(item) && movable(item)) {
        positions.push_back(item.position); if(item.id!=w.value().id) ids.push_back(item.id);
    } }
    q.finish(); const auto at=ids.indexOf(target.value().id); ids.insert(at,w.value().id);
    q.prepare("UPDATE transfer_work_v1 SET position=? WHERE work_id=?");
    for(int i=0;i<ids.size();++i) {
        q.bindValue(0,positions[i]); q.bindValue(1,ids[i].toRfc4122());
        if(!q.exec()) { db_.rollback(); return sqlFailure("reorder-write",q); }
    }
    if(!db_.commit()) { db_.rollback(); return Error{ErrorCode::IoError}; } return {};
}
dsfm::Result<void> TransferWorkStore::reconcile(dsfm::TransferKey key,const dsfm::DurableTransferProgress &journal) {
    auto w=find(key); if(!w) return w.error();
    if(journal.description.key!=key || journal.description.side!=key.side || journal.description.size!=w.value().size || journal.description.sha256!=w.value().sha256)
        return project(key,WorkState::Attention,false);
    if(journal.failure!=dsfm::TransferFailureReason::None) return project(key,WorkState::Failed,false);
    if(journal.complete) return project(key,WorkState::Completed,false);
    if(w.value().state==WorkState::Cancelled) return project(key,WorkState::Cancelled,false);
    // Journal pause/recovery evidence cannot override explicit user pause.
    return project(key,WorkState::Paused,false);
}
} // namespace ds::core
