#pragma once

#include "ds/transfersession.h"
#include "ds/transferwork.h"
#include <QPointer>
#include <QElapsedTimer>
#include <map>
#include <tuple>

namespace ds::core {

// Admission lease for sessions and bounded startup journal reconciliation.
// It remains occupied until the owner has joined and settled its work.
class TransferReservation {
public:
    ~TransferReservation();
private:
    friend class TransferManager;
    struct Budget;
    TransferReservation(std::shared_ptr<Budget> budget, dsfm::TransferKey key);
    std::shared_ptr<Budget> budget_;
    dsfm::TransferKey key_;
};

class File;

class TransferManager : public QObject {
    Q_OBJECT
public:
    explicit TransferManager(QObject *parent = nullptr);
    ~TransferManager() override;
    dsfm::Result<void> openWorkStore(QSqlDatabase db);
    TransferWorkStore *workStore() const { return work_store_.get(); }
    dsfm::Result<void> bindWork(std::shared_ptr<File> file, std::optional<TransferPurpose> purpose = {});
    dsfm::Result<void> acceptWork(File &file);
    dsfm::Result<void> pauseQueuedWork(dsfm::TransferKey key, bool paused);
    dsfm::Result<void> reorderWork(dsfm::TransferKey key, dsfm::TransferKey before);
    dsfm::Result<void> setWorkPriority(dsfm::TransferKey key, TransferPriority priority);
    dsfm::Result<void> cancelWork(dsfm::TransferKey key);
    std::shared_ptr<File> boundWorkFile(int file_id) const;
    void dispatchWork();
    void restoreWork();
    void forgetWork(int file_id);
    dsfm::Result<std::shared_ptr<TransferReservation>> reserve(const dsfm::TransferKey &key);
    size_t active(QUuid local_scope, QUuid remote_scope) const;
    dsfm::Result<void> submit(std::shared_ptr<TransferSession> session);
    std::shared_ptr<TransferSession> find(const dsfm::TransferKey &key) const;
    std::shared_ptr<TransferSession> findLocalFile(int local_file) const;
    QCoro::Task<void> shutdown();
    void retireConnection(QUuid connection);

signals:
    void admissionChanged();
    void attemptFinished(dsfm::TransferKey key);
    void workChanged();

private:
    friend class TransferReservation;
    friend class TransferSession;
    using Key = std::tuple<QUuid, QUuid, QUuid, dsfm::TransferSide>;
    static Key index(const dsfm::TransferKey &key);
    struct Entry {
        std::shared_ptr<TransferSession> session;
        std::shared_ptr<TransferReservation> reservation;
        std::optional<QCoro::Task<void>> root;
        quint64 waiting_since = 0;
        qint64 started_at = 0;
    };
    struct Registry;
    static QCoro::Task<void> run(std::shared_ptr<Registry> registry, Key key, QPointer<TransferManager> manager);
    dsfm::Result<std::shared_ptr<TransferReservation>> reserveActive(const dsfm::TransferKey &key);
    void schedule();
    void preemptForConversation();
    void park(const dsfm::TransferKey &key);
    QCoro::Task<void> acquire(const dsfm::TransferKey &key);
    std::shared_ptr<TransferReservation::Budget> budget_;
    std::shared_ptr<Registry> registry_;
    std::unique_ptr<TransferWorkStore> work_store_;
    struct WorkBinding {
        std::shared_ptr<File> file;
        QMetaObject::Connection state, recovery;
    };
    std::map<int, WorkBinding> work_files_;
    bool schedule_wakeup_ = false;
    void requestSchedule();
    bool work_wakeup_ = false;
    bool dispatch_running_ = false;
    bool dispatch_again_ = false;
    void requestWorkDispatch();
    void projectWork(File &file);
    bool budgetClosing() const;
    bool attemptStarted(const dsfm::TransferKey &key) const;
    bool restore_started_ = false;
    void restorePage(int after, int count);
    QVector<TransferWork> dispatch_candidates_;
    std::map<QUuid,quint64> waiting_round_;
    quint64 dispatch_round_ = 0;
    QElapsedTimer scheduling_clock_;
    std::map<Key,qint64> preemption_after_;
    bool preemption_wakeup_ = false;
    void dispatchPage(int offset);
    void dispatchCandidates(int offset);

};

} // namespace ds::core
