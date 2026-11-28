#include <ds/transferqueuemodel.h>
#include <ds/dsengine.h>
#include <ds/filemanager.h>
#include <ds/textpolicy.h>
#include <QSqlQuery>
#include <QSqlError>
#include <logfault/logfault.h>

namespace ds::models {
using namespace core;
TransferQueueModel::Reader applicationTransferReader(TransferManager &manager) {
    return [manager=QPointer(&manager)](TransferWork w)->QCoro::Task<TransferQueueModel::Metadata> {
        TransferQueueModel::Metadata result;
        if(!manager) co_return result;
        // Read only display fields, with both ownership relations checked. Do
        // not load File: loading can trigger recovery and is not a UI action.
        QSqlQuery q(DsEngine::instance().getDb());
        q.prepare("SELECT f.name,i.name,COALESCE(NULLIF(c.name,''),c.nickname) FROM file f "
                  "JOIN identity i ON i.id=f.identity_id JOIN contact c ON c.id=f.contact_id AND c.identity=i.id "
                  "WHERE f.id=? AND i.uuid=? AND c.uuid=?");
        q.addBindValue(w.file_id); q.addBindValue(w.key.local_scope); q.addBindValue(w.key.remote_scope);
        if(!q.exec()) {
            LFLOG_TRACE << "Transfer view SQL failure stage=display native-code=" << q.lastError().nativeErrorCode().toStdString();
            co_return result;
        }
        if(!q.next()) co_return result;
        result.title=q.value(0).toString();
        if(w.key.side==dsfm::TransferSide::Receiver) {
            const auto identity=DsEngine::instance().getIdentityManager()->identityFromUuid(w.key.local_scope);
            if(!identity || (!identity->getAllowUtf8() && !ds::core::text_policy::permittedAscii(result.title,false)))
                result.title=TransferQueueModel::tr("Filename hidden by text security preference");
        }
        result.title=FileManager::transferName(result.title); result.identity=q.value(1).toString(); result.contact=q.value(2).toString(); q.finish();
        if(auto file=manager->boundWorkFile(w.file_id)) {
            result.provisional=quint64(qMax<qlonglong>(0,file->getBytesTransferred()));
            result.reason=file->pauseReasonText();
            QString reason;
            result.can_accept=file->getDirection()==File::INCOMING && DsEngine::instance().getFileManager()->canAcceptIncoming(*file,reason);
        }
        auto &storage=DsEngine::instance().getFileManager()->storage();
        if (auto session=manager->find(w.key); session && session->pausingForPriority())
            result.reason=TransferQueueModel::tr("Pausing for higher-priority transfer");
        auto held=storage.holdOperation();
        if(!co_await storage.ready()) co_return result;
        auto saved=co_await storage.transferProgress().load(w.key);
        if(!manager) co_return result;
        // Immutable content identity must match; a stale or corrupt mirror is
        // never presented as verified evidence.
        if(saved && saved.value().description.key==w.key && saved.value().description.size==w.size && saved.value().description.sha256==w.sha256) {
            result.journal_known=true;
            result.verified=saved.value().verified_prefix;
            result.provisional=std::max(result.provisional,saved.value().confirmed_offset);
            if(saved.value().failure!=dsfm::TransferFailureReason::None)
                result.reason=TransferQueueModel::tr("Integrity or transfer failure (%1)").arg(int(saved.value().failure));
        }
        co_return result;
    };
}
TransferQueueModel::Decision applicationTransferDecision(TransferManager &manager) {
    return [manager=QPointer(&manager)](TransferWork w,bool accept)->dsfm::Result<void> {
        if(!manager) return dsfm::Error{dsfm::ErrorCode::ShuttingDown};
        auto file=manager->boundWorkFile(w.file_id);
        if(!file || !file->checkpointKey() || file->checkpointKey().value()!=w.key || file->getState()!=File::FS_OFFERED)
            return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
        if(accept) {
            QString reason;
            if(!DsEngine::instance().getFileManager()->canAcceptIncoming(*file,reason)) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
            file->accept();
        } else file->reject();
        return {};
    };
}
}
