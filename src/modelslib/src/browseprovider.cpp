#include <ds/browseprovider.h>
#include <ds/contact.h>
#include <ds/dsengine.h>
#include <ds/filemanager.h>
#include <ds/identity.h>
#include <ds/peer.h>
#include <QCoroTimer>
#include <QScopeGuard>
#include <QTimeZone>
#include <QCryptographicHash>
#include <logfault/logfault.h>

namespace ds::models {
using namespace dsfm;
namespace {
Error invalid() { return Error{ErrorCode::InvalidArgument}; }
Error unavailable() { return Error{ErrorCode::PermissionDenied}; }
QString childPath(const QString &parent, const QString &name) { return parent == "/" ? parent + name : parent + "/" + name; }
Error remoteError(int status) {
    LFLOG_TRACE << "FileShare browse response rejected status=" << status;
    return Error{status == 2 ? ErrorCode::PermissionDenied : status == 8 ? ErrorCode::IntegrityError : ErrorCode::StorageUnavailable};
}
}
QCoro::Task<Result<BrowserNode>> BrowseProvider::resolveDirectory(QStringList route, CancellationToken cancel) {
    if (route.size() > 64 || ("/" + route.join('/')).toUtf8().size() > prot::fileshare::max_path_bytes) co_return invalid();
    auto result = co_await root(); if (!result) co_return result.error();
    for (auto name : route) {
        if (cancel.cancelled() || !current()) co_return Error{ErrorCode::Cancelled};
        auto child = co_await resolveChild(result.value(), name); if (!child) co_return child.error();
        if (child.value().info.type != EntryType::Directory || child.value().info.name != name) co_return invalid();
        result = std::move(child);
    }
    if (cancel.cancelled()) co_return Error{ErrorCode::Cancelled};
    co_return std::move(result.value());
}
LocalBrowseProvider::LocalBrowseProvider(QUuid identity, Host host) : BrowseProvider({identity, {}}), host_(std::move(host)) {}
bool LocalBrowseProvider::current() const {
    return generation_->load() == 1 && !scope_.identity.isNull() && host_.identity_exists && host_.identity_exists(scope_.identity);
}
void LocalBrowseProvider::cancel() { unwatch(); generation_->fetch_add(1); }
QCoro::Task<Result<void>> LocalBrowseProvider::watch(BrowserNode directory, dsvfs::ChangeCallback callback) {
    unwatch(); const auto generation = watch_generation_;
    if (!current() || !owns(directory) || !host_.open) co_return unavailable();
    auto hold = host_.hold ? host_.hold() : nullptr;
    auto opened = co_await host_.open(); if (!opened) co_return opened.error();
    if (!current() || generation != watch_generation_) co_return Error{ErrorCode::Cancelled};
    auto composed = co_await opened.value()->compose(scope_.identity, request()); if (!composed) co_return composed.error();
    auto result = co_await opened.value()->subscribeDirectory(scope_.identity, directory.info.id, std::move(callback), composed.value());
    if (!current() || generation != watch_generation_) co_return Error{ErrorCode::Cancelled};
    if (!result) co_return result.error();
    subscription_ = std::move(result.value());
    co_return Result<void>{};
}
dsvfs::Request LocalBrowseProvider::request() const {
    dsvfs::Request r;
    r.scope = scope_.identity; r.generation = 1; r.current_generation = generation_;
    r.authorize = [identity = scope_.identity, generation = generation_](dsvfs::Target target, dsvfs::Operation op) -> Result<bool> {
        using enum dsvfs::Operation;
        return generation->load() == 1 && (target.identity.isNull() || target.identity == identity) &&
            (op == Traverse || op == Inspect || op == List || op == Recurse);
    };
    // No write/configuration permit: local browsing cannot mutate storage or mounts.
    return r;
}
QCoro::Task<Result<BrowserNode>> LocalBrowseProvider::root() {
    if (!current() || !host_.open) co_return unavailable();
    auto hold = host_.hold ? host_.hold() : nullptr;
    auto opened = co_await host_.open(); if (!opened) co_return opened.error();
    if (!current()) co_return unavailable();
    auto vfs = opened.value();
    auto composed = co_await vfs->compose(scope_.identity, request()); if (!composed) co_return composed.error();
    auto info = co_await vfs->stat(scope_.identity, vfs->root(scope_.identity), composed.value());
    if (!current()) co_return unavailable();
    if (!info) co_return info.error();
    co_return BrowserNode{id_, info.value(), "/", false};
}
QCoro::Task<Result<BrowserNode>> LocalBrowseProvider::stat(BrowserNode node) {
    if (!current() || !owns(node) || !host_.open) co_return invalid();
    auto hold = host_.hold ? host_.hold() : nullptr;
    auto opened = co_await host_.open(); if (!opened) co_return opened.error();
    if (!current()) co_return unavailable();
    auto composed = co_await opened.value()->compose(scope_.identity, request()); if (!composed) co_return composed.error();
    auto info = co_await opened.value()->stat(scope_.identity, node.info.id, composed.value());
    if (!current()) co_return unavailable();
    if (!info) co_return info.error();
    co_return BrowserNode{id_, info.value(), node.path, info.value().type == EntryType::File};
}
QCoro::Task<Result<BrowserNode>> LocalBrowseProvider::resolveChild(BrowserNode parent, QString name) {
    if (!current() || !owns(parent) || parent.info.type != EntryType::Directory || !host_.open) co_return invalid();
    auto hold = host_.hold ? host_.hold() : nullptr;
    auto opened = co_await host_.open(); if (!opened) co_return opened.error();
    if (!current()) co_return unavailable();
    auto composed = co_await opened.value()->compose(scope_.identity, request()); if (!composed) co_return composed.error();
    auto info = co_await opened.value()->lookup(scope_.identity, parent.info.id, name, composed.value());
    if (!current()) co_return unavailable();
    if (!info) co_return info.error();
    co_return BrowserNode{id_, info.value(), childPath(parent.path, name), info.value().type == EntryType::File};
}
QCoro::Task<Result<BrowserPage>> LocalBrowseProvider::listPage(BrowserNode directory, QByteArray cursor) {
    if (!current() || !owns(directory) || directory.info.type != EntryType::Directory || !host_.open || cursor.size() > 2048)
        co_return invalid();
    auto hold = host_.hold ? host_.hold() : nullptr;
    auto opened = co_await host_.open(); if (!opened) co_return opened.error();
    if (!current()) co_return unavailable();
    auto composed = co_await opened.value()->compose(scope_.identity, request()); if (!composed) co_return composed.error();
    auto page = co_await opened.value()->list(scope_.identity, directory.info.id, {100, cursor}, composed.value());
    if (!current()) co_return unavailable();
    if (!page) co_return page.error();
    BrowserPage out; out.continuation = page.value().next_cursor;
    for (auto &info : page.value().entries)
        out.entries.append({id_, info, childPath(directory.path, info.name), info.type == EntryType::File});
    co_return out;
}

RemoteBrowseProvider::RemoteBrowseProvider(Context context) : BrowseProvider(context.scope), context_(std::move(context)) {}
Result<std::shared_ptr<BrowseProvider>> RemoteBrowseProvider::forContact(std::shared_ptr<prot::Peer> peer,
        std::shared_ptr<core::Contact> contact, prot::FileShareService &responses) {
    if (!peer || !contact || !contact->getIdentity() || !peer->getPeerCert() || !contact->getCert() ||
        peer->getIdentityId() != contact->getIdentity()->getUuid() ||
        peer->getPeerCert()->getHash().toByteArray() != contact->getCert()->getHash().toByteArray()) return unavailable();
    std::weak_ptr<prot::Peer> weak_peer(peer); std::weak_ptr<core::Contact> weak_contact(contact);
    const auto utf8 = peer->fileShareAllowsUtf8();
    Context context{{peer->getIdentityId(), contact->getUuid()}, peer->getConnectionId(), utf8,
        [weak_peer, weak_contact, utf8] {
            auto peer = weak_peer.lock(); auto contact = weak_contact.lock();
            return peer && contact && contact->getState() == core::Contact::ACCEPTED && !contact->iBlocked() &&
                contact->getIdentity() && contact->getIdentity()->getAllowFiles() && peer->isConnected() &&
                contact->isCurrentConnection(peer->getConnectionId()) && peer->fileShareReady() &&
                peer->fileShareAllowsUtf8() == utf8;
        }, [weak_peer](protocol::Envelope e) { auto peer = weak_peer.lock(); return peer ? peer->sendFileShare(std::move(e)) : 0; }, &responses};
    // Retain only policy validity, never the disconnected Peer or a capability.
    // A new connection invalidates the snapshot via peerPreferencesChanged.
    struct HistoryPolicy : QObject { bool valid = true; };
    auto history = std::make_shared<HistoryPolicy>();
    history->valid = peer->fileSharePeerPermits();
    auto invalidate = [guard = history.get()] { guard->valid = false; };
    QObject::connect(contact.get(), &core::Contact::peerPreferencesChanged, history.get(), invalidate);
    QObject::connect(contact.get(), &core::Contact::stateChanged, history.get(), invalidate);
    QObject::connect(contact.get(), &core::Contact::blockedChanged, history.get(), invalidate);
    QObject::connect(contact->getIdentity(), &core::Identity::securityPreferencesChanged, history.get(), invalidate);
    QObject::connect(&responses, &prot::FileShareService::configurationChanged, history.get(), invalidate);
    const auto identity = QPointer(contact->getIdentity());
    context.cached_metadata_allowed = [history, weak_contact, identity] {
        auto contact = weak_contact.lock();
        return history->valid && contact && identity && contact->getIdentity() == identity &&
            identity->getAllowFiles() && contact->getState() == core::Contact::ACCEPTED &&
            !contact->iBlocked() && !contact->currentConnection();
    };
    const auto current = context.current;
    QPointer<core::FileManager> files(core::DsEngine::instance().getFileManager());
    const auto scope = context.scope; const auto connection = context.connection;
    context.downloads_allowed = [weak_contact] {
        auto contact = weak_contact.lock();
        return contact && contact->canReceiveFileType("application/octet-stream");
    };
    context.accept_download = [files, current, scope, connection, weak_contact](BrowserNode node, QByteArray transfer) -> Result<void> {
        if (!files || !current()) return unavailable();
        try {
            return files->expectDownload(scope.identity, scope.contact, connection, std::move(transfer),
                node.size_known ? std::optional<quint64>(node.info.size) : std::nullopt, node.info.sha256,
                [current, weak_contact] {
                    auto contact = weak_contact.lock();
                    return current() && contact && contact->canReceiveFileType("application/octet-stream");
                });
        } catch (const std::exception &) {
            LFLOG_TRACE << "FileShare download consent failed stage=scoped-file-admission";
            return unavailable();
        }
    };
    auto provider = std::make_shared<RemoteBrowseProvider>(std::move(context));
    if (!provider->current()) return unavailable();
    return std::static_pointer_cast<BrowseProvider>(provider);
}
bool RemoteBrowseProvider::current() const {
    return !cancelled_ && !scope_.identity.isNull() && !scope_.contact.isNull() && !context_.connection.isNull() &&
        context_.responses && context_.current && context_.current();
}
bool RemoteBrowseProvider::canDownload(const BrowserNode &node) const {
    return current() && owns(node) && context_.accept_download && (!context_.downloads_allowed || context_.downloads_allowed()) && node.info.type == EntryType::File &&
        node.info.status == EntryStatus::Available;
}
QCoro::Task<Result<QByteArray>> RemoteBrowseProvider::download(BrowserNode node) {
    if (!canDownload(node) || !prot::fileshare::validPath(path(node.path), context_.allow_utf8)) co_return unavailable();
    protocol::RequestDownload get; get.setPath(path(node.path)); get.setKnownNodeId(node.info.id);
    get.setExpectedSha256(node.info.sha256); if (node.size_known) get.setExpectedSize(node.info.size);
    protocol::Envelope envelope; envelope.setProtocolVersion(3); envelope.setRequestDownload(get);
    auto reply = co_await exchange(std::move(envelope)); if (!reply) co_return reply.error();
    const auto result = reply.value().requestDownloadResult();
    if (int(result.status()) != 1) co_return remoteError(int(result.status()));
    if (!canDownload(node)) co_return unavailable();
    auto admitted = context_.accept_download(node, result.transferId()); if (!admitted) co_return admitted.error();
    co_return result.transferId();
}
bool RemoteBrowseProvider::cachedMetadataAllowed() const {
    return !cancelled_ && context_.responses && context_.cached_metadata_allowed && context_.cached_metadata_allowed();
}
void RemoteBrowseProvider::cancelPending() {
    ++operation_;
    if (pending_ && !pending_cancelled_ && current() && context_.send) {
        pending_cancelled_ = true;
        protocol::CancelFileShareRequest cancel; cancel.setRequestToCancel(pending_);
        protocol::Envelope e; e.setProtocolVersion(3); e.setCancelFileShareRequest(cancel);
        send(std::move(e));
    }
    if (pending_timer_) pending_timer_->start(0);
}
void RemoteBrowseProvider::cancel() { cancelPending(); cancelled_ = true; }
quint64 RemoteBrowseProvider::send(protocol::Envelope request) noexcept {
    try { return context_.send ? context_.send(std::move(request)) : 0; }
    catch (const std::exception &) {
        LFLOG_TRACE << "FileShare browser control failed stage=peer-send (connection/readiness/admission)";
        return 0;
    }
}
protocol::VirtualPath RemoteBrowseProvider::path(QString value) const {
    protocol::VirtualPath p; p.setPath(value);
    p.setEncoding(protocol::VirtualPathEncodingGadget::VirtualPathEncoding(context_.allow_utf8 ? 1 : 2)); return p;
}
BrowserNode RemoteBrowseProvider::node(protocol::DirectoryEntry entry, QString value, QByteArray parent) const {
    dsvfs::NodeInfo info;
    info.id = entry.nodeId(); info.parent = std::move(parent); info.name = entry.name().value();
    info.type = int(entry.type()) == 1 ? EntryType::File : EntryType::Directory;
    info.status = int(entry.state()) == 1 ? EntryStatus::Available : EntryStatus::Updating;
    if (entry.hasSize()) info.size = entry.size();
    info.sha256 = entry.sha256();
    info.hash_state = info.sha256.isEmpty() ? HashState::Missing : HashState::Verified;
    if (entry.hasModifiedUtcDay()) info.modified = QDateTime::fromSecsSinceEpoch(entry.modifiedUtcDay() * qint64(86400), QTimeZone::UTC);
    return {id_, std::move(info), std::move(value), entry.hasSize()};
}
QCoro::Task<Result<protocol::Envelope>> RemoteBrowseProvider::exchange(protocol::Envelope request) {
    if (!current() || pending_ || !context_.send) co_return unavailable();
    const auto operation = operation_;
    pending_cancelled_ = false;
    QTimer timer; timer.setSingleShot(true); timer.start(30000);
    std::optional<protocol::Envelope> reply;
    QObject::connect(context_.responses, &prot::FileShareService::responseReceived, &timer,
        [&](QUuid connection, protocol::Envelope response) {
            if (connection != context_.connection || response.responseToRequestId() != pending_ || !pending_) return;
            reply = std::move(response); timer.start(0);
        });
    pending_timer_ = &timer;
    auto cleanup = qScopeGuard([this] { pending_ = 0; pending_timer_ = nullptr; });
    pending_ = send(request);
    if (!pending_) co_return Error{ErrorCode::StorageUnavailable};
    request.setRequestId(pending_);
    co_await qCoro(timer).waitForTimeout();
    if (operation != operation_) co_return Error{ErrorCode::Cancelled};
    if (!current()) co_return unavailable();
    if (!reply) {
        protocol::CancelFileShareRequest cancel; cancel.setRequestToCancel(pending_);
        protocol::Envelope e; e.setProtocolVersion(3); e.setCancelFileShareRequest(cancel); send(std::move(e));
        LFLOG_TRACE << "FileShare browse request timed out operation=" << (request.hasStatVirtualRequest() ? "STAT" : "LIST");
        co_return Error{ErrorCode::StorageUnavailable};
    }
    if (!prot::fileshare::validate(*reply, context_.allow_utf8) || !prot::fileshare::matches(request, *reply)) co_return invalid();
    co_return *reply;
}
QCoro::Task<Result<BrowserNode>> RemoteBrowseProvider::root() {
    if (!current()) co_return unavailable();
    // The virtual root has no component name or server-issued entry reference.
    // Represent that known route locally; LIST supplies current authorization.
    // Never send this synthetic view key as a remote known_node_id.
    BrowserNode root; root.provider = id_; root.path = "/"; root.info.name = "/";
    root.info.id = QCryptographicHash::hash(id_.toRfc4122() + "/", QCryptographicHash::Sha256);
    co_return root;
}
QCoro::Task<Result<BrowserNode>> RemoteBrowseProvider::stat(BrowserNode value) {
    if (!owns(value) || !prot::fileshare::validPath(path(value.path), context_.allow_utf8)) co_return invalid();
    if (value.path == "/") {
        auto page = co_await listPage(value);
        if (!page) co_return page.error();
        co_return value;
    }
    protocol::StatVirtualRequest stat; stat.setPath(path(value.path)); stat.setKnownNodeId(value.info.id);
    protocol::Envelope e; e.setProtocolVersion(3); e.setStatVirtualRequest(stat);
    auto reply = co_await exchange(e); if (!reply) co_return reply.error();
    auto result = reply.value().statVirtualResult();
    if (int(result.status()) != 1) co_return remoteError(int(result.status()));
    co_return node(result.entry(), value.path, value.info.parent);
}
QCoro::Task<Result<BrowserNode>> RemoteBrowseProvider::resolveChild(BrowserNode parent, QString name) {
    if (!owns(parent) || parent.info.type != EntryType::Directory || name.isEmpty() || name.contains('/')) co_return invalid();
    auto value = childPath(parent.path, name);
    if (!prot::fileshare::validPath(path(value), context_.allow_utf8)) co_return invalid();
    protocol::StatVirtualRequest stat; stat.setPath(path(value));
    protocol::Envelope e; e.setProtocolVersion(3); e.setStatVirtualRequest(stat);
    auto reply = co_await exchange(e); if (!reply) co_return reply.error();
    auto result = reply.value().statVirtualResult();
    if (int(result.status()) != 1) co_return remoteError(int(result.status()));
    co_return node(result.entry(), value, parent.info.id);
}
QCoro::Task<Result<BrowserNode>> RemoteBrowseProvider::resolveDirectory(QStringList route, CancellationToken cancel) {
    if (cancel.cancelled()) co_return Error{ErrorCode::Cancelled};
    if (route.isEmpty()) co_return co_await root();
    const auto value = "/" + route.join('/');
    if (route.size() > 64 || !prot::fileshare::validPath(path(value), context_.allow_utf8)) co_return invalid();
    // STAT resolves all virtual legs on the server. Avoid one network round trip
    // per breadcrumb, which would exhaust the bounded browse rate on deep paths.
    protocol::StatVirtualRequest stat; stat.setPath(path(value));
    protocol::Envelope e; e.setProtocolVersion(3); e.setStatVirtualRequest(stat);
    auto reply = co_await exchange(e); if (!reply) co_return reply.error();
    if (cancel.cancelled()) co_return Error{ErrorCode::Cancelled};
    auto result = reply.value().statVirtualResult();
    if (int(result.status()) != 1) co_return remoteError(int(result.status()));
    if (int(result.entry().type()) != 2 || result.entry().name().value() != route.last()) co_return invalid();
    co_return node(result.entry(), value);
}
QCoro::Task<Result<BrowserPage>> RemoteBrowseProvider::listPage(BrowserNode directory, QByteArray cursor) {
    if (!owns(directory) || directory.info.type != EntryType::Directory || cursor.size() > prot::fileshare::max_cursor_bytes ||
        !prot::fileshare::validPath(path(directory.path), context_.allow_utf8)) co_return invalid();
    protocol::ListDirectoryRequest list; list.setDirectory(path(directory.path)); list.setPageSize(100); list.setContinuation(cursor);
    protocol::Envelope e; e.setProtocolVersion(3); e.setListDirectoryRequest(list);
    auto reply = co_await exchange(e); if (!reply) co_return reply.error();
    auto result = reply.value().listDirectoryResult();
    if (int(result.status()) != 1) co_return remoteError(int(result.status()));
    BrowserPage page; page.continuation = result.continuation();
    QSet<QByteArray> ids;
    for (auto entry : result.entries()) {
        if (ids.contains(entry.nodeId())) co_return invalid();
        ids.insert(entry.nodeId());
        page.entries.append(node(entry, childPath(directory.path, entry.name().value()), directory.info.id));
    }
    co_return page;
}
}
