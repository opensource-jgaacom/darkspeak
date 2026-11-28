#include <ds/remotecatalogs.h>
#include <ds/fileshareservice.h>
#include <ds/filemanager.h>
#include <ds/contactmanager.h>
#include <ds/dsengine.h>
#include <ds/peer.h>
#include <utility>
#include "logfault/logfault.h"

namespace ds::prot {
using namespace dsfm;
namespace {
constexpr auto catalog_mime = "application/vnd.darkspeak.vfs-catalog+jsonl";
constexpr quint64 max_catalog_bytes = 256ULL * 1024 * 1024;
Error rejected(ErrorCode code = ErrorCode::PermissionDenied) { return Error{code}; }
}
RemoteCatalogs::RemoteCatalogs(Discard discard, QObject *parent)
    : QObject(parent), discard_(std::move(discard)) {
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &RemoteCatalogs::prune);
    timer_.start();
}
void RemoteCatalogs::install(core::FileManager &files, FileShareService &service) {
    QPointer<RemoteCatalogs> self(this);
    QPointer<core::FileManager> manager(&files);
    discard_ = [manager](QUuid identity, QUuid contact_id, QByteArray transfer) {
        if (!manager) return;
        try {
            auto contact = core::DsEngine::instance().getContactManager()->getContact(contact_id);
            if (!contact || !contact->getIdentity() || contact->getIdentity()->getUuid() != identity) return;
            auto file = manager->getFileFromId(transfer, *contact, core::File::INCOMING);
            if (!file) return;
            if (file->getState() != core::File::FS_DONE) file->cancel();
            file->deleteFromDb();
            manager->storage().collectGarbage().then([](Result<void> result) {
                if (!result) LFLOG_TRACE << "Remote catalog cleanup failed at storage stage code=" << int(result.error().code);
            });
        } catch (const std::exception &) { LFLOG_TRACE << "Remote catalog cleanup failed at scoped file-row stage"; }
    };
    files.incomingCatalogAdmission = [self](const core::PeerFileOffer &offer) {
        if (!self || !offer.peer || !offer.peer->getPeerCert() || offer.size <= 0) return false;
        auto identity = core::DsEngine::instance().getIdentityManager()->identityFromUuid(offer.peer->getIdentityId());
        auto contact = identity ? identity->contactFromHash(offer.peer->getPeerCert()->getHash().toByteArray()) : nullptr;
        return contact && self->admit(identity->getUuid(), contact->getUuid(), offer.connectionId, offer.fileId,
                                     offer.type, offer.name, quint64(offer.size), offer.sha512);
    };
    connect(&service, &FileShareService::responseReceived, this, &RemoteCatalogs::response);
    connect(&files.storage(), &core::ManagedStorage::shutdownStarted, this, &RemoteCatalogs::stop);
    connect(&files, &core::FileManager::fileStateChanged, this, [self, manager](const core::File *raw) {
        if (!self || !manager || raw->getDirection() != core::File::INCOMING ||
            raw->getMimeType() != QLatin1String(catalog_mime)) return;
        auto contact = raw->getContact();
        if (!contact || !contact->getIdentity()) return;
        if (raw->getState() == core::File::FS_DONE) self->completedFile(*manager, manager->getFile(raw->getId()));
        else if (raw->getState() == core::File::FS_FAILED || raw->getState() == core::File::FS_CANCELLED || raw->getState() == core::File::FS_REJECTED)
            self->failed(contact->getIdentity()->getUuid(), contact->getUuid(), raw->getFileId());
    });
}
Result<quint64> RemoteCatalogs::requestContact(std::shared_ptr<Peer> peer, std::shared_ptr<core::Contact> contact, QString path) {
    if (!peer || !contact || !contact->getIdentity() || !peer->getPeerCert() ||
        peer->getIdentityId() != contact->getIdentity()->getUuid() ||
        peer->getPeerCert()->getHash().toByteArray() != contact->getCert()->getHash().toByteArray()) return rejected();
    std::weak_ptr<Peer> weak_peer(peer);
    std::weak_ptr<core::Contact> weak_contact(contact);
    const auto utf8 = peer->fileShareAllowsUtf8();
    Context context{peer->getIdentityId(), contact->getUuid(), peer->getConnectionId(), utf8,
        [weak_peer, weak_contact, utf8] {
            auto peer = weak_peer.lock(); auto contact = weak_contact.lock();
            return peer && contact && contact->getState() == core::Contact::ACCEPTED && !contact->iBlocked() &&
                contact->getIdentity() && contact->getIdentity()->getAllowFiles() && peer->isConnected() &&
                contact->isCurrentConnection(peer->getConnectionId()) && peer->fileShareReady() &&
                peer->fileShareAllowsUtf8() == utf8;
        }, [weak_peer](protocol::Envelope envelope) {
            auto peer = weak_peer.lock();
            if (!peer) return quint64(0);
            return peer->sendFileShare(std::move(envelope));
        }};
    return request(std::move(context), std::move(path));
}
QCoro::Task<void> RemoteCatalogs::completedFile(core::FileManager &files, std::shared_ptr<core::File> file) {
    QPointer<RemoteCatalogs> self(this);
    auto hold = files.storage().holdOperation();
    auto contact = file->getContact();
    if (!contact || !contact->getIdentity()) co_return;
    const auto identity = contact->getIdentity()->getUuid(), contact_id = contact->getUuid();
    const auto transfer = file->getFileId();
    auto vfs = co_await files.storage().openVfs();
    if (!self) co_return;
    if (!vfs || file->storageNamespace() != files.storage().namespaceId(core::ManagedStorage::Private)) {
        self->failed(identity, contact_id, transfer); co_return;
    }
    ReadPreparationOptions options;
    options.expected_sha256 = file->getHash();
    auto prepared = co_await files.storage().manager().prepareRead(file->storageNamespace(), file->storageId(), options);
    if (!self) co_return;
    if (!prepared) { self->failed(identity, contact_id, transfer); co_return; }
    auto result = co_await self->complete(identity, contact_id, file->offeredConnection(), transfer,
        {file->storageNamespace(), file->storageId(), quint64(file->getSize()), file->getHash()},
        std::shared_ptr<IFileReader>(std::move(prepared.value().reader)), *vfs.value());
    if (!result && self) self->failed(identity, contact_id, transfer);
}
RemoteCatalogs::~RemoteCatalogs() { stop(); }
void RemoteCatalogs::discard(const Item &item) {
    item.cancel.cancel();
    if (discard_ && item.offered) discard_(item.context.identity, item.context.contact, item.transfer);
}
void RemoteCatalogs::prune() {
    for (auto it = items_.begin(); it != items_.end();) {
        if ((*it)->expires.hasExpired() || !(*it)->context.current()) {
            auto item = *it;
            it = items_.erase(it);
            discard(*item);
        } else ++it;
    }
}
void RemoteCatalogs::stop() {
    stopped_ = true;
    timer_.stop();
    auto items = std::exchange(items_, {});
    for (const auto &item : items) discard(*item);
}
Result<quint64> RemoteCatalogs::request(Context context, QString path) {
    if (stopped_ || context.identity.isNull() || context.contact.isNull() || context.connection.isNull() ||
        !context.current || !context.send || !context.current()) return rejected();
    prune();
    if (items_.size() >= 2) return rejected(ErrorCode::QueueFull);
    protocol::VirtualPath directory;
    directory.setEncoding(protocol::VirtualPathEncodingGadget::VirtualPathEncoding(context.allow_utf8 ? 1 : 2));
    directory.setPath(path);
    protocol::ListDirectoryRequest list; list.setDirectory(directory); list.setRecursive(true);
    protocol::Envelope envelope; envelope.setProtocolVersion(3); envelope.setRequestId(1); envelope.setListDirectoryRequest(list);
    if (!fileshare::validate(envelope, context.allow_utf8)) return rejected(ErrorCode::InvalidArgument);
    auto item = std::make_shared<Item>(); item->context = std::move(context); item->path = std::move(path);
    try { item->request = item->context.send(envelope); }
    catch (const std::exception &) { return rejected(ErrorCode::IoError); }
    if (!item->request) return rejected(ErrorCode::InvalidArgument);
    items_.append(item);
    return item->request;
}
void RemoteCatalogs::response(QUuid connection, const protocol::Envelope &envelope) {
    if (stopped_) return;
    prune();
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        auto item = *it;
        if (item->context.connection != connection || item->request != envelope.responseToRequestId()) continue;
        // One acceptance only; neither duplicate nor stale responses replace authority.
        if (!item->transfer.isEmpty()) return;
        const auto fail = [&] { items_.erase(it); };
        if (!fileshare::validate(envelope, item->context.allow_utf8) || !envelope.hasListDirectoryResult()) { fail(); return; }
        const auto result = envelope.listDirectoryResult();
        if (int(result.status()) != 1 || result.catalogFormatVersion() != 2 || result.directory().path() != item->path ||
            int(result.directory().encoding()) != (item->context.allow_utf8 ? 1 : 2)) { fail(); return; }
        for (const auto &other : items_)
            if (other != item && other->context.identity == item->context.identity && other->context.contact == item->context.contact &&
                other->transfer == result.catalogTransferId()) { fail(); return; }
        item->transfer = result.catalogTransferId();
        item->expires = QDeadlineTimer(300000);
        return;
    }
}
void RemoteCatalogs::cancel(QUuid connection, quint64 request_id) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        auto item = *it;
        if (item->context.connection != connection || item->request != request_id) continue;
        items_.erase(it);
        if (item->transfer.isEmpty() && item->context.current()) {
            protocol::CancelFileShareRequest cancel; cancel.setRequestToCancel(request_id);
            protocol::Envelope envelope; envelope.setProtocolVersion(3); envelope.setCancelFileShareRequest(cancel);
            try { item->context.send(std::move(envelope)); }
            catch (const std::exception &) { LFLOG_TRACE << "Remote catalog request cancellation discarded at connection stage"; }
        }
        discard(*item);
        return;
    }
}
bool RemoteCatalogs::admit(QUuid identity, QUuid contact, QUuid connection, QByteArray transfer,
                          QString mime, QString name, quint64 size, QByteArray digest) {
    if (stopped_ || mime != QLatin1String(catalog_mime) || name != QStringLiteral("catalog.jsonl") ||
        !size || size > max_catalog_bytes || digest.size() != 32 || transfer.size() != 16) return false;
    prune();
    for (const auto &item : items_) {
        if (item->context.identity != identity || item->context.contact != contact || item->context.connection != connection ||
            item->transfer != transfer) continue;
        if (item->offered) return item->digest == digest && item->size == size;
        item->offered = true; item->digest = digest; item->size = size;
        return true;
    }
    return false;
}
QCoro::Task<Result<void>> RemoteCatalogs::complete(QUuid identity, QUuid contact, QUuid connection,
    QByteArray transfer, Cached artifact, std::shared_ptr<IFileReader> reader, dsvfs::Vfs &validator) {
    if (stopped_) co_return rejected();
    prune();
    std::shared_ptr<Item> selected;
    for (const auto &item : items_) {
        if (item->context.identity == identity && item->context.contact == contact && item->context.connection == connection &&
            item->transfer == transfer && item->offered && !item->validating && !item->cache) { selected = item; break; }
    }
    if (!selected) co_return rejected();
    const auto &item = selected;
    if (!reader || artifact.namespace_id.isNull() || artifact.file.isNull() || artifact.size != item->size ||
        artifact.sha256 != item->digest) {
        items_.removeAll(item); discard(*item);
        co_return rejected(ErrorCode::Corruption);
    }
    item->validating = true;
    QPointer<RemoteCatalogs> self(this);
    auto valid = co_await validator.validateCatalog(std::move(reader), item->size, item->digest, item->cancel, item->context.allow_utf8, true);
    if (!self) co_return rejected(ErrorCode::Cancelled);
    if (!valid || self->stopped_ || !self->items_.contains(item) || item->expires.hasExpired() || !item->context.current()) {
        self->items_.removeAll(item); self->discard(*item);
        LFLOG_TRACE << "Remote catalog withheld at full-file validation/publication stage code=" << (valid ? int(ErrorCode::Cancelled) : int(valid.error().code));
        co_return valid ? rejected(ErrorCode::Cancelled) : valid.error();
    }
    // Keep the previous valid snapshot until its replacement has passed every
    // check, then release it so refreshing cannot fill the bounded cache.
    item->cache = artifact;
    for (auto it = self->items_.begin(); it != self->items_.end();) {
        auto previous = *it;
        if (previous != item && previous->cache && previous->context.identity == identity &&
            previous->context.contact == contact && previous->path == item->path) {
            it = self->items_.erase(it); self->discard(*previous);
        } else ++it;
    }
    item->expires = QDeadlineTimer(300000);
    LFLOG_DEBUG << "Remote catalog complete and validated before cache publication";
    emit self->catalogReady(identity, contact, item->path);
    co_return Result<void>{};
}
std::optional<RemoteCatalogs::Cached> RemoteCatalogs::cached(QUuid identity, QUuid contact, QString path) {
    if (stopped_) return {};
    prune();
    for (const auto &item : items_)
        if (item->context.identity == identity && item->context.contact == contact && item->path == path && item->cache) return item->cache;
    return {};
}
void RemoteCatalogs::failed(QUuid identity, QUuid contact, QByteArray transfer) {
    for (auto it = items_.begin(); it != items_.end(); ++it)
        if ((*it)->context.identity == identity && (*it)->context.contact == contact && (*it)->transfer == transfer) {
            auto item = *it; items_.erase(it); discard(*item); return;
        }
}
}
