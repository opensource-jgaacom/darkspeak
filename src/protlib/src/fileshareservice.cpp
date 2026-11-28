#include <ds/fileshareservice.h>
#include <ds/peer.h>
#include <ds/protocolcodec.h>
#include <ds/contact.h>
#include <ds/identity.h>
#include <ds/dsengine.h>
#include <ds/filemanager.h>
#include <QDeadlineTimer>
#include <QMessageAuthenticationCode>
#include <QScopeGuard>
#include <QTimer>
#include <sodium.h>
#include "logfault/logfault.h"

namespace ds::prot {
using namespace ds::protocol;
using Status = FileShareStatusGadget::FileShareStatus;
namespace {
QByteArray randomToken() {
    QByteArray token(32, Qt::Uninitialized);
    randombytes_buf(token.data(), size_t(token.size()));
    return token;
}
QString policyPath(const VirtualPath &path) {
    if (int(path.encoding()) == 1) return path.path();
    // Input has already passed canonical ASCII_ESCAPED validation. Decode one
    // presentation layer only for logical permission matching, never VFS lookup.
    const auto bytes = path.path().toLatin1();
    QByteArray logical; logical.reserve(bytes.size());
    auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '%') {
            logical.append(char((hex(bytes[i + 1]) << 4) | hex(bytes[i + 2]))); i += 2;
        } else logical.append(bytes[i]);
    }
    return QString::fromUtf8(logical);
}
struct Bucket {
    double tokens = 16;
    std::chrono::steady_clock::time_point time = std::chrono::steady_clock::now();
    bool take(double capacity, double rate) {
        const auto now = std::chrono::steady_clock::now();
        tokens = std::min(capacity, tokens + std::chrono::duration<double>(now - time).count() * rate);
        time = now;
        if (tokens < 1) return false;
        --tokens;
        return true;
    }
};
Status publicError(dsfm::Error error) {
    LFLOG_TRACE << "FileShare VFS operation failed stage=resolver code=" << int(error.code)
                << " native=" << error.native_code;
    switch (error.code) {
    case dsfm::ErrorCode::StaleCursor: return Status::FILESHARE_STALE_CURSOR;
    case dsfm::ErrorCode::Busy: case dsfm::ErrorCode::QueueFull:
    case dsfm::ErrorCode::QuotaExceeded: return Status::FILESHARE_BUSY;
    case dsfm::ErrorCode::ExternalModification: case dsfm::ErrorCode::IntegrityError: return Status::FILESHARE_CHANGED;
    default: return Status::FILESHARE_DENIED_OR_NOT_FOUND;
    }
}
Envelope result(const Envelope &request, Status status) {
    Envelope response;
    response.setProtocolVersion(codec::protocolVersion);
    response.setResponseToRequestId(request.requestId());
    if (request.hasListDirectoryRequest()) {
        ListDirectoryResult r; r.setStatus(status); response.setListDirectoryResult(r);
    } else if (request.hasStatVirtualRequest()) {
        StatVirtualResult r; r.setStatus(status); response.setStatVirtualResult(r);
    } else if (request.hasRequestDownload()) {
        RequestDownloadResult r; r.setStatus(status); response.setRequestDownloadResult(r);
    } else {
        CancelFileShareResult r; r.setStatus(status); response.setCancelFileShareResult(r);
    }
    return response;
}
DirectoryEntry entry(const dsvfs::NodeInfo &node, const dsvfs::Request &request, const QByteArray &secret) {
    DirectoryEntry out;
    out.setNodeId(QMessageAuthenticationCode::hash(QByteArray::number(request.generation) + ':' +
        QByteArray::number(request.allow_utf8) + ':' + node.id, secret, QCryptographicHash::Sha256));
    VirtualName name;
    name.setEncoding(request.allow_utf8 ? VirtualPathEncodingGadget::VirtualPathEncoding::VIRTUAL_PATH_UTF8 :
                                         VirtualPathEncodingGadget::VirtualPathEncoding::VIRTUAL_PATH_ASCII_ESCAPED);
    // Root has no component; its portable display label is not used for lookup.
    name.setValue(node.name.isEmpty() ? QStringLiteral("root") : node.name);
    out.setName(name);
    const bool file = node.type == dsfm::EntryType::File;
    out.setType(file ? VirtualEntryTypeGadget::VirtualEntryType::VIRTUAL_FILE : VirtualEntryTypeGadget::VirtualEntryType::VIRTUAL_DIRECTORY);
    const bool ready = node.status == dsfm::EntryStatus::Available;
    out.setState(ready ? VirtualEntryStateGadget::VirtualEntryState::VIRTUAL_ENTRY_READY : VirtualEntryStateGadget::VirtualEntryState::VIRTUAL_ENTRY_UPDATING);
    if (file) {
        out.setSize(node.size);
        if (node.modified.isValid()) out.setModifiedUtcDay(QDate(1970, 1, 1).daysTo(node.modified.toUTC().date()));
        if (ready && node.hash_state == dsfm::HashState::Verified && node.sha256.size() == 32) out.setSha256(node.sha256);
    }
    return out;
}
}
struct FileShareService::Session {
    Binding binding;
    QByteArray secret = randomToken();
    QSet<quint64> active, cancelled;
    QHash<quint64, dsfm::CancellationToken> generating;
    struct Pending {
        CatalogDelivery delivery;
        std::function<bool()> current;
        ~Pending() {
            try { if (delivery.cancel) delivery.cancel(); }
            catch (const std::exception &) { LFLOG_TRACE << "FileShare staged transfer cleanup failed stage=unoffered-transfer-removal"; }
        }
    };
    QHash<quint64, std::shared_ptr<Pending>> pending;
    ~Session() { for (auto &token : generating) token.cancel(); }
};
struct FileShareService::State {
    Host host;
    std::atomic_bool stopped{false};
    std::atomic<quint64> configuration_generation{0};
    // Reader guards can run on a storage worker. Publish immutable connection
    // snapshots instead of traversing the owner-thread session container there.
    std::atomic<std::shared_ptr<const QVector<std::weak_ptr<Session>>>> read_sessions{
        std::make_shared<const QVector<std::weak_ptr<Session>>>()};
    int active = 0;
    int catalog_generators = 0;
    QVector<std::weak_ptr<dsvfs::CatalogExport>> catalogs;
    struct Download { QUuid identity, contact; std::function<bool()> active; };
    QVector<Download> downloads;
    Bucket global;
    struct Rate { Bucket bucket; QDeadlineTimer expires{60000}; };
    QHash<QByteArray, Rate> rates;
    struct Cursor {
        std::weak_ptr<Session> owner;
        QString path;
        QByteArray node, backend;
        dsvfs::Request request;
        QDeadlineTimer expires{60000};
    };
    QHash<QByteArray, Cursor> cursors;
    QVector<std::weak_ptr<Session>> sessions;
};
FileShareService::FileShareService(Host host, QObject *parent)
    : QObject(parent), state_(std::make_shared<State>()) { state_->host = std::move(host); }
