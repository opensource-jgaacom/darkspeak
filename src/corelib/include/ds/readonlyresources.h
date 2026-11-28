#pragma once

#include <dsfilemanager/readonlyfilemanager.h>
#include <QAbstractListModel>
#include <QTimer>

namespace ds::core {
// Local administration only. The host supplies the shared backend and its
// mandatory private-path exclusions; this model never grants VFS access.
class ReadOnlyResources final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(int errorCode READ errorCode NOTIFY stateChanged)
    Q_PROPERTY(QString draftName MEMBER draft_name_ NOTIFY draftChanged)
    Q_PROPERTY(QString draftPath MEMBER draft_path_ NOTIFY draftChanged)
    Q_PROPERTY(QString draftExclusions MEMBER draft_exclusions_ NOTIFY draftChanged)
    Q_PROPERTY(int draftSymlinks MEMBER draft_symlinks_ NOTIFY draftChanged)
    Q_PROPERTY(int draftVerification MEMBER draft_verification_ NOTIFY draftChanged)
public:
    struct Host {
        std::function<std::shared_ptr<void>()> hold;
        std::function<QCoro::Task<dsfm::Result<dsfm::NamespaceInfo>>(dsfm::ReadOnlyNamespaceConfig, bool)> open;
        std::function<QCoro::Task<dsfm::Result<void>>(dsfm::NamespaceId)> close;
        std::function<QCoro::Task<dsfm::Result<void>>(dsfm::NamespaceId)> remove;
        std::function<QCoro::Task<dsfm::Result<dsfm::ReadOnlyNamespaceConfig>>(dsfm::NamespaceId)> config;
        std::function<QCoro::Task<dsfm::Result<dsfm::NamespaceInfo>>(dsfm::NamespaceId)> info;
        std::function<QCoro::Task<dsfm::Result<dsfm::ReadOnlySourceUsage>>(dsfm::NamespaceId)> usage;
    };
    enum Role { ResourceId = Qt::UserRole + 1, Name, Path, Status, Problem, Symlinks, Verification, Exclusions, FileCount, Size };
    ReadOnlyResources(QString registry, Host host, QObject *parent = nullptr);
    ~ReadOnlyResources() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool busy() const { return (busy_ && !silent_inspection_) || bool(pending_command_); }
    QString error() const { return error_; }
    int errorCode() const { return error_code_; }
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void add();
    Q_INVOKABLE void remove(QString id);
    Q_INVOKABLE void discardDraft();
    Q_INVOKABLE void rename(QString id, QString name);
    QCoro::Task<void> renameSource(QUuid id, QString name);
    void stop();
    QCoro::Task<void> load();
    QCoro::Task<void> create();
    QCoro::Task<void> erase(QUuid id);
signals:
    void stateChanged();
    void draftChanged();
private:
    struct Source {
        QString name;
        dsfm::ReadOnlyNamespaceConfig config;
        QString status = QStringLiteral("Unavailable");
        QString problem;
        QString effective_exclusions;
        bool opened = false;
        std::optional<dsfm::ReadOnlySourceUsage> usage;
    };
    QCoro::Task<void> inspect(bool background = false);
    void runCommand(std::function<void()> command);
    QCoro::Task<dsfm::Result<void>> save(const QVector<Source> &sources);
    void fail(dsfm::Error error, QString message);
    bool begin(bool background = false);
    void end();
    QString registry_;
    Host host_;
    QVector<Source> sources_;
    QTimer timer_;
    bool loaded_ = false;
    bool busy_ = false;
    bool silent_inspection_ = false;
    std::function<void()> pending_command_;
    bool failed_load_ = false;
    bool stopping_ = false;
    QString error_;
    int error_code_ = -1;
    QString draft_name_, draft_path_, draft_exclusions_;
    int draft_symlinks_ = 0, draft_verification_ = 0;
};
} // namespace ds::core
