#pragma once
#include <dsvfs/vfs.h>
#include <ds/fileshareservice.h>
#include <QTimer>

namespace ds::prot { class Peer; }
namespace ds::core { class Contact; }
namespace ds::models {
struct BrowserScope {
    QUuid identity, contact;
    bool operator==(const BrowserScope &) const = default;
};
// Display metadata plus an opaque provider binding. Paths are logical presentation
// only; foreign wire IDs are never passed to the local VFS.
struct BrowserNode {
    QUuid provider;
    dsvfs::NodeInfo info;
    QString path;
    bool size_known = false;
};
struct BrowserPage {
    QVector<BrowserNode> entries;
    QByteArray continuation;
};
class BrowseProvider {
public:
    virtual ~BrowseProvider() = default;
    const BrowserScope &scope() const { return scope_; }
    QUuid id() const { return id_; }
    virtual bool current() const = 0;
    virtual bool cachedMetadataAllowed() const { return false; }
    // Interrupt an obsolete operation without invalidating this provider binding.
    virtual void cancelPending() {}
    virtual void cancel() = 0;
    // One current-folder subscription; remote v1 has no live notification extension.
    virtual void unwatch() {}
    virtual QCoro::Task<dsfm::Result<void>> watch(BrowserNode, dsvfs::ChangeCallback) { co_return dsfm::Result<void>{}; }
    virtual bool canDownload(const BrowserNode &) const { return false; }
    virtual QCoro::Task<dsfm::Result<QByteArray>> download(BrowserNode) { co_return dsfm::Error{dsfm::ErrorCode::UnsupportedCapability}; }
    virtual QCoro::Task<dsfm::Result<BrowserNode>> root() = 0;
    virtual QCoro::Task<dsfm::Result<BrowserNode>> stat(BrowserNode node) = 0;
    virtual QCoro::Task<dsfm::Result<BrowserNode>> resolveChild(BrowserNode parent, QString name) = 0;
    virtual QCoro::Task<dsfm::Result<BrowserNode>> resolveDirectory(QStringList route, dsfm::CancellationToken cancel);
    virtual QCoro::Task<dsfm::Result<BrowserPage>> listPage(BrowserNode directory, QByteArray cursor = {}) = 0;
protected:
    explicit BrowseProvider(BrowserScope scope) : scope_(scope) {}
    bool owns(const BrowserNode &node) const { return node.provider == id_ && !node.info.id.isEmpty(); }
    BrowserScope scope_;
    QUuid id_ = QUuid::createUuid();
};
class LocalBrowseProvider final : public BrowseProvider {
public:
    struct Host {
        std::function<std::shared_ptr<void>()> hold;
        std::function<bool(QUuid)> identity_exists;
        std::function<QCoro::Task<dsfm::Result<dsvfs::Vfs*>>()> open;
    };
    LocalBrowseProvider(QUuid identity, Host host);
    bool current() const override;
    void cancel() override;
    void unwatch() override { subscription_.reset(); ++watch_generation_; }
    QCoro::Task<dsfm::Result<void>> watch(BrowserNode directory, dsvfs::ChangeCallback callback) override;
    QCoro::Task<dsfm::Result<BrowserNode>> root() override;
    QCoro::Task<dsfm::Result<BrowserNode>> stat(BrowserNode node) override;
    QCoro::Task<dsfm::Result<BrowserNode>> resolveChild(BrowserNode parent, QString name) override;
    QCoro::Task<dsfm::Result<BrowserPage>> listPage(BrowserNode directory, QByteArray cursor = {}) override;
private:
    dsvfs::Request request() const;
    Host host_;
    dsvfs::Subscription subscription_;
    quint64 watch_generation_ = 0;
    std::shared_ptr<std::atomic<quint64>> generation_ = std::make_shared<std::atomic<quint64>>(1);
};
class RemoteBrowseProvider final : public BrowseProvider {
public:
    struct Context {
        BrowserScope scope;
        QUuid connection;
        bool allow_utf8 = false;
        std::function<bool()> current;
        std::function<quint64(protocol::Envelope)> send;
        QPointer<prot::FileShareService> responses;
        std::function<bool()> cached_metadata_allowed;
        std::function<dsfm::Result<void>(BrowserNode, QByteArray)> accept_download;
        std::function<bool()> downloads_allowed;
    };
    explicit RemoteBrowseProvider(Context context);
    static dsfm::Result<std::shared_ptr<BrowseProvider>> forContact(std::shared_ptr<prot::Peer> peer,
        std::shared_ptr<core::Contact> contact, prot::FileShareService &responses);
    bool current() const override;
    bool cachedMetadataAllowed() const override;
    bool canDownload(const BrowserNode &node) const override;
    QCoro::Task<dsfm::Result<QByteArray>> download(BrowserNode node) override;
    void cancelPending() override;
    void cancel() override;
    QCoro::Task<dsfm::Result<BrowserNode>> root() override;
    QCoro::Task<dsfm::Result<BrowserNode>> stat(BrowserNode node) override;
    QCoro::Task<dsfm::Result<BrowserNode>> resolveChild(BrowserNode parent, QString name) override;
    QCoro::Task<dsfm::Result<BrowserPage>> listPage(BrowserNode directory, QByteArray cursor = {}) override;
private:
    quint64 send(protocol::Envelope request) noexcept;
    QCoro::Task<dsfm::Result<BrowserNode>> resolveDirectory(QStringList route, dsfm::CancellationToken cancel) override;
    protocol::VirtualPath path(QString value) const;
    BrowserNode node(protocol::DirectoryEntry entry, QString path, QByteArray parent = {}) const;
    QCoro::Task<dsfm::Result<protocol::Envelope>> exchange(protocol::Envelope request);
    Context context_;
    quint64 pending_ = 0;
    QPointer<QTimer> pending_timer_;
    bool cancelled_ = false;
    bool pending_cancelled_ = false;
    quint64 operation_ = 0;
};
}
