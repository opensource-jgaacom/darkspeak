#pragma once
#include <dsvfs/vfs.h>
#include <QVariantList>

namespace ds::core {
// Trusted local administration. Never supplies authorization to a peer.
class ShareMounts final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY changed)
    Q_PROPERTY(QVariantList assignments READ assignments NOTIFY changed)
    Q_PROPERTY(QVariantList mounts READ mounts NOTIFY changed)
    Q_PROPERTY(QVariantList directories READ directories NOTIFY changed)
    Q_PROPERTY(QVariantList preview READ preview NOTIFY changed)
    Q_PROPERTY(QString directoryName READ directoryName NOTIFY changed)
    Q_PROPERTY(bool moreDirectories READ moreDirectories NOTIFY changed)
    Q_PROPERTY(bool morePreview READ morePreview NOTIFY changed)
    Q_PROPERTY(QString draftPath MEMBER draft_path_ NOTIFY draftChanged)
public:
    struct Host {
        std::function<std::shared_ptr<void>()> hold;
        std::function<bool(QUuid)> identityExists;
        std::function<QCoro::Task<dsfm::Result<dsvfs::Vfs*>>()> open;
        std::function<dsfm::IFileManager*()> backend;
    };
    ShareMounts(Host host, QObject *parent = nullptr);
    Q_INVOKABLE void selectIdentity(QUuid identity);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void selectSource(QString ns);
    Q_INVOKABLE void enterDirectory(QString id);
    Q_INVOKABLE void up();
    Q_INVOKABLE void nextDirectories();
    Q_INVOKABLE void assign();
    Q_INVOKABLE void unassign(QString id);
    Q_INVOKABLE void addMount(QString assignment);
    Q_INVOKABLE void moveMount(QString id);
    Q_INVOKABLE void removeMount(QString id);
    Q_INVOKABLE void browse(QString id);
    Q_INVOKABLE void nextPreview();
    void stop() { stopping_ = true; }
    bool busy() const { return busy_; }
    QString error() const { return error_; }
    QVariantList sources() const { return sources_; }
    QVariantList assignments() const { return assignments_; }
    QVariantList mounts() const { return mounts_; }
    QVariantList directories() const { return directories_; }
    QVariantList preview() const { return preview_; }
    QString directoryName() const { return directory_name_; }
    bool moreDirectories() const { return !directory_cursor_.isEmpty(); }
    bool morePreview() const { return !preview_cursor_.isEmpty(); }
    enum Command { Refresh, SelectSource, EnterDirectory, Up, NextDirectories, Assign,
                   Unassign, AddMount, MoveMount, RemoveMount, Browse, NextPreview };
    QCoro::Task<void> run(Command operation, QString id = {});
signals:
    void changed();
    void draftChanged();
private:
    dsvfs::Request request(bool inspect = false) const;
    QCoro::Task<dsfm::Result<void>> validatePlacement(dsvfs::Vfs &vfs, QString path, QUuid moving = {});
    QCoro::Task<dsfm::Result<void>> reload(dsvfs::Vfs &vfs);
    QCoro::Task<dsfm::Result<void>> directoryPage(QByteArray cursor = {});
    QCoro::Task<dsfm::Result<void>> previewPage(dsvfs::Vfs &vfs, QByteArray cursor = {});
    Host host_;
    QUuid identity_, namespace_, directory_;
    QVector<QPair<QUuid, QString>> ancestors_;
    QByteArray directory_cursor_, preview_cursor_, preview_node_;
    QString directory_name_, draft_path_ = QStringLiteral("/share"), error_;
    QVariantList sources_, assignments_, mounts_, directories_, preview_;
    bool busy_ = false, stopping_ = false;
};
}
