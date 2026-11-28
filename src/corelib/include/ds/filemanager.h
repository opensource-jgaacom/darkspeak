#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <set>
#include <deque>
#include <QUuid>
#include <QObject>
#include <QSettings>
#include <QDeadlineTimer>

#include "ds/conversation.h"
#include "ds/identity.h"
#include "ds/file.h"
#include "ds/registry.h"
#include "ds/lru_cache.h"
#include "ds/managedstorage.h"

namespace ds {
namespace core {

class FileManager : public QObject
{
    Q_OBJECT
public:
    explicit FileManager(QObject &parent, QSettings& settings);

    ManagedStorage& storage() { return storage_; }
    File::ptr_t getFile(const int dbId);
    File::ptr_t getFile(const QByteArray& hash, Conversation& conversation);
    File::ptr_t getFileFromId(const QByteArray& fileId, Conversation& conversation);
    File::ptr_t getFileFromId(const QByteArray& fileId, const File::Direction direction);
    File::ptr_t getFileFromId(const QByteArray& fileId, const Contact& contact);
    File::ptr_t getFileFromId(const QByteArray& fileId, const Contact& contact, File::Direction direction);
    File::ptr_t getFileFromPeer(const QByteArray& fileId, QUuid identity, const QByteArray& peer_hash,
                              File::Direction direction);

    File::ptr_t addFile(std::unique_ptr<FileData> data, std::optional<dsfm::TransferAttempt> attempt = {});
    static QString transferName(QString logical_name);
    QCoro::Task<dsfm::Result<File::ptr_t>> addVfsTransfer(Conversation& conversation, dsvfs::Vfs& vfs, dsvfs::NodeId node,
                                                       dsvfs::Request request, QString mime_type = "application/octet-stream");
    // Bind an already authorized/prepared VFS source without offering it yet.
    dsfm::Result<File::ptr_t> addPreparedVfsTransfer(Conversation& conversation, dsvfs::Vfs& vfs,
        dsvfs::NodeId node, dsvfs::Request request, dsvfs::PreparedRead prepared,
        QString mime_type = "application/octet-stream", bool enqueue = false);
    dsfm::Result<File::ptr_t> addCatalogTransfer(Conversation& conversation, std::shared_ptr<dsvfs::CatalogExport> catalog,
                                              bool enqueue = true);

    // Validate the local policy before an incoming file is started.
    bool canAcceptIncoming(const File& file, QString& reason) const;

    // Catalogs are accepted only against a live, explicitly requested remote export.
    std::function<bool(const PeerFileOffer&)> incomingCatalogAdmission;
    void receivedFileOffer(Conversation& conversation, const PeerFileOffer& offer);
    // Explicit local GET consent only; generic File/TransferSession still owns execution.
    dsfm::Result<void> expectDownload(QUuid identity, QUuid contact, QUuid connection, QByteArray transfer,
        std::optional<quint64> size, QByteArray sha256, std::function<bool()> current);

    void touch(const File::ptr_t& file);

    void onFileStateChanged(const File *file);
    File::ptr_t thumbnailFor(const File& primary);
    QByteArray thumbnailManifest(const File& primary);

signals:
    void fileAdded(const File::ptr_t& file);
    void fileDeleted(const int dbId);
    void fileStateChanged(const core::File *file);

public slots:

private:
    struct DownloadConsent {
        QUuid identity, contact, connection;
        QByteArray transfer, sha256;
        std::optional<quint64> size;
        std::function<bool()> current;
        QDeadlineTimer expires{30000};
    };
    QVector<DownloadConsent> downloads_;
    bool downloads_stopped_ = false;
    void pruneDownloads();
    // -1 rejects a mismatched requested offer; 0 ordinary offer; 1 explicit consent.
    int downloadConsent(Conversation &conversation, const PeerFileOffer &offer);
    void importOutgoingFile(const File::ptr_t& file);
    QCoro::Task<void> prepareOutgoingOffer(File::ptr_t file);

    Registry<int, File> registry_;
    LruCache<File::ptr_t> lru_cache_{3};
    //std::set<File::ptr_t> hashing_;
    QSettings &settings_;
    ManagedStorage storage_;
};

}}

#endif // FILEMANAGER_H
