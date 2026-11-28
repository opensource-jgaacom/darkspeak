#include <QtTest>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <ds/dsengine.h>
#include <ds/crypto.h>
#include <ds/managedstorage.h>
#include <ds/peer.h>
#include <ds/incomingtransfer.h>
#include <ds/outgoingtransfer.h>
#include <ds/fileshareservice.h>
#include <ds/remotecatalogs.h>
#include <dsfilemanager/readonlyfilemanager.h>
#include <algorithm>
#include <utility>
using namespace ds::core;
template<class T> T waitTask(QCoro::Task<T> task) {
    QEventLoop loop;
    std::optional<T> result;
    task.then([&](T value) { result.emplace(std::move(value)); loop.quit(); });
    if (!result) loop.exec();
    return std::move(*result);
}
class ReceivingPeer : public ds::prot::Peer {
  public:
    ReceivingPeer() : Peer(ds::prot::ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    void receive(quint32 channel, quint64 sequence, quint64 offset, const QByteArray &bytes, bool final) {
        QByteArray frame(file_chunk_header_bytes, Qt::Uninitialized);
        qToBigEndian(sequence, frame.data());
        qToBigEndian(offset, frame.data() + sizeof(quint64));
        frame.append(bytes);
        auto instance = inChannels_.at(channel);
        instance->onIncoming(*this, 0, mview_t{reinterpret_cast<uint8_t *>(frame.data()), size_t(frame.size())}, final);
    }
    uint64_t sendFileAck(const QByteArray &, quint64, quint64 offset, bool final) override {
        acknowledged = offset;
        completed = final;
        return 0;
    }
    quint64 acknowledged = 0;
    bool completed = false;
};
class OwnedReceivingPeer final : public ReceivingPeer {
public:
    ds::crypto::DsCert::ptr_t certificate;
    QUuid identity;
    ds::crypto::DsCert::ptr_t getPeerCert() const noexcept override { return certificate; }
    QUuid getIdentityId() const noexcept override { return identity; }
    File::ptr_t file;
    QByteArray payload;
    quint64 next_sequence = 0, next_offset = 0;
    bool sent_final = false, confirm_proposals = true, interrupt_checkpoint = false;
    int proposals = 0;
    size_t retainedCompletedCount() const { return completed_incoming_.size(); }
    size_t incomingChannelCount() const { return inChannels_.size(); }
    quint32 checkpointInterval() const noexcept override { return dsfm::minimum_checkpoint_interval; }
    uint64_t sendCheckpointOfferResponse(const QByteArray &, const dsfm::TransferAttempt &attempt) override {
        next_sequence = 0; sent_final = false;
        next_offset = checkpoint_fence_ = attempt.selected.offset;
        sendWindow(0); return 0;
    }
    uint64_t sendCheckpointResume(const QByteArray &, const QByteArray &, quint64, quint32 interval,
                                  QVector<dsfm::DurableCheckpoint> candidates, QDeadlineTimer) override {
        auto progress = waitTask(DsEngine::instance().getFileManager()->storage().transferProgress().load(file->checkpointKey().value()));
        const auto selected = *std::max_element(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.offset < b.offset; });
        auto session = DsEngine::instance().transfers().find(file->checkpointKey().value());
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Resume; event.request = 1;
        event.selection = dsfm::TransferAttempt{QUuid::createUuid(), interval, progress.value().next_checkpoint_sequence, selected};
        session->deliver(session->connection(), session->attempt(), std::move(event));
        return 1;
    }
    uint64_t sendCheckpointAck(const QByteArray &, QUuid, quint64 sequence, quint64 offset, bool final) override {
        acknowledged = offset; completed = final;
        if (!final) sendWindow(sequence);
        return 0;
    }
    uint64_t sendFileCheckpoint(const QByteArray &, const QByteArray &, const dsfm::TransferAttempt &,
                                dsfm::DurableCheckpoint checkpoint, dsfm::CheckpointPhase phase) override {
        if (phase == dsfm::CheckpointPhase::Proposed) {
            ++proposals;
            if (confirm_proposals) {
                auto session = DsEngine::instance().transfers().find(file->checkpointKey().value());
                TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
                event.checkpoint = checkpoint; event.checkpoint_phase = dsfm::CheckpointPhase::SenderConfirmed;
                session->deliver(session->connection(), session->attempt(), std::move(event));
            }
        } else if (phase == dsfm::CheckpointPhase::MutuallyConfirmed && !checkpoint.final) {
            checkpoint_fence_ = checkpoint.offset;
            if (interrupt_checkpoint) {
                DsEngine::instance().transfers().find(file->checkpointKey().value())->stop(TransferSession::Stop::Disconnected);
                return 0;
            }
            sendWindow(next_sequence);
        }
        return 0;
    }
    uint64_t sendFilePause(const QByteArray &, QUuid, dsfm::DurableCheckpoint, dsfm::TransferPauseReason) override { return 0; }
    uint64_t sendFileCancel(const QByteArray &, FileCancelReason) override { return 0; }
private:
    quint64 checkpoint_fence_ = 0;
    void sendWindow(quint64 acknowledged_sequence) {
        while (!sent_final && next_sequence - acknowledged_sequence < file_window_chunks &&
               next_offset - checkpoint_fence_ < dsfm::minimum_checkpoint_interval) {
            const auto length = qMin<quint64>(file_chunk_bytes, quint64(payload.size()) - next_offset);
            const auto offset = next_offset, sequence = next_sequence++;
            next_offset += length;
            sent_final = next_offset == quint64(payload.size());
            ReceivingPeer::receive(file->getChannel(), sequence, offset, payload.mid(qsizetype(offset), qsizetype(length)), sent_final);
        }
    }
};
class SendingPeer final : public ds::prot::Peer {
public:
    SendingPeer() : Peer(ds::prot::ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    quint32 checkpointInterval() const noexcept override { return dsfm::minimum_checkpoint_interval; }
    void begin(const File::ptr_t &file) {
        file_ = file;
        owner_ = std::make_shared<OutgoingTransfer>(file, std::static_pointer_cast<PeerConnection>(shared_from_this()), OutgoingTransferStart::Offer);
        if (!DsEngine::instance().transfers().acceptWork(*file)) throw std::runtime_error("Test work acceptance failed");
        if (!DsEngine::instance().transfers().submit(owner_)) throw std::runtime_error("Test sender admission failed");
    }
    uint64_t offerFile(const File &) override {
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ready; event.selection = file_->checkpointAttempt();
        owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
        return 0;
    }
    uint64_t sendCheckpointChunk(const QByteArray &, QUuid, quint64 sequence, quint64 offset, const QByteArray &bytes, bool final) override {
        sent.append(bytes); next_sequence_ = sequence + 1; next_offset_ = offset + quint64(bytes.size()); final_sent = final;
        if (!final && mutate) std::exchange(mutate, {})();
        if (!final) ack(false);
        if (final || next_offset_ % dsfm::minimum_checkpoint_interval == 0) {
            TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
            event.checkpoint = {file_->getSize() == 0 ? 0 : checkpoint_sequence_++, next_offset_,
                QCryptographicHash::hash(sent, QCryptographicHash::Sha256), final, false};
            event.checkpoint_phase = dsfm::CheckpointPhase::Proposed;
            owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
        }
        return 0;
    }
    uint64_t sendFileCheckpoint(const QByteArray &, const QByteArray &, const dsfm::TransferAttempt &,
                                dsfm::DurableCheckpoint checkpoint, dsfm::CheckpointPhase) override {
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
        event.checkpoint = checkpoint; event.checkpoint_phase = dsfm::CheckpointPhase::MutuallyConfirmed;
        owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
        if (checkpoint.final) ack(true);
        return 0;
    }
    uint64_t sendFileCancel(const QByteArray &, FileCancelReason reason) override { cancelled = reason == FileCancelReason::SourceChanged; return 0; }
    uint64_t sendFilePause(const QByteArray &, QUuid, dsfm::DurableCheckpoint, dsfm::TransferPauseReason) override { return 0; }
    QByteArray sent;
    bool final_sent = false, cancelled = false;
    std::function<void()> mutate;
private:
    void ack(bool final) {
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ack;
        event.sequence = next_sequence_; event.offset = next_offset_; event.final = final;
        owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
    }
    File::ptr_t file_;
    std::shared_ptr<OutgoingTransfer> owner_;
    quint64 next_sequence_ = 0, next_offset_ = 0, checkpoint_sequence_ = 1;
};

class CatalogTransferTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(directory_.isValid());
        qputenv("DARKSPEAK_PROFILE_ROOT", directory_.path().toUtf8());
        qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE", "managed-storage-disposable-fixture");
        crypto_ = std::make_unique<ds::crypto::Crypto>();
        auto settings = std::make_unique<QSettings>(directory_.filePath("settings.ini"), QSettings::IniFormat);
        settings->setValue("dbpath", directory_.filePath("app.sqlite"));
        settings->setValue("downloadLocation", directory_.filePath("downloads"));
        QDir().mkdir(directory_.filePath("downloads"));
        engine_ = std::make_unique<DsEngine>(std::move(settings));
        auto initialized = waitTask(storage().ready());
        QVERIFY2(initialized, "Managed namespaces failed to initialize");
        IdentityData identity;
        identity.uuid = QUuid::createUuid();
        identity.name = "Storage fixture";
        identity.cert = ds::crypto::DsCert::create();
        identity.hash = identity.cert->getHash().toByteArray();
        identity.autoConnect = false;
        identity_ = engine_->getIdentityManager()->addIdentity(identity);
        auto contact = std::make_unique<ContactData>();
        contact->identity = identity_->getId();
        contact->name = "Fixture peer";
        const auto cert = ds::crypto::DsCert::create();
        contact->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        contact->address = "fixture-not-networked";
        contact->autoConnect = false;
        contact_ = engine_->getContactManager()->addContact(std::move(contact));
        conversation_ = engine_->getConversationManager()->getConversation(contact_);
    }

    void recursive_catalog_uses_generic_transport_and_private_validated_cache() {
        contact_->setState(Contact::ACCEPTED);
        auto opened = waitTask(storage().openVfs()); QVERIFY(opened); auto* vfs = opened.value();
        QTemporaryDir external; QVERIFY(external.isValid());
        QVERIFY(QDir().mkdir(external.filePath("dir")));
        for (int i = 0; i < 30; ++i) {
            QFile file(external.filePath(QStringLiteral("dir/entry-%1").arg(i, 3, 10, QChar('0'))));
            QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("hello"), 5);
        }
        dsfm::ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), external.path()};
        auto source = waitTask(storage().openReadOnlySource(config, true)); QVERIFY(source);
        auto* backend = storage().readOnlyManager(); QVERIFY(backend);
        bool ready = false;
        for (int turn = 0; turn < 1000; ++turn) {
            auto info = waitTask(backend->namespaceInfo(config.common.id)); QVERIFY(info);
            if (info.value().status == dsfm::NamespaceStatus::Ready) { ready = true; break; }
        }
        QVERIFY(ready);
        auto generation = std::make_shared<std::atomic<quint64>>(1);
        dsvfs::Request request; request.scope = contact_->getUuid(); request.generation = 1; request.current_generation = generation;
        request.authorize = [](dsvfs::Target, dsvfs::Operation) -> dsfm::Result<bool> { return true; };
        auto policy = std::make_shared<dsvfs::PermissionPolicy>();
        for (int op = 0; op <= int(dsvfs::Operation::Recurse); ++op)
            policy->defaults.insert(dsvfs::Operation(op), dsvfs::Decision::Allow);
        QVERIFY(policy->validate()); request.permissions = policy;
        request.commit_permit = [](dsvfs::Target, dsvfs::Operation) -> dsfm::Result<std::shared_ptr<void>> {
            return std::static_pointer_cast<void>(std::make_shared<int>(1));
        };
        const auto identity = identity_->getUuid();
        auto resource = waitTask(vfs->addResource({{}, config.common.id, {}}, request)); QVERIFY(resource);
        auto assignment = waitTask(vfs->assignStorage({{}, identity, resource.value().id, source.value().root, false}, request)); QVERIFY(assignment);
        auto mount = waitTask(vfs->addMount(identity, {{}, {}, config.common.id, source.value().root, "share", {}}, request)); QVERIFY(mount);
        bool connected = true;
        ds::prot::FileShareService::Host host;
        host.open = [vfs]() -> QCoro::Task<dsfm::Result<dsvfs::Vfs*>> { co_return vfs; };
        host.request = [request](QUuid, QUuid) -> QCoro::Task<dsfm::Result<dsvfs::Request>> { co_return request; };
        host.enabled = [](QUuid) { return true; };
        host.approve_recursion = [](QUuid, QUuid, const QString&) { return true; };
        ds::prot::FileShareService service(std::move(host));
        ds::prot::RemoteCatalogs remote;
        remote.install(*engine_->getFileManager(), service);
        auto sender = std::make_shared<SendingPeer>();
        auto receiver = std::make_shared<OwnedReceivingPeer>();
        receiver->identity = identity; receiver->certificate = contact_->getCert();
        File::ptr_t outgoing;
        std::shared_ptr<dsvfs::CatalogExport> staged;
        auto session = service.bind({identity, contact_->getUuid(), receiver->getConnectionId(),
            [&] { return connected; }, [&] { return connected; }, [] { return false; },
            [&](std::shared_ptr<dsvfs::CatalogExport> catalog) -> dsfm::Result<ds::prot::FileShareService::CatalogDelivery> {
                staged = catalog;
                auto result = engine_->getFileManager()->addCatalogTransfer(*conversation_, std::move(catalog), false);
                if (!result) return result.error();
                outgoing = result.value();
                return ds::prot::FileShareService::CatalogDelivery{outgoing->getFileId(),
                    [&, file = outgoing]() -> dsfm::Result<void> { sender->begin(file); return {}; },
                    [file = outgoing] { file->cancel(); }};
            }});
        ds::protocol::Envelope control;
        ds::prot::RemoteCatalogs::Context context{identity, contact_->getUuid(), receiver->getConnectionId(), false,
            [&] { return connected; }, [&](ds::protocol::Envelope envelope) {
                envelope.setRequestId(91); control = envelope; return quint64(91);
            }};
        auto requested = remote.request(context, "/share"); QVERIFY(requested);
        auto response = waitTask(service.handle(session, control)); QVERIFY(response);
        QCOMPARE(int(response->listDirectoryResult().status()), 1);
        QCOMPARE(response->listDirectoryResult().catalogTransferId(), outgoing->getFileId());
        QVERIFY(outgoing->hasCatalogSource()); QVERIFY(outgoing->storageId().isNull());
        QVERIFY(sender->sent.isEmpty()); // control acceptance must precede the generic offer
        remote.response(receiver->getConnectionId(), *response);
        QVERIFY(service.publishCatalog(session, control.requestId()));
        QTRY_COMPARE_WITH_TIMEOUT(outgoing->getState(), File::FS_DONE, 10000);
        QVERIFY(sender->final_sent); QVERIFY(!sender->sent.isEmpty());
        QCOMPARE(QCryptographicHash::hash(sender->sent, QCryptographicHash::Sha256), staged->sha256());
        QVERIFY(!remote.cached(identity, contact_->getUuid(), "/share"));
        QSignalSpy published(&remote, &ds::prot::RemoteCatalogs::catalogReady);
        PeerFileOffer offer(receiver, receiver->getConnectionId(), 1, {}, outgoing->getFileId(), "catalog.jsonl",
            sender->sent.size(), "application/vnd.darkspeak.vfs-catalog+jsonl", outgoing->getHash());
        auto wrong_connection = offer; wrong_connection.connectionId = QUuid::createUuid();
        QVERIFY(!engine_->getFileManager()->incomingCatalogAdmission(wrong_connection));
        auto unsolicited = offer; unsolicited.fileId = QUuid::createUuid().toRfc4122();
        QVERIFY(!engine_->getFileManager()->incomingCatalogAdmission(unsolicited));
        QVERIFY(engine_->getFileManager()->incomingCatalogAdmission(offer));
        auto data = std::make_unique<FileData>();
        data->direction = File::INCOMING; data->state = File::FS_OFFERED;
        data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->fileId = outgoing->getFileId(); data->name = "catalog.jsonl";
        data->mime_type = "application/vnd.darkspeak.vfs-catalog+jsonl";
        data->size = sender->sent.size(); data->hash = outgoing->getHash(); data->offered_connection = receiver->getConnectionId();
        auto incoming = engine_->getFileManager()->addFile(std::move(data)); QVERIFY(incoming);
        receiver->file = incoming; receiver->payload = sender->sent;
        auto progress = waitTask(incoming->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval)); QVERIFY(progress);
        dsfm::TransferAttempt attempt{QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1, progress.value().checkpoints.first()};
        QVERIFY(incoming->setCheckpointAttempt(attempt));
        auto transfer = std::make_shared<IncomingTransfer>(incoming, receiver, IncomingTransferStart::Accept);
        QVERIFY(engine_->transfers().acceptWork(*incoming));
        QVERIFY(engine_->transfers().submit(transfer));
        QTRY_VERIFY_WITH_TIMEOUT(transfer->outcome(), 10000);
        QCOMPARE(transfer->outcome()->kind, TransferSession::Outcome::Kind::Completed);
        QCOMPARE(incoming->getState(), File::FS_DONE); QVERIFY(receiver->completed);
        QCOMPARE(incoming->storageNamespace(), storage().namespaceId(ManagedStorage::Private));
        QVERIFY(!storage().isDownloadNamespace(incoming->storageNamespace()));
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 10000);
        auto cached = remote.cached(identity, contact_->getUuid(), "/share"); QVERIFY(cached);
        QCOMPARE(cached->file, incoming->storageId()); QCOMPARE(cached->namespace_id, incoming->storageNamespace());
        QCOMPARE(cached->sha256, outgoing->getHash());
        auto read = waitTask(storage().manager().openRead(cached->namespace_id, cached->file)); QVERIFY(read);
        QByteArray bytes;
        while (bytes.size() < sender->sent.size()) {
            auto part = waitTask(read.value()->readAt(bytes.size(), 4096)); QVERIFY(part); QVERIFY(!part.value().isEmpty()); bytes += part.value();
        }
        QCOMPARE(bytes, sender->sent); QVERIFY(waitTask(read.value()->close()));
        auto download_space = waitTask(storage().manager().namespaceInfo(storage().namespaceId(ManagedStorage::Downloads))); QVERIFY(download_space);
        auto downloads = waitTask(storage().manager().list(download_space.value().config.id, download_space.value().root)); QVERIFY(downloads);
        QVERIFY(downloads.value().entries.isEmpty());
        remote.stop(); service.stop(); QVERIFY(waitTask(staged->close()));
    }
    void invalid_catalog_is_withheld_after_successful_byte_transfer() {
        contact_->setState(Contact::ACCEPTED);
        ds::prot::FileShareService service({});
        ds::prot::RemoteCatalogs remote; remote.install(*engine_->getFileManager(), service);
        auto receiver = std::make_shared<OwnedReceivingPeer>();
        receiver->identity = identity_->getUuid(); receiver->certificate = contact_->getCert();
        bool connected = true;
        const auto transfer_id = QUuid::createUuid().toRfc4122();
        const QByteArray payload("not a catalog\n");
        const auto digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
        ds::prot::RemoteCatalogs::Context context{identity_->getUuid(), contact_->getUuid(), receiver->getConnectionId(), false,
            [&] { return connected; }, [](ds::protocol::Envelope) { return quint64(92); }};
        QVERIFY(remote.request(context, "/bad"));
        ds::protocol::VirtualPath path; path.setPath("/bad"); path.setEncoding(ds::protocol::VirtualPathEncodingGadget::VirtualPathEncoding(2));
        ds::protocol::ListDirectoryResult result;
        result.setStatus(ds::protocol::FileShareStatusGadget::FileShareStatus(1)); result.setDirectory(path);
        result.setCatalogTransferId(transfer_id); result.setCatalogFormatVersion(2);
        ds::protocol::Envelope response; response.setProtocolVersion(3); response.setRequestId(1); response.setResponseToRequestId(92);
        response.setListDirectoryResult(result); remote.response(receiver->getConnectionId(), response);
        PeerFileOffer offer(receiver, receiver->getConnectionId(), 1, {}, transfer_id, "catalog.jsonl", payload.size(),
            "application/vnd.darkspeak.vfs-catalog+jsonl", digest);
        QVERIFY(engine_->getFileManager()->incomingCatalogAdmission(offer));
        auto data = std::make_unique<FileData>();
        data->direction = File::INCOMING; data->state = File::FS_OFFERED;
        data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->fileId = transfer_id; data->name = "catalog.jsonl";
        data->mime_type = "application/vnd.darkspeak.vfs-catalog+jsonl";
        data->size = payload.size(); data->hash = digest; data->offered_connection = receiver->getConnectionId();
        auto incoming = engine_->getFileManager()->addFile(std::move(data)); QVERIFY(incoming);
        receiver->file = incoming; receiver->payload = payload;
        auto progress = waitTask(incoming->prepareCheckpointProgress(dsfm::minimum_checkpoint_interval)); QVERIFY(progress);
        QVERIFY(incoming->setCheckpointAttempt({QUuid::createUuid(), dsfm::minimum_checkpoint_interval, 1, progress.value().checkpoints.first()}));
        QSignalSpy published(&remote, &ds::prot::RemoteCatalogs::catalogReady);
        auto session = std::make_shared<IncomingTransfer>(incoming, receiver, IncomingTransferStart::Accept);
        QVERIFY(engine_->transfers().acceptWork(*incoming));
        QVERIFY(engine_->transfers().submit(session));
        QTRY_VERIFY_WITH_TIMEOUT(session->outcome(), 10000);
        QCOMPARE(session->outcome()->kind, TransferSession::Outcome::Kind::Completed); // bytes are intact
        QCOMPARE(incoming->storageNamespace(), storage().namespaceId(ManagedStorage::Private));
        QTRY_VERIFY_WITH_TIMEOUT(!engine_->getFileManager()->getFileFromId(transfer_id, *contact_, File::INCOMING), 10000);
        QCOMPARE(published.count(), 0);
        QVERIFY(!remote.cached(identity_->getUuid(), contact_->getUuid(), "/bad"));
        remote.stop(); service.stop();
    }
    void cleanupTestCase() {
        auto joined = engine_->transfers().shutdown(); QTRY_VERIFY_WITH_TIMEOUT(joined.isReady(), 10000);
        QVERIFY(waitTask(storage().shutdown()));
        conversation_.reset(); engine_.reset();
    }
private:
    ManagedStorage& storage() { return engine_->getFileManager()->storage(); }
    QTemporaryDir directory_;
    std::unique_ptr<ds::crypto::Crypto> crypto_;
    std::unique_ptr<DsEngine> engine_;
    Identity* identity_ = nullptr;
    Contact* contact_ = nullptr;
    Conversation::ptr_t conversation_;
};
QTEST_GUILESS_MAIN(CatalogTransferTest)
#include "catalog_transfer_test.moc"