FileShareService::~FileShareService() { stop(); }
void FileShareService::stop() {
    state_->stopped = true; state_->cursors.clear();
    for (auto &weak : state_->sessions) if (auto session = weak.lock()) {
        for (auto &token : session->generating) token.cancel();
        session->pending.clear();
    }
}
bool FileShareService::enabled(QUuid identity) const {
    return !state_->stopped && !identity.isNull() && state_->host.enabled(identity);
}
void FileShareService::setEnabled(QUuid identity, bool value) {
    if (state_->stopped || identity.isNull()) return;
    state_->host.enable(identity, value);
    ++state_->configuration_generation;
    // No stale cursor can survive disable/re-enable.
    state_->cursors.clear();
    for (auto &weak : state_->sessions) if (auto session = weak.lock(); session && session->binding.identity == identity) {
        for (auto &token : session->generating) token.cancel();
        session->pending.clear();
    }
    emit configurationChanged();
}
FileShareService::SessionPtr FileShareService::bind(Binding binding) {
    if (binding.identity.isNull() || binding.contact.isNull() || binding.connection.isNull() ||
        !binding.available || !binding.current || !binding.allow_utf8) return {};
    state_->sessions.removeIf([](const auto &weak) { return weak.expired(); });
    if (state_->sessions.size() >= 128) return {};
    auto session = std::make_shared<Session>(); session->binding = std::move(binding);
    state_->sessions.append(session);
    state_->read_sessions.store(std::make_shared<const QVector<std::weak_ptr<Session>>>(state_->sessions));
    return session;
}
void FileShareService::attachContact(std::shared_ptr<Peer> peer, std::shared_ptr<core::Contact> contact) {
    if (!peer || !contact || !contact->getIdentity() || !peer->getPeerCert() ||
        peer->getIdentityId() != contact->getIdentity()->getUuid() ||
        peer->getPeerCert()->getHash().toByteArray() != contact->getCert()->getHash().toByteArray()) return;
    std::weak_ptr<core::Contact> weak_contact(contact);
    std::weak_ptr<Peer> weak_peer(peer);
    const auto available = [weak_contact] {
        const auto contact = weak_contact.lock();
        return contact && contact->getState() == core::Contact::ACCEPTED && !contact->iBlocked() &&
            contact->getIdentity() && contact->getIdentity()->getAllowFiles();
    };
    attach(peer, {peer->getIdentityId(), contact->getUuid(), peer->getConnectionId(), available,
        [available, weak_contact, weak_peer] {
            auto contact = weak_contact.lock(); auto peer = weak_peer.lock();
            return available() && peer && peer->isConnected() && peer->fileSharePeerPermits() && contact &&
                contact->isCurrentConnection(peer->getConnectionId()) && contact->canSendFiles();
        }, [weak_peer] { auto peer = weak_peer.lock(); return peer && peer->fileShareAllowsUtf8(); },
        [weak_contact](std::shared_ptr<dsvfs::CatalogExport> catalog) -> dsfm::Result<CatalogDelivery> {
            auto contact = weak_contact.lock();
            if (!contact || !contact->getDefaultConversation()) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
            auto staged = core::DsEngine::instance().getFileManager()->addCatalogTransfer(*contact->getDefaultConversation(), std::move(catalog), false);
            if (!staged) return staged.error();
            auto file = staged.value();
            return CatalogDelivery{file->getFileId(), [weak_contact, file]() -> dsfm::Result<void> {
                auto contact = weak_contact.lock();
                if (!contact || !file->outgoingSourceCurrent()) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
                contact->queueFile(file); return {};
            }, [file] {
                // This transfer has never been offered: revoke it locally,
                // without sending an unsolicited generic FileCancel.
                file->setState(core::File::FS_CANCELLED); file->deleteFromDb();
            }};
        }, [weak_contact] {
            const auto contact = weak_contact.lock();
            return contact && contact->canSendFileType(QStringLiteral("application/vnd.darkspeak.vfs-catalog+jsonl"), QStringLiteral("catalog.jsonl"));
        }, [weak_contact](dsvfs::Vfs &vfs, dsvfs::NodeId node, dsvfs::Request request,
                         dsvfs::PreparedRead prepared) -> dsfm::Result<CatalogDelivery> {
            auto contact = weak_contact.lock();
            if (!contact || !contact->getDefaultConversation()) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
            auto staged = core::DsEngine::instance().getFileManager()->addPreparedVfsTransfer(
                *contact->getDefaultConversation(), vfs, node, request, std::move(prepared));
            if (!staged) return staged.error();
            auto file = staged.value();
            return CatalogDelivery{file->getFileId(), [weak_contact, file]() -> dsfm::Result<void> {
                auto contact = weak_contact.lock();
                if (!contact || !file->outgoingSourceCurrent()) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
                contact->queueFile(file); return {};
            }, [file] { file->setState(core::File::FS_CANCELLED); file->deleteFromDb(); },
            [weak = std::weak_ptr<core::File>(file)] {
                auto file = weak.lock();
                return file && file->getState() != core::File::FS_DONE && file->getState() != core::File::FS_CANCELLED &&
                    file->getState() != core::File::FS_REJECTED;
            }};
        }},
        [weak_contact] { auto contact = weak_contact.lock(); return contact && contact->getIdentity() && contact->getIdentity()->getAllowUtf8(); });
}
void FileShareService::attach(std::shared_ptr<Peer> peer, Binding binding, std::function<bool()> local_utf8) {
    auto session = bind(std::move(binding));
    if (!session) return;
    QPointer<FileShareService> self(this);
    std::weak_ptr<Peer> weak(peer);
    peer->configureFileShare([self, session] {
        return self && self->enabled(session->binding.identity) && session->binding.available();
    }, std::move(local_utf8), [self, weak, session](const Envelope &request) {
        if (!self) return;
        if (!fileshare::isRequest(request)) {
            emit self->responseReceived(session->binding.connection, request); return;
        }
        self->handle(session, request).then([self, weak, session, id = request.requestId()](std::optional<Envelope> response) {
            if (auto peer = weak.lock(); peer && response && peer->fileShareReady()) {
                try {
                    peer->sendFileShare(std::move(*response));
                    if (self) self->publishCatalog(session, id);
                    return;
                } catch (const std::exception &) { LFLOG_TRACE << "FileShare response discarded: expired correlation or connection"; }
            }
            if (self) self->discardCatalog(session, id);
        });
    });
}
void FileShareService::discardCatalog(SessionPtr session, quint64 request_id) {
    if (session) session->pending.remove(request_id);
}
dsfm::Result<void> FileShareService::publishCatalog(SessionPtr session, quint64 request_id) {
    if (!session) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    auto pending = session->pending.take(request_id);
    if (!pending) return {};
    if (!pending->current()) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
    auto started = pending->delivery.start();
    if (started) pending->delivery.cancel = {};
    else LFLOG_TRACE << "FileShare transfer failed stage=generic-offer-start code=" << int(started.error().code)
                     << " native=" << started.error().native_code;
    return started;
}
QCoro::Task<std::optional<Envelope>> FileShareService::handle(SessionPtr session, Envelope input) {
    // Copy shared state before the first suspension; no QObject access afterwards.
    auto state = state_;
    if (!session || state->stopped || !fileshare::isRequest(input)) co_return std::nullopt;
    const auto &binding = session->binding;
    auto live = [&] { return !state->stopped && state->host.enabled(binding.identity) &&
        binding.available() && binding.current(); };
    if (!live()) co_return std::nullopt;
    if (!fileshare::validate(input, binding.allow_utf8()) || !fileshare::validateWire(codec::serialize(input)))
        co_return result(input, Status::FILESHARE_INVALID_REQUEST);
    if (input.hasCancelFileShareRequest()) {
        const auto id = input.cancelFileShareRequest().requestToCancel();
        if (session->active.contains(id)) session->cancelled.insert(id);
        if (session->generating.contains(id)) session->generating[id].cancel();
        session->pending.remove(id);
        co_return result(input, Status::FILESHARE_OK);
    }
    for (auto &weak : state->sessions) if (auto owner = weak.lock()) {
        for (auto it = owner->pending.begin(); it != owner->pending.end();)
            if (!it.value()->current()) it = owner->pending.erase(it); else ++it;
    }
    for (auto it = state->cursors.begin(); it != state->cursors.end();)
        if (it->owner.expired() || it->expires.hasExpired() || !it->request.current()) it = state->cursors.erase(it); else ++it;
    for (auto it = state->rates.begin(); it != state->rates.end();)
        if (it->expires.hasExpired()) it = state->rates.erase(it); else ++it;
    const QByteArray rate_key = binding.identity.toRfc4122() + binding.contact.toRfc4122();
    if ((!state->rates.contains(rate_key) && state->rates.size() >= 128) || state->active >= 32 ||
        session->active.size() >= 4 || session->active.contains(input.requestId()) || !state->global.take(64, 32))
        co_return result(input, Status::FILESHARE_BUSY);
    auto &rate = state->rates[rate_key]; rate.expires = QDeadlineTimer(60000);
    if (!rate.bucket.take(16, 8)) co_return result(input, Status::FILESHARE_BUSY);
    ++state->active; session->active.insert(input.requestId());
    auto release = qScopeGuard([&] { --state->active; session->active.remove(input.requestId()); session->cancelled.remove(input.requestId()); });
    QDeadlineTimer deadline(30000);
    const auto configuration_generation = state->configuration_generation.load();
    auto current = [&] { return live() && configuration_generation == state->configuration_generation &&
        !deadline.hasExpired() && !session->cancelled.contains(input.requestId()); };
    auto hold = state->host.hold ? state->host.hold() : std::shared_ptr<void>{};
    try {
        const bool recursive = input.hasListDirectoryRequest() && input.listDirectoryRequest().recursive();
        if (recursive && (!state->host.approve_recursion || !binding.stage_catalog ||
            (binding.accepts_catalog && !binding.accepts_catalog()) ||
            !state->host.approve_recursion(binding.identity, binding.contact, policyPath(input.listDirectoryRequest().directory()))))
            co_return result(input, Status::FILESHARE_RECURSION_REFUSED);
        state->catalogs.removeIf([](const auto &weak) {
            const auto catalog = weak.lock();
            return !catalog || !catalog->isAuthorized();
        });
        if (recursive && (state->catalog_generators + state->catalogs.size() >= 2 || !session->pending.isEmpty()))
            co_return result(input, Status::FILESHARE_BUSY);
        if (recursive) ++state->catalog_generators;
        auto catalog_admission = qScopeGuard([&] { if (recursive) --state->catalog_generators; });
        const bool download = input.hasRequestDownload();
        if (download && !binding.stage_download) co_return result(input, Status::FILESHARE_UNSUPPORTED);
        const bool listing = input.hasListDirectoryRequest();
        const auto path = listing ? input.listDirectoryRequest().directory() :
            download ? input.requestDownload().path() : input.statVirtualRequest().path();
        const bool utf8 = int(path.encoding()) == 1;
        if (utf8 != binding.allow_utf8()) co_return result(input, Status::FILESHARE_INVALID_REQUEST);
        std::optional<dsvfs::Request> admission_request;
        if (recursive) {
            auto authorized = co_await state->host.request(binding.identity, binding.contact);
            if (!current()) co_return std::nullopt;
            if (!authorized) co_return result(input, publicError(authorized.error()));
            const auto &request = authorized.value();
            if (!request.current() || !request.permissions ||
                request.permissions->resolve(request.scope, request.groups, policyPath(path), dsvfs::Operation::List) != dsvfs::Decision::Allow ||
                request.permissions->resolve(request.scope, request.groups, policyPath(path), dsvfs::Operation::Export) != dsvfs::Decision::Allow)
                co_return result(input, Status::FILESHARE_DENIED_OR_NOT_FOUND);
            admission_request = authorized.value();
        }
        auto opened = co_await state->host.open();
        if (!current()) co_return std::nullopt;
        if (!opened) co_return result(input, publicError(opened.error()));
        auto *vfs = opened.value();
        auto authorized = admission_request ? dsfm::Result<dsvfs::Request>{*admission_request}
                                           : co_await state->host.request(binding.identity, binding.contact);
        if (!current()) co_return std::nullopt;
        if (!authorized) co_return result(input, publicError(authorized.error()));
        auto request = authorized.value(); request.allow_utf8 = utf8;
        dsvfs::NodeId node;
        QByteArray backend_cursor;
        const auto cursor = listing ? input.listDirectoryRequest().continuation() : QByteArray{};
        if (!cursor.isEmpty()) {
            auto it = state->cursors.find(cursor);
            if (it == state->cursors.end() || it->owner.lock() != session || it->path != path.path() ||
                it->request.allow_utf8 != utf8 || it->request.current_generation != request.current_generation ||
                it->request.generation != request.generation)
                co_return result(input, Status::FILESHARE_STALE_CURSOR);
            request = it->request; node = it->node; backend_cursor = it->backend;
            state->cursors.erase(it);
        } else {
            auto composed = co_await vfs->compose(binding.identity, request);
            if (!current()) co_return std::nullopt;
            if (!composed) { LFLOG_TRACE << "FileShare failed stage=compose code=" << int(composed.error().code) << " current=" << request.current() << " authorize=" << bool(request.authorize) << " epoch=" << bool(request.current_generation); co_return result(input, publicError(composed.error())); }
            request = composed.value(); node = vfs->root(binding.identity);
            for (const auto &component : path.path().split(u'/', Qt::SkipEmptyParts)) {
                auto next = co_await vfs->lookup(binding.identity, node, component, request);
                if (!current() || !request.current()) co_return std::nullopt;
                if (!next) { LFLOG_TRACE << "FileShare failed stage=lookup code=" << int(next.error().code); co_return result(input, publicError(next.error())); }
                node = next.value().id;
            }
        }
        Envelope response = result(input, Status::FILESHARE_OK);
        if (recursive) {
            if (!session->pending.isEmpty()) co_return result(input, Status::FILESHARE_BUSY);
            dsfm::CancellationToken cancel;
            session->generating.insert(input.requestId(), cancel);
            auto generating = qScopeGuard([&] { session->generating.remove(input.requestId()); });
            const auto authorize = request.authorize;
            const auto permit = request.commit_permit;
            const auto guard = [state, weak_session = std::weak_ptr<Session>(session), configuration_generation, utf8, location = policyPath(path)] {
                auto session = weak_session.lock();
                if (!session) return false;
                const auto &binding = session->binding;
                return !state->stopped && state->configuration_generation == configuration_generation &&
                    state->host.enabled(binding.identity) && binding.available() && binding.current() && binding.allow_utf8() == utf8 &&
                    (!binding.accepts_catalog || binding.accepts_catalog()) &&
                    state->host.approve_recursion && state->host.approve_recursion(binding.identity, binding.contact, location);
            };
            request.authorize = [authorize, guard](dsvfs::Target target, dsvfs::Operation operation) -> dsfm::Result<bool> {
                if (!guard()) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
                return authorize(target, operation);
            };
            request.commit_permit = [permit, guard](dsvfs::Target target, dsvfs::Operation operation) -> dsfm::Result<std::shared_ptr<void>> {
                if (!guard()) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
                return permit(target, operation);
            };
            auto catalog = co_await vfs->exportCatalog(binding.identity, node, true, request, cancel);
            if (!current() || !request.current() || !guard()) co_return std::nullopt;
            if (!catalog) {
                LFLOG_TRACE << "FileShare recursive export failed stage=exportCatalog code=" << int(catalog.error().code)
                            << " native=" << catalog.error().native_code;
                co_return result(input, publicError(catalog.error()));
            }
            state->catalogs.append(catalog.value());
            auto delivery = binding.stage_catalog(catalog.value());
            if (!delivery) {
                LFLOG_TRACE << "FileShare recursive export failed stage=generic-transfer-admission code=" << int(delivery.error().code)
                            << " native=" << delivery.error().native_code;
                co_return result(input, publicError(delivery.error()));
            }
            if (delivery.value().transfer_id.size() != 16 || !delivery.value().start || !delivery.value().cancel) {
                if (delivery.value().cancel) delivery.value().cancel();
                co_return result(input, Status::FILESHARE_INTERNAL_ERROR);
            }
            auto pending = std::make_shared<Session::Pending>(); pending->delivery = std::move(delivery.value());
            pending->current = [guard, request, catalog = catalog.value(), deadline] {
                return !deadline.hasExpired() && guard() && request.current() && catalog->isAuthorized();
            };
            if (!current() || !pending->current()) co_return std::nullopt;
            ListDirectoryResult out; out.setStatus(Status::FILESHARE_OK); out.setDirectory(path);
            out.setCatalogTransferId(pending->delivery.transfer_id); out.setCatalogFormatVersion(2);
            response.setListDirectoryResult(out);
            session->pending.insert(input.requestId(), std::move(pending));
        } else if (download) {
            state->downloads.removeIf([](const auto &item) { return !item.active(); });
            int identity_count = 0, contact_count = 0;
            for (const auto &item : state->downloads) if (item.identity == binding.identity) {
                ++identity_count;
                if (item.contact == binding.contact) ++contact_count;
            }
            if (state->downloads.size() >= 16 || identity_count >= 4 || contact_count >= 2 || !session->pending.isEmpty())
                co_return result(input, Status::FILESHARE_BUSY);
            // Reserve while preparation suspends. After staging, weak generic
            // file state accounts for paused/queued work until it terminates.
            auto reservation = std::make_shared<int>(0);
            state->downloads.append({binding.identity, binding.contact,
                [weak = std::weak_ptr<int>(reservation)] { return !weak.expired(); }});
            const auto authorize = request.authorize;
            const auto guard = [state, b = binding, configuration_generation, utf8] {
                if (state->stopped || state->configuration_generation != configuration_generation ||
                    !state->host.enabled(b.identity) || !b.available()) return false;
                // Resume may use a newly authenticated connection of the same
                // identity/contact. Never borrow another contact's live session.
                const auto connections = state->read_sessions.load();
                for (const auto &weak : *connections) if (auto live = weak.lock()) {
                    const auto &current = live->binding;
                    if (current.identity == b.identity && current.contact == b.contact && current.available() &&
                        current.current() && current.allow_utf8() == utf8) return true;
                }
                return false;
            };
            request.authorize = [authorize, guard](dsvfs::Target target, dsvfs::Operation operation) -> dsfm::Result<bool> {
                if (!guard()) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
                return authorize(target, operation);
            };
            auto info = co_await vfs->stat(binding.identity, node, request);
            if (!current() || !request.current() || !guard()) co_return std::nullopt;
            if (!info) { LFLOG_TRACE << "FileShare failed stage=stat code=" << int(info.error().code); co_return result(input, publicError(info.error())); }
            const auto get = input.requestDownload();
            if (info.value().type != dsfm::EntryType::File) co_return result(input, Status::FILESHARE_DENIED_OR_NOT_FOUND);
            if (!get.knownNodeId().isEmpty() && get.knownNodeId() != entry(info.value(), request, session->secret).nodeId())
                co_return result(input, Status::FILESHARE_CHANGED);
            dsfm::CancellationToken cancel;
            session->generating.insert(input.requestId(), cancel);
            auto generating = qScopeGuard([&] { session->generating.remove(input.requestId()); });
            QTimer preparation_deadline;
            preparation_deadline.setSingleShot(true);
            QObject::connect(&preparation_deadline, &QTimer::timeout, [cancel] { cancel.cancel(); });
            preparation_deadline.start(int(std::max<qint64>(1, deadline.remainingTime())));
            dsfm::ReadPreparationOptions options; options.cancel = cancel; options.expected_sha256 = get.expectedSha256();
            options.expected_generation = info.value().revision;
            auto prepared = co_await vfs->prepareRead(binding.identity, node, options, request);
            if (!current() || !request.current() || !guard()) co_return std::nullopt;
            if (!prepared) {
                LFLOG_TRACE << "FileShare GET failed stage=prepareRead code=" << int(prepared.error().code)
                            << " native=" << prepared.error().native_code;
                co_return result(input, publicError(prepared.error()));
            }
            if ((!get.expectedSha256().isEmpty() && get.expectedSha256() != prepared.value().info.sha256) ||
                (get.hasExpectedSize() && get.expectedSize() != prepared.value().info.size)) {
                co_await prepared.value().reader->close();
                co_return result(input, Status::FILESHARE_CHANGED);
            }
            auto source_current = prepared.value().current;
            auto delivery = binding.stage_download(*vfs, node, request, std::move(prepared).value());
            if (!delivery) {
                LFLOG_TRACE << "FileShare GET failed stage=generic-transfer-admission code=" << int(delivery.error().code)
                            << " native=" << delivery.error().native_code;
                co_return result(input, publicError(delivery.error()));
            }
            if (delivery.value().transfer_id.size() != 16 || !delivery.value().start || !delivery.value().cancel || !delivery.value().active) {
                if (delivery.value().cancel) delivery.value().cancel();
                co_return result(input, Status::FILESHARE_INTERNAL_ERROR);
            }
            auto pending = std::make_shared<Session::Pending>(); pending->delivery = std::move(delivery).value();
            pending->current = [guard, request, source_current, deadline] {
                return !deadline.hasExpired() && guard() && request.current() && source_current && source_current();
            };
            if (!current() || !pending->current()) co_return std::nullopt;
            // Replace the in-flight reservation with weak generic-file state.
            state->downloads.append({binding.identity, binding.contact, pending->delivery.active});
            RequestDownloadResult out; out.setStatus(Status::FILESHARE_OK); out.setTransferId(pending->delivery.transfer_id);
            response.setRequestDownloadResult(out);
            session->pending.insert(input.requestId(), std::move(pending));
        } else if (listing) {
            const auto limit = input.listDirectoryRequest().pageSize();
            auto page = co_await vfs->list(binding.identity, node, {limit ? limit : 100, backend_cursor}, request);
            if (!current() || !request.current() || binding.allow_utf8() != utf8) co_return std::nullopt;
            if (!page) co_return result(input, publicError(page.error()));
            ListDirectoryResult out; out.setStatus(Status::FILESHARE_OK); out.setDirectory(path);
            QList<DirectoryEntry> entries;
            for (const auto &info : page.value().entries) entries.append(entry(info, request, session->secret));
            out.setEntries(entries);
            if (!page.value().next_cursor.isEmpty()) {
                if (state->cursors.size() >= 128) co_return result(input, Status::FILESHARE_BUSY);
                auto token = randomToken();
                state->cursors.insert(token, {session, path.path(), node, page.value().next_cursor, request});
                out.setContinuation(token);
            }
            out.setComplete(out.continuation().isEmpty()); response.setListDirectoryResult(out);
        } else {
            auto info = co_await vfs->stat(binding.identity, node, request);
            if (!current() || !request.current() || binding.allow_utf8() != utf8) co_return std::nullopt;
            if (!info) { LFLOG_TRACE << "FileShare failed stage=stat code=" << int(info.error().code); co_return result(input, publicError(info.error())); }
            auto metadata = entry(info.value(), request, session->secret);
            const auto known = input.statVirtualRequest().knownNodeId();
            if (!known.isEmpty() && known != metadata.nodeId()) co_return result(input, Status::FILESHARE_CHANGED);
            StatVirtualResult out; out.setStatus(Status::FILESHARE_OK); out.setPath(path); out.setEntry(metadata);
            response.setStatVirtualResult(out);
        }
        if (!fileshare::validate(response, binding.allow_utf8()) || !fileshare::validateWire(codec::serialize(response))) {
            LFLOG_TRACE << "FileShare response rejected at portable metadata/size validation";
            session->pending.remove(input.requestId());
            co_return result(input, Status::FILESHARE_INTERNAL_ERROR);
        }
        LFLOG_DEBUG << "FileShare authorized direct request completed";
        co_return response;
    } catch (const std::exception &) {
        LFLOG_TRACE << "FileShare request failed at application service operation";
        session->pending.remove(input.requestId());
        if (!current()) co_return std::nullopt;
        co_return result(input, Status::FILESHARE_INTERNAL_ERROR);
    }
}
}
