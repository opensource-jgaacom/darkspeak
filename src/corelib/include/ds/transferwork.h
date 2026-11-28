#pragma once

#include <dsfilemanager/transfercheckpoint.h>
#include <QSqlDatabase>
#include <optional>

namespace ds::core {

// Trusted local adapters choose purpose. Neither wire metadata nor MIME does.
enum class TransferPurpose { Attachment, Preview, Catalog, FileShare };
enum class TransferClass { Conversation, Utility, Bulk };
enum class TransferPriority { High, Normal, Low };
constexpr TransferClass schedulingClass(TransferPurpose purpose) {
    switch (purpose) {
    case TransferPurpose::Attachment: case TransferPurpose::Preview: return TransferClass::Conversation;
    case TransferPurpose::Catalog: return TransferClass::Utility;
    case TransferPurpose::FileShare: return TransferClass::Bulk;
    }
    return TransferClass::Bulk;
}
enum class WorkState { AwaitingAcceptance, Queued, Active, Paused, Completed, Failed, Cancelled, Attention };
struct TransferWork {
    QUuid id;
    dsfm::TransferKey key;
    int file_id = 0;
    TransferPurpose purpose = TransferPurpose::Attachment;
    WorkState state = WorkState::Queued;
    qint64 position = 0;
    TransferPriority priority = TransferPriority::Normal;
    bool user_paused = false;
    bool eligible = false;
    bool resume_requested = false;
    quint64 size = 0;
    QByteArray sha256;
};

// Local operator view filters. Contact is always nested within a local identity.
struct TransferWorkFilter {
    QUuid identity, contact;
    int category = 0; // All, Active, Queued, Paused/attention, Completed/history
    int direction = -1; // -1 both, otherwise TransferSide
};
struct TransferWorkCounts {
    int total = 0, active = 0, queued = 0, attention = 0, completed = 0, sending = 0, receiving = 0;
};

// A bounded, private application-DB projection. It never writes the journal,
// owns an I/O lease or reconstructs native paths from peer metadata.
class TransferWorkStore {
public:
    explicit TransferWorkStore(QSqlDatabase db);
    dsfm::Result<void> open();
    dsfm::Result<TransferWork> enroll(int file_id, dsfm::TransferKey key, TransferPurpose purpose,
                                    quint64 size, QByteArray sha256, bool accepted);
    dsfm::Result<TransferWork> find(dsfm::TransferKey key) const;
    dsfm::Result<QVector<TransferWork>> page(int offset = 0, int limit = 100) const;
    dsfm::Result<QVector<TransferWork>> viewPage(TransferWorkFilter filter, int offset, int limit = 100) const;
    dsfm::Result<TransferWorkCounts> viewCounts(TransferWorkFilter scope) const;
    dsfm::Result<void> project(dsfm::TransferKey key, WorkState state, bool eligible);
    dsfm::Result<void> accept(dsfm::TransferKey key);
    dsfm::Result<void> classify(dsfm::TransferKey key, TransferPurpose purpose);
    dsfm::Result<void> setPaused(dsfm::TransferKey key, bool paused);
    dsfm::Result<void> resumeAfterPreemption(dsfm::TransferKey key, bool enabled = true);
    dsfm::Result<void> setPriority(dsfm::TransferKey key, TransferPriority priority);
    dsfm::Result<void> reorder(dsfm::TransferKey key, dsfm::TransferKey before);
    dsfm::Result<void> reconcile(dsfm::TransferKey key,
                                const dsfm::DurableTransferProgress &journal);
private:
    QSqlDatabase db_;
};

} // namespace ds::core
