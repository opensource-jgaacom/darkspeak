#pragma once
#include <ds/filesharecodec.h>
#include <dsvfs/vfs.h>
#include <QDeadlineTimer>
#include <QObject>
#include <QTimer>

namespace ds::core { class FileManager; class Contact; class File; }
namespace ds::prot {
class Peer;
class FileShareService;
// Correlation and validated metadata cache only. Generic transfers own all bytes,
// checkpoint state and storage; no catalog is visible until whole-file validation.
class RemoteCatalogs final : public QObject {
    Q_OBJECT
public:
    struct Context {
        QUuid identity, contact, connection;
        bool allow_utf8 = false;
        std::function<bool()> current;
        std::function<quint64(protocol::Envelope)> send;
    };
    struct Cached {
        dsfm::NamespaceId namespace_id;
        dsfm::FileId file;
        quint64 size = 0;
        QByteArray sha256;
    };
    // Release the associated generic file row/artifact on failure or eviction.
    using Discard = std::function<void(QUuid, QUuid, QByteArray)>;
    explicit RemoteCatalogs(Discard discard = {}, QObject *parent = nullptr);
    ~RemoteCatalogs() override;
    void install(core::FileManager &files, FileShareService &service);
    dsfm::Result<quint64> requestContact(std::shared_ptr<Peer> peer, std::shared_ptr<core::Contact> contact, QString virtual_path);
    dsfm::Result<quint64> request(Context context, QString virtual_path);
    void response(QUuid connection, const protocol::Envelope &response);
    void cancel(QUuid connection, quint64 request);
    bool admit(QUuid identity, QUuid contact, QUuid connection, QByteArray transfer,
               QString mime, QString name, quint64 size, QByteArray sha256);
    QCoro::Task<dsfm::Result<void>> complete(QUuid identity, QUuid contact, QUuid connection,
        QByteArray transfer, Cached artifact, std::shared_ptr<dsfm::IFileReader> reader, dsvfs::Vfs &validator);
    std::optional<Cached> cached(QUuid identity, QUuid contact, QString virtual_path);
    void failed(QUuid identity, QUuid contact, QByteArray transfer);
    void stop();
signals:
    void catalogReady(QUuid identity, QUuid contact, QString virtual_path);
private:
    struct Item {
        Context context;
        QString path;
        quint64 request = 0;
        QByteArray transfer, digest;
        quint64 size = 0;
        bool offered = false, validating = false;
        dsfm::CancellationToken cancel;
        QDeadlineTimer expires{30000};
        std::optional<Cached> cache;
    };
    QCoro::Task<void> completedFile(core::FileManager &files, std::shared_ptr<core::File> file);
    void prune();
    void discard(const Item &item);
    QVector<std::shared_ptr<Item>> items_;
    Discard discard_;
    QTimer timer_;
    bool stopped_ = false;
};
}
