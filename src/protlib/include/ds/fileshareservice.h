#pragma once
#include <ds/filesharecodec.h>
#include <dsvfs/vfs.h>
#include <QObject>

namespace ds::core { class Contact; }
namespace ds::prot {
class Peer;
// Application controller. Peer owns only validation/correlation and transport.
class FileShareService final : public QObject {
    Q_OBJECT
public:
    struct CatalogDelivery {
        QByteArray transfer_id;
        std::function<dsfm::Result<void>()> start;
        std::function<void()> cancel;
        std::function<bool()> active; // required for bounded GET admission; weak transfer ownership
    };
    struct Host {
        std::function<std::shared_ptr<void>()> hold;
        std::function<QCoro::Task<dsfm::Result<dsvfs::Vfs*>>()> open;
        std::function<QCoro::Task<dsfm::Result<dsvfs::Request>>(QUuid, QUuid)> request;
        std::function<bool(QUuid)> enabled;
        std::function<void(QUuid, bool)> enable;
        // Explicit trusted local admission, evaluated before opening/enumerating
        // the VFS. Path is logical presentation, decoded exactly one wire layer.
        std::function<bool(QUuid, QUuid, const QString &)> approve_recursion;
    };
    struct Binding {
        QUuid identity, contact, connection;
        std::function<bool()> available; // accepted contact and local security preferences
        std::function<bool()> current; // live authenticated connection and peer preferences
        std::function<bool()> allow_utf8; // bilateral effective presentation
        std::function<dsfm::Result<CatalogDelivery>(std::shared_ptr<dsvfs::CatalogExport>)> stage_catalog;
        std::function<bool()> accepts_catalog; // current peer MIME/filename preferences
        std::function<dsfm::Result<CatalogDelivery>(dsvfs::Vfs&, dsvfs::NodeId, dsvfs::Request, dsvfs::PreparedRead)> stage_download;
    };
    struct Session;
    using SessionPtr = std::shared_ptr<Session>;
    FileShareService(Host host, QObject *parent = nullptr);
    ~FileShareService() override;
    SessionPtr bind(Binding binding);
    void attach(std::shared_ptr<Peer> peer, Binding binding, std::function<bool()> local_utf8);
    void attachContact(std::shared_ptr<Peer> peer, std::shared_ptr<core::Contact> contact);
    // An empty result means disconnected, cancelled, expired or stopped; never publish it.
    QCoro::Task<std::optional<protocol::Envelope>> handle(SessionPtr session, protocol::Envelope request);
    // Publish the staged catalog or download only after its accepted control was sent.
    dsfm::Result<void> publishCatalog(SessionPtr session, quint64 request_id);
    void discardCatalog(SessionPtr session, quint64 request_id);
    Q_INVOKABLE bool enabled(QUuid identity) const;
    Q_INVOKABLE void setEnabled(QUuid identity, bool enabled);
    void stop();
signals:
    void configurationChanged();
    void responseReceived(QUuid connection, protocol::Envelope response);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
