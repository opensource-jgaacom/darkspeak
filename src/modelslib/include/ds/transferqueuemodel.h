#pragma once

#include <ds/transfermanager.h>
#include <QAbstractListModel>
#include <QVariantMap>
#include <functional>

namespace ds::models {
// Local presentation only: no File QObject, native path or authorization handle
// crosses into QML. Metadata reads never accept, start or restore a transfer.
class TransferQueueModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString scopeLabel READ scopeLabel NOTIFY scopeChanged)
    Q_PROPERTY(QUuid identityId READ identityId NOTIFY scopeChanged)
    Q_PROPERTY(QUuid contactId READ contactId NOTIFY scopeChanged)
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY countsChanged)
    Q_PROPERTY(QVariantMap applicationCounts READ applicationCounts NOTIFY countsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int category READ category WRITE setCategory NOTIFY filtersChanged)
    Q_PROPERTY(int direction READ direction WRITE setDirection NOTIFY filtersChanged)
public:
    struct Metadata {
        QString title, identity, contact, reason;
        quint64 provisional = 0, verified = 0;
        bool journal_known = false, can_accept = false;
    };
    using Reader = std::function<QCoro::Task<Metadata>(core::TransferWork)>;
    using Decision = std::function<dsfm::Result<void>(core::TransferWork,bool)>;
    TransferQueueModel(core::TransferManager &manager, Reader reader, Decision decision = {}, QObject *parent = nullptr);
    ~TransferQueueModel() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int,QByteArray> roleNames() const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;
    Q_INVOKABLE bool setScope(QUuid identity = {}, QUuid contact = {}, QString identity_name = {}, QString contact_name = {});
    Q_INVOKABLE void command(QUuid work, QString action, int value = 0);
    Q_INVOKABLE void refresh();
    QString scopeLabel() const { return scope_label_; }
    QUuid identityId() const { return filter_.identity; }
    QUuid contactId() const { return filter_.contact; }
    QVariantMap counts() const { return counts_; }
    QVariantMap applicationCounts() const { return application_counts_; }
    bool busy() const { return applying_; }
    QString error() const { return error_; }
    int category() const { return filter_.category; }
    int direction() const { return filter_.direction; }
    void setCategory(int category);
    void setDirection(int direction);
signals:
    void scopeChanged();
    void filtersChanged();
    void countsChanged();
    void busyChanged();
    void errorChanged();
    void commandFinished(QUuid work, int error_code);
private:
    struct Row { core::TransferWork work; QVariantMap display; };
    QCoro::Task<void> reload();
    void invalidate();
    void publish(QVector<Row> rows);
    void setError(QString error);
    QPointer<core::TransferManager> manager_;
    Reader reader_;
    Decision decision_;
    core::TransferWorkFilter filter_;
    QVector<Row> rows_;
    QVariantMap counts_, application_counts_;
    QString scope_label_ = tr("All transfers"), error_;
    QTimer timer_;
    quint64 generation_ = 0;
    int limit_ = 100;
    bool loading_ = false, again_ = false, more_ = false, applying_ = false;
};

TransferQueueModel::Reader applicationTransferReader(core::TransferManager &manager);
TransferQueueModel::Decision applicationTransferDecision(core::TransferManager &manager);
}
