#include <ds/transfermanager.h>
#include <ds/transferqueuemodel.h>
#include <ds/dsengine.h>
#include <ds/filemanager.h>
#include <ds/incomingtransfer.h>
#include <ds/outgoingtransfer.h>
#include <ds/peer.h>
#include <ds/protocolcodec.h>
#include <ds/crypto.h>
#include <QCryptographicHash>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QFile>
#include <QScopeGuard>
#include <QtTest>
#include <QtEndian>
using namespace ds::core;
using namespace dsfm;

template<class T> T settle(QCoro::Task<T> task) { return QCoro::waitFor(std::move(task)); }

// A bounded peer counterpart, exercising production sender I/O and its journal.
class PauseReceivingPeer final : public ds::prot::Peer {
public:
    PauseReceivingPeer() : Peer(ds::prot::ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    quint32 checkpointInterval() const noexcept override { return minimum_checkpoint_interval; }
    File::ptr_t file;
    std::shared_ptr<OutgoingTransfer> owner;
    QByteArray sent;
    DurableCheckpoint fenced;
    bool request_pause = true, remote_request = false, wrong_ack = false, withhold_ack = false;
    bool final_pause = false;
    int notices = 0, fences = 0,accepted_resumes = 0,rejected_resumes = 0;
    quint64 resumed_offset = 0;
    std::function<void()> on_fence,after_pause_ack;
    uint64_t sendUserInfo(const UserInfo &) override { return 0; }
    void begin() {
        owner = std::make_shared<OutgoingTransfer>(file, shared_from_this(), OutgoingTransferStart::Offer);
        if (!DsEngine::instance().transfers().acceptWork(*file) || !DsEngine::instance().transfers().submit(owner)) qFatal("sender enrollment");
    }
    void resume(DurableTransferProgress saved) {
        request_pause = false;
        TransferSession::Event request; request.kind = TransferSession::Event::Kind::Resume;
        request.request = 9; request.candidates = saved.checkpoints;
        owner = std::make_shared<OutgoingTransfer>(file, shared_from_this(), OutgoingTransferStart::Resume, request);
        auto &m = DsEngine::instance().transfers();
        if (!m.pauseQueuedWork(owner->key(),false) || !m.workStore()->project(owner->key(),WorkState::Queued,true) || !m.submit(owner)) qFatal("resume enrollment");
    }
    void deliver(TransferSession::Event event) { if (!owner->deliver(owner->connection(),owner->attempt(),std::move(event))) qFatal("sender event refused"); }
    uint64_t offerFile(const File &) override {
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ready; event.selection = file->checkpointAttempt(); deliver(event); return 0;
    }
    uint64_t sendCheckpointResumeResult(quint64,const QByteArray &,FileResumeStatus status,
            std::optional<TransferAttempt> selected,TransferPauseReason,FileCancelReason) override {
        if (status == FileResumeStatus::Rejected) ++rejected_resumes;
        if (selected) {
            ++accepted_resumes;
            owner=std::dynamic_pointer_cast<OutgoingTransfer>(DsEngine::instance().transfers().find(file->checkpointKey().value())); if (!owner) qFatal("resume response has no current owner");
            resumed_offset = next_offset_ = selected->selected.offset; sent.truncate(qsizetype(next_offset_)); checkpoint_sequence_ = selected->next_sequence;
            TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ready; event.selection = selected; deliver(event);
        }
        return 0;
    }
    uint64_t sendCheckpointChunk(const QByteArray &,QUuid,quint64 sequence,quint64 offset,const QByteArray &bytes,bool final) override {
        sent.append(bytes); next_sequence_ = sequence + 1; next_offset_ = offset + quint64(bytes.size());
        if (request_pause && (!final || final_pause)) { request_pause = false; if (!owner->requestCooperativePause(!remote_request)) qFatal("pause refused"); }
        if (!final) ack(false);
        if (final || next_offset_ % minimum_checkpoint_interval == 0) {
            TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
            event.checkpoint = {file->getSize() == 0 ? 0 : checkpoint_sequence_++,next_offset_,QCryptographicHash::hash(sent,QCryptographicHash::Sha256),final,false};
            event.checkpoint_phase = CheckpointPhase::Proposed; deliver(event);
        }
        return 0;
    }
    uint64_t sendFileCheckpoint(const QByteArray &,const QByteArray &,const TransferAttempt &,DurableCheckpoint checkpoint,CheckpointPhase) override {
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
        event.checkpoint = checkpoint; event.checkpoint_phase = CheckpointPhase::MutuallyConfirmed; deliver(event);
        if (checkpoint.final) ack(true);
        return 0;
    }
    uint64_t sendCooperativePause(const QByteArray &,QUuid,DurableCheckpoint checkpoint,int phase) override {
        if (phase != 3) qFatal("unexpected sender pause phase");
        ++fences; fenced = checkpoint;
        if (on_fence) on_fence();
        if (!withhold_ack && owner->accepting()) {
            TransferSession::Event event; event.kind = TransferSession::Event::Kind::Pause; event.sequence = 4; event.checkpoint = checkpoint;
            if (wrong_ack) event.checkpoint.sha256 = QByteArray(32,'x');
            deliver(event);
            if (after_pause_ack) after_pause_ack();
        }
        return 0;
    }
    uint64_t sendFilePause(const QByteArray &,QUuid,DurableCheckpoint,TransferPauseReason) override { ++notices; return 0; }
    uint64_t sendFileCancel(const QByteArray &,FileCancelReason) override { return 0; }
private:
    quint64 next_sequence_ = 0,next_offset_ = 0,checkpoint_sequence_ = 1;
    void ack(bool final) { TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ack;
        event.sequence = next_sequence_; event.offset = next_offset_; event.final = final; deliver(event); }
};

// Counterpart for the production receiver. It continues ordinary windows after
// REQUEST and fences exactly the next mutually confirmed normal checkpoint.
class PauseSendingPeer final : public ds::prot::Peer {
public:
    PauseSendingPeer() : Peer(ds::prot::ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    quint32 checkpointInterval() const noexcept override { return minimum_checkpoint_interval; }
    File::ptr_t file;
    QByteArray payload;
    std::shared_ptr<IncomingTransfer> owner;
    bool pause = true, requested = false, acknowledged = false, final_ack = false, malformed = false;
    quint64 offset = 0,resumed_offset = 0;
    DurableCheckpoint fenced;
    uint64_t sendCheckpointOfferResponse(const QByteArray &,const TransferAttempt &a) override {
        sequence_ = 0; offset = fence_ = a.selected.offset; resumed_offset = offset; sendWindow(0); return 0;
    }
    uint64_t sendCheckpointResume(const QByteArray &,const QByteArray &,quint64,quint32 interval,
                                  QVector<DurableCheckpoint> candidates,QDeadlineTimer) override {
        auto saved = settle(DsEngine::instance().getFileManager()->storage().transferProgress().load(file->checkpointKey().value()));
        const auto selected = *std::max_element(candidates.begin(),candidates.end(),[](const auto &a,const auto &b) { return a.offset < b.offset; });
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Resume; event.request = 1;
        event.selection = TransferAttempt{QUuid::createUuid(),interval,saved.value().next_checkpoint_sequence,selected}; deliver(event); return 1;
    }
    uint64_t sendCheckpointAck(const QByteArray &,QUuid,quint64 sequence,quint64,bool final) override {
        final_ack = final; if (!final) sendWindow(sequence); return 0;
    }
    uint64_t sendFileCheckpoint(const QByteArray &,const QByteArray &,const TransferAttempt &,DurableCheckpoint checkpoint,CheckpointPhase phase) override {
        if (phase == CheckpointPhase::Proposed) {
            if (pause && !requested) { if (!owner->requestCooperativePause()) qFatal("receiver pause refused"); }
            TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
            event.checkpoint = checkpoint; event.checkpoint_phase = CheckpointPhase::SenderConfirmed; deliver(event);
        } else if (!checkpoint.final) {
            fence_ = checkpoint.offset;
            if (requested) {
                fenced = checkpoint;
                TransferSession::Event event; event.kind = TransferSession::Event::Kind::Pause; event.sequence = 3; event.checkpoint = checkpoint;
                if (malformed) event.checkpoint.sha256 = QByteArray(32,'x');
                deliver(event);
            } else sendWindow(sequence_);
        }
        return 0;
    }
    uint64_t sendCooperativePause(const QByteArray &,QUuid,DurableCheckpoint checkpoint,int phase) override {
        if (phase == 2) requested = true;
        else if (phase == 4) { acknowledged = checkpoint == fenced; }
        else qFatal("unexpected receiver pause phase");
        return 0;
    }
    uint64_t sendFilePause(const QByteArray &,QUuid,DurableCheckpoint,TransferPauseReason) override { return 0; }
    uint64_t sendFileCancel(const QByteArray &,FileCancelReason) override { return 0; }
private:
    quint64 sequence_ = 0,fence_ = 0;
    void deliver(TransferSession::Event event) { if (!owner->deliver(owner->connection(),owner->attempt(),std::move(event))) qFatal("receiver event refused"); }
    void sendWindow(quint64 acknowledged_sequence) {
        while (offset < quint64(payload.size()) && sequence_ - acknowledged_sequence < file_window_chunks && offset - fence_ < minimum_checkpoint_interval) {
            const auto count = qMin<quint64>(file_chunk_bytes,quint64(payload.size()) - offset);
            TransferSession::Event event; event.kind = TransferSession::Event::Kind::Chunk; event.sequence = sequence_++;
            event.offset = offset; event.bytes = payload.mid(qsizetype(offset),qsizetype(count)); offset += count;
            event.final = offset == quint64(payload.size()); deliver(event);
        }
    }
};

class YieldSession final : public TransferSession {
public:
    YieldSession(TransferKey key,int file) : TransferSession(key,QUuid::createUuid(),QUuid::createUuid(),file) {}
    bool supportsCooperativePause() const override { return true; }
    bool entered = false,requested = false;
    void completePause() { stop(Stop::Paused,TransferPauseReason::FeaturePaused); }
protected:
    void cooperativePauseRequested() override { requested = true; }
    QCoro::Task<Outcome> execute() override { entered = true; setPhase(Phase::Transferring); while (co_await nextEvent()) {} co_return interrupted(); }
    QCoro::Task<Outcome> finalize(Outcome o) override { co_return o; }
};

class TransferPreemptionTest final : public QObject {
    Q_OBJECT
    QTemporaryDir profile_;
    std::unique_ptr<ds::crypto::Crypto> crypto_;
    std::unique_ptr<DsEngine> engine_;
    Identity *identity_ = nullptr;
    Contact *contact_ = nullptr;
    Conversation::ptr_t conversation_;
    QByteArray payload(int n = 2*minimum_checkpoint_interval + 37) { return QByteArray(n,'p'); }
    File::ptr_t outgoing(const QByteArray &bytes) {
        QFile input(profile_.filePath(QUuid::createUuid().toString()+".bin"));
        if (!input.open(QIODevice::WriteOnly) || input.write(bytes) != bytes.size()) qFatal("source fixture"); input.close();
        auto imported = settle(engine_->getFileManager()->storage().importFile(input.fileName(),"payload.bin")); if (!imported) qFatal("import fixture");
        auto data = fileData(bytes); data->direction = File::OUTGOING; data->state = File::FS_WAITING;
        data->storage_namespace = imported.value().namespace_id; data->storage_id = imported.value().file.id;
        auto file = engine_->getFileManager()->addFile(std::move(data));
        if (!engine_->transfers().bindWork(file,TransferPurpose::FileShare)) qFatal("source binding"); return file;
    }
    std::unique_ptr<FileData> fileData(const QByteArray &bytes) {
        auto data = std::make_unique<FileData>(); data->identity = identity_->getId(); data->contact = contact_->getId(); data->conversation = conversation_->getId();
        data->name = "payload.bin"; data->mime_type = "application/octet-stream"; data->size = bytes.size(); data->hash = QCryptographicHash::hash(bytes,QCryptographicHash::Sha256); return data;
    }
private slots:
    void initTestCase() {
        QVERIFY(profile_.isValid()); qputenv("DARKSPEAK_PROFILE_ROOT",profile_.path().toUtf8()); qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE","disposable-preemption-test");
        crypto_ = std::make_unique<ds::crypto::Crypto>();
        auto settings = std::make_unique<QSettings>(profile_.filePath("settings.ini"),QSettings::IniFormat);
        settings->setValue("dbpath",profile_.filePath("app.sqlite")); settings->setValue("downloadLocation",profile_.filePath("downloads")); QDir().mkpath(profile_.filePath("downloads"));
        engine_ = std::make_unique<DsEngine>(std::move(settings)); QVERIFY(settle(engine_->getFileManager()->storage().ready()));
        IdentityData data; data.uuid = QUuid::createUuid(); data.name = "Pause fixture"; data.cert = ds::crypto::DsCert::create(); data.hash = data.cert->getHash().toByteArray(); data.autoConnect = false;
        identity_ = engine_->getIdentityManager()->addIdentity(data); identity_->setAllowFiles(true);
        auto peer = std::make_unique<ContactData>(); peer->identity = identity_->getId(); peer->state = Contact::ACCEPTED;
        auto cert = ds::crypto::DsCert::create(); peer->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray()); peer->name = "Peer"; peer->address = "fixture-no-network"; peer->autoConnect = false;
        contact_ = engine_->getContactManager()->addContact(std::move(peer)); conversation_ = engine_->getConversationManager()->getConversation(contact_);
    }
    void versionedWireAndBounds() {
        using namespace ds::prot::codec;
        ds::protocol::FilePause pause; pause.setFileId(QByteArray(16,'f')); pause.setAttemptId(QByteArray(16,'a'));
        pause.setVersion(1); pause.setPhase(ds::protocol::FilePause::Phase::FENCED);
        pause.setReason(ds::protocol::FilePauseReasonGadget::FilePauseReason::TRANSFER_FEATURE_PAUSED);
        pause.setConfirmedCheckpoint(checkpointReference({1,minimum_checkpoint_interval,QByteArray(32,'h'),false,false}));
        QVERIFY(validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval));
        ds::protocol::Envelope envelope; envelope.setProtocolVersion(protocolVersion); envelope.setFilePause(pause);
        auto encoded = serialize(envelope); QFile fixture(QStringLiteral(DS_PAUSE_FIXTURE)); QVERIFY(fixture.open(QIODevice::ReadOnly)); QCOMPARE(encoded,QByteArray::fromHex(fixture.readAll().trimmed())); ds::protocol::Envelope decoded; QVERIFY(deserialize(encoded,decoded)); QCOMPARE(decoded.filePause(),pause);
        pause.setVersion(0); QVERIFY(!validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval)); pause.setVersion(1);
        pause.setPhase(ds::protocol::FilePause::Phase::PHASE_UNSPECIFIED); QVERIFY(!validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval)); pause.setPhase(ds::protocol::FilePause::Phase::ACK);
        auto reference = pause.confirmedCheckpoint(); reference.setFinal(true); pause.setConfirmedCheckpoint(reference); QVERIFY(!validateFilePause(pause,minimum_checkpoint_interval,minimum_checkpoint_interval));
        reference.setFinal(false); reference.setPauseBoundary(true); pause.setConfirmedCheckpoint(reference); QVERIFY(!validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval));
        reference.setPauseBoundary(false); reference.setNextOffset(minimum_checkpoint_interval+1); pause.setConfirmedCheckpoint(reference); QVERIFY(!validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval));
        reference.setNextOffset(0); reference.setSequence(0); reference.setPrefixSha256(QCryptographicHash::hash(QByteArray{},QCryptographicHash::Sha256)); pause.setConfirmedCheckpoint(reference);
        QVERIFY(!validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval));
        pause.setPhase(ds::protocol::FilePause::Phase::REQUEST); QVERIFY(validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval));
        pause.setPhase(ds::protocol::FilePause::Phase::FENCED); reference = checkpointReference({1,3*minimum_checkpoint_interval,QByteArray(32,'h'),false,false}); pause.setConfirmedCheckpoint(reference);
        QVERIFY(!validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval));
        reference = checkpointReference({1,minimum_checkpoint_interval,QByteArray(32,'h'),false,false}); pause.setConfirmedCheckpoint(reference);
        pause.setPhase(ds::protocol::FilePause::Phase(99)); QVERIFY(!validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval));
        pause.setPhase(ds::protocol::FilePause::Phase::ACK);
        pause.setReason(ds::protocol::FilePauseReasonGadget::FilePauseReason::SOURCE_UNAVAILABLE); QVERIFY(!validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval));
        pause.setPhase(ds::protocol::FilePause::Phase::NOTICE); QVERIFY(validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval));
        pause.setAttemptId(QByteArray(16,'\0')); QVERIFY(!validateFilePause(pause,3*minimum_checkpoint_interval,minimum_checkpoint_interval));
    }
    void senderPauseResume_data() { QTest::addColumn<bool>("remote"); QTest::newRow("local-sender") << false; QTest::newRow("receiver-request") << true; }
    void senderPauseResume() {
        QFETCH(bool,remote); auto bytes = payload(); auto peer = std::make_shared<PauseReceivingPeer>(); peer->file = outgoing(bytes); peer->remote_request = remote; peer->begin();
        QTRY_VERIFY_WITH_TIMEOUT(peer->owner->outcome(),10000); QCOMPARE(peer->owner->outcome()->pause,TransferPauseReason::FeaturePaused);
        QTRY_VERIFY(!engine_->transfers().find(peer->owner->key()));
        QTRY_VERIFY(engine_->transfers().workStore()->find(peer->owner->key()).value().eligible);
        auto work = engine_->transfers().workStore()->find(peer->owner->key()).value(); QVERIFY(work.resume_requested); QVERIFY(!work.user_paused); QCOMPARE(work.state,WorkState::Paused); QCOMPARE(peer->file->getState(),File::FS_PAUSED);
        QCOMPARE(peer->fences,1); QCOMPARE(peer->notices,0); QCOMPARE(peer->sent.size(),qsizetype(minimum_checkpoint_interval));
        QCOMPARE(peer->fenced.offset,quint64(minimum_checkpoint_interval));
        auto saved = settle(engine_->getFileManager()->storage().transferProgress().load(peer->owner->key())); QVERIFY(saved); QCOMPARE(saved.value().verified_prefix,quint64(minimum_checkpoint_interval));
        QCOMPARE(engine_->transfers().active(identity_->getUuid(),contact_->getUuid()),size_t(0));
        peer->resume(saved.value()); QTRY_VERIFY_WITH_TIMEOUT(peer->owner->outcome(),10000); QCOMPARE(peer->owner->outcome()->kind,TransferSession::Outcome::Kind::Completed);
        QTRY_VERIFY(!engine_->transfers().find(peer->owner->key())); QCOMPARE(peer->resumed_offset,quint64(minimum_checkpoint_interval)); QCOMPARE(peer->sent,bytes);
        QCOMPARE(QCryptographicHash::hash(peer->sent,QCryptographicHash::Sha256),peer->file->getHash());
    }
    void senderMalformedAckAndRevocation_data() { QTest::addColumn<int>("mode"); QTest::newRow("wrong-digest") << 0; QTest::newRow("authorization-withdrawal") << 1; QTest::newRow("bounded-timeout-command") << 2; }
    void senderMalformedAckAndRevocation() {
        QFETCH(int,mode); auto peer = std::make_shared<PauseReceivingPeer>(); peer->file = outgoing(payload()); peer->wrong_ack = mode == 0;
        if (mode == 1) peer->on_fence = [this] { contact_->setBlocked(true); };
        if (mode == 2) { peer->withhold_ack = true; peer->on_fence = [weak=std::weak_ptr(peer)] { if (auto p=weak.lock()) p->owner->stop(TransferSession::Stop::Deadline); }; }
        peer->begin(); QTRY_VERIFY_WITH_TIMEOUT(peer->owner->outcome(),10000); QVERIFY(peer->owner->outcome()->kind != TransferSession::Outcome::Kind::Completed);
        QTRY_VERIFY(!engine_->transfers().find(peer->owner->key())); QCOMPARE(peer->sent.size(),qsizetype(minimum_checkpoint_interval));
        auto saved = settle(engine_->getFileManager()->storage().transferProgress().load(peer->owner->key())); QVERIFY(saved); QCOMPARE(saved.value().verified_prefix,quint64(minimum_checkpoint_interval));
        QCOMPARE(engine_->transfers().active(identity_->getUuid(),contact_->getUuid()),size_t(0));
        contact_->setBlocked(false);
    }
    void finalPublicationWins() {
        auto peer = std::make_shared<PauseReceivingPeer>(); peer->file = outgoing(payload(37)); peer->final_pause = true; peer->begin();
        QTRY_VERIFY_WITH_TIMEOUT(peer->owner->outcome(),10000); QCOMPARE(peer->owner->outcome()->kind,TransferSession::Outcome::Kind::Completed); QCOMPARE(peer->fences,0);
        QTRY_VERIFY(!engine_->transfers().find(peer->owner->key()));
    }
    void receiverPauseAndResume_data() { QTest::addColumn<bool>("malformed"); QTest::newRow("durable-resume") << false; QTest::newRow("wrong-fence-digest") << true; }
    void receiverPauseAndResume() {
        QFETCH(bool,malformed);
        auto peer = std::make_shared<PauseSendingPeer>(); peer->malformed = malformed; peer->payload = payload(); auto data = fileData(peer->payload);
        data->direction = File::INCOMING; data->state = File::FS_OFFERED; data->offered_connection = peer->getConnectionId();
        peer->file = engine_->getFileManager()->addFile(std::move(data));
        auto progress = settle(peer->file->prepareCheckpointProgress(minimum_checkpoint_interval)); QVERIFY(progress);
        TransferAttempt attempt{QUuid::createUuid(),minimum_checkpoint_interval,1,progress.value().checkpoints.first()}; QVERIFY(peer->file->setCheckpointAttempt(attempt));
        peer->owner = std::make_shared<IncomingTransfer>(peer->file,peer,IncomingTransferStart::Accept);
        auto &m = engine_->transfers(); QVERIFY(m.acceptWork(*peer->file)); QVERIFY(m.submit(peer->owner));
        QTRY_VERIFY_WITH_TIMEOUT(peer->owner->outcome(),10000);
        if (malformed) {
            QVERIFY(!peer->acknowledged); QVERIFY(!peer->final_ack); QVERIFY(peer->file->getState()!=File::FS_DONE);
            QTRY_VERIFY(!m.find(peer->owner->key()));
            auto rejected = settle(engine_->getFileManager()->storage().transferProgress().load(peer->owner->key())); QVERIFY(rejected); QVERIFY(!rejected.value().complete);
            QCOMPARE(rejected.value().verified_prefix,quint64(minimum_checkpoint_interval)); QCOMPARE(m.active(identity_->getUuid(),contact_->getUuid()),size_t(0)); return;
        }
        QCOMPARE(peer->owner->outcome()->pause,TransferPauseReason::FeaturePaused); QVERIFY(peer->requested); QVERIFY(peer->acknowledged);
        QTRY_VERIFY(!m.find(peer->owner->key())); QTRY_VERIFY(m.workStore()->find(peer->owner->key()).value().eligible);
        auto work = m.workStore()->find(peer->owner->key()).value(); QVERIFY(work.resume_requested); QVERIFY(!work.user_paused); QCOMPARE(work.state,WorkState::Paused); QCOMPARE(peer->file->getState(),File::FS_PAUSED); QCOMPARE(peer->offset,quint64(minimum_checkpoint_interval));
        auto saved = settle(engine_->getFileManager()->storage().transferProgress().load(peer->owner->key())); QVERIFY(saved); QCOMPARE(saved.value().verified_prefix,quint64(minimum_checkpoint_interval)); QVERIFY(!saved.value().complete);
        peer->pause = false; peer->requested = false; peer->owner = std::make_shared<IncomingTransfer>(peer->file,peer,IncomingTransferStart::Resume);
        QVERIFY(m.pauseQueuedWork(peer->owner->key(),false)); QVERIFY(m.workStore()->project(peer->owner->key(),WorkState::Queued,true)); QVERIFY(m.submit(peer->owner));
        QTRY_VERIFY_WITH_TIMEOUT(peer->owner->outcome(),10000); QCOMPARE(peer->owner->outcome()->kind,TransferSession::Outcome::Kind::Completed); QVERIFY(peer->final_ack);
        QCOMPARE(peer->resumed_offset,quint64(minimum_checkpoint_interval)); QTRY_VERIFY(!m.find(peer->owner->key()));
        auto reader = settle(engine_->getFileManager()->storage().manager().openRead(peer->file->storageNamespace(),peer->file->storageId())); QVERIFY(reader);
        auto result = settle(reader.value()->readAt(0,peer->payload.size())); QVERIFY(result); QCOMPARE(result.value(),peer->payload); QVERIFY(settle(reader.value()->close()));
    }
    void receiverFinalizationStopPrecedence_data() { QTest::addColumn<bool>("cancel"); QTest::newRow("cancel-during-finalization") << true; QTest::newRow("revoke-during-finalization") << false; }
    void receiverFinalizationStopPrecedence() {
        QFETCH(bool,cancel);
        auto restore = qScopeGuard([&] { contact_->setBlocked(false); });
        auto peer = std::make_shared<PauseSendingPeer>(); peer->payload = payload(); auto data = fileData(peer->payload);
        data->direction = File::INCOMING; data->state = File::FS_OFFERED; data->offered_connection = peer->getConnectionId();
        peer->file = engine_->getFileManager()->addFile(std::move(data));
        auto progress = settle(peer->file->prepareCheckpointProgress(minimum_checkpoint_interval)); QVERIFY(progress);
        TransferAttempt attempt{QUuid::createUuid(),minimum_checkpoint_interval,1,progress.value().checkpoints.first()}; QVERIFY(peer->file->setCheckpointAttempt(attempt));
        peer->owner = std::make_shared<IncomingTransfer>(peer->file,peer,IncomingTransferStart::Accept);
        bool injected = false,awaiting_finalization = false,command_succeeded = false;
        connect(peer->owner.get(),&TransferSession::phaseChanged,peer->owner.get(),[&,weak=std::weak_ptr(peer->owner)] {
            auto owner=weak.lock(); if (!owner || owner->phase()!=TransferSession::Phase::Finalizing) return;
            QTimer::singleShot(0,owner.get(),[&,weak] {
                auto active=weak.lock(); if (!active) return;
                awaiting_finalization=active->phase()==TransferSession::Phase::Finalizing; injected=true;
                if (cancel) command_succeeded=bool(engine_->transfers().cancelWork(active->key())); else { contact_->setBlocked(true); command_succeeded=true; }
            });
        });
        auto &m=engine_->transfers(); QVERIFY(m.acceptWork(*peer->file)); QVERIFY(m.submit(peer->owner));
        QTRY_VERIFY_WITH_TIMEOUT(peer->owner->outcome(),10000); QVERIFY(injected); QVERIFY(awaiting_finalization); QVERIFY(command_succeeded); QVERIFY(!peer->acknowledged); QVERIFY(!peer->final_ack);
        QTRY_VERIFY(!m.find(peer->owner->key()));
        const auto work=m.workStore()->find(peer->owner->key()); QVERIFY(work); QVERIFY(!work.value().resume_requested); QVERIFY(!work.value().eligible);
        auto saved=settle(engine_->getFileManager()->storage().transferProgress().load(peer->owner->key())); QVERIFY(saved); QVERIFY(!saved.value().complete); QCOMPARE(saved.value().verified_prefix,quint64(minimum_checkpoint_interval));
        if (cancel) {
            QCOMPARE(peer->owner->outcome()->kind,TransferSession::Outcome::Kind::Cancelled); QCOMPARE(peer->file->getState(),File::FS_CANCELLED); QCOMPARE(saved.value().failure,TransferFailureReason::UserAction);
            auto pending=settle(engine_->getFileManager()->storage().manager().openPending(peer->file->storageNamespace(),peer->file->pendingId())); QVERIFY(!pending);
        } else {
            QCOMPARE(peer->owner->outcome()->pause,TransferPauseReason::AuthorizationWithdrawn); QCOMPARE(saved.value().pause,TransferPauseReason::AuthorizationWithdrawn); QCOMPARE(peer->file->getState(),File::FS_PAUSED);
        }
        QCOMPARE(m.active(identity_->getUuid(),contact_->getUuid()),size_t(0));
    }
    void pressureWaitsForQuantumAndPreservesLease() {
        const auto db = QSqlDatabase::addDatabase("QSQLITE",QUuid::createUuid().toString()); auto writable = db; writable.setDatabaseName(":memory:"); QVERIFY(writable.open());
        QSqlQuery q(writable); QVERIFY(q.exec("CREATE TABLE file(id INTEGER PRIMARY KEY)")); QVERIFY(q.exec("INSERT INTO file VALUES(1),(2),(3),(4),(5),(6)"));
        TransferManager m; auto joining = qScopeGuard([&] { settle(m.shutdown()); }); QVERIFY(m.openWorkStore(writable)); ds::models::TransferQueueModel model(m,[](TransferWork)->QCoro::Task<ds::models::TransferQueueModel::Metadata> { co_return ds::models::TransferQueueModel::Metadata{}; }); const auto local = QUuid::createUuid(),remote = QUuid::createUuid();
        auto add = [&](int id,TransferPurpose purpose) { TransferKey key{local,remote,QUuid::createUuid(),TransferSide::Receiver};
            auto work = m.workStore()->enroll(id,key,purpose,3,QCryptographicHash::hash("abc",QCryptographicHash::Sha256),true);
            if (!work || !m.workStore()->project(key,WorkState::Queued,true)) qFatal("scheduler fixture");
            auto session = std::make_shared<YieldSession>(key,id); if (!m.submit(session)) qFatal("scheduler enrollment"); return session; };
        auto first = add(1,TransferPurpose::FileShare),second = add(2,TransferPurpose::FileShare); QTRY_VERIFY(first->entered && second->entered);
        QVERIFY(!first->claimDeferredCooperativeResume()); QVERIFY(!second->claimDeferredCooperativeResume());
        auto conversation = add(3,TransferPurpose::Attachment); QTest::qWait(100); QVERIFY(!first->requested); QVERIFY(!second->requested); QVERIFY(!conversation->entered);
        QTRY_VERIFY_WITH_TIMEOUT(first->requested || second->requested,5000); auto victim = first->requested ? first : second;
        QVERIFY(!conversation->entered); QCOMPARE(m.active(local,remote),size_t(2));
        QVERIFY(victim->claimDeferredCooperativeResume()); QVERIFY(!victim->claimDeferredCooperativeResume());
        model.refresh(); QTRY_COMPARE(model.rowCount(),3);
        bool pausing_visible = false; for (int i=0;i<model.rowCount();++i) pausing_visible |= model.data(model.index(i),Qt::UserRole).toMap().value("state").toString().contains("Pausing for higher-priority"); QVERIFY(pausing_visible);
        victim->completePause(); QTRY_VERIFY(conversation->entered); QCOMPARE(m.active(local,remote),size_t(2));
        auto other = victim == first ? second : first; other->stop(TransferSession::Stop::Cancelled); QTRY_VERIFY(!m.find(other->key())); QTRY_VERIFY(!m.find(victim->key()));
        QVERIFY(m.workStore()->project(victim->key(),WorkState::Queued,true)); auto resumed = std::make_shared<YieldSession>(victim->key(),victim->localFile()); QVERIFY(m.submit(resumed)); QTRY_VERIFY(resumed->entered);
        auto next = add(4,TransferPurpose::Attachment); QTest::qWait(2300); QVERIFY(!resumed->requested); QVERIFY(!next->entered);
        auto closing = m.shutdown(); QTRY_VERIFY(closing.isReady()); QCOMPARE(m.active(local,remote),size_t(0));
    }
    void rejectedPressure_data() { QTest::addColumn<bool>("paused"); QTest::newRow("user-paused-conversation") << true; QTest::newRow("ineligible-conversation") << false; }
    void rejectedPressure() {
        QFETCH(bool,paused);
        auto db = QSqlDatabase::addDatabase("QSQLITE",QUuid::createUuid().toString()); db.setDatabaseName(":memory:"); QVERIFY(db.open());
        QSqlQuery q(db); QVERIFY(q.exec("CREATE TABLE file(id INTEGER PRIMARY KEY)")); QVERIFY(q.exec("INSERT INTO file VALUES(1),(2),(3)"));
        TransferManager m; auto joining = qScopeGuard([&] { settle(m.shutdown()); }); QVERIFY(m.openWorkStore(db)); const auto local=QUuid::createUuid(),remote=QUuid::createUuid();
        auto add = [&](int id,TransferPurpose purpose) { TransferKey key{local,remote,QUuid::createUuid(),TransferSide::Receiver};
            if (!m.workStore()->enroll(id,key,purpose,3,QCryptographicHash::hash("abc",QCryptographicHash::Sha256),true) || !m.workStore()->project(key,WorkState::Queued,true)) qFatal("rejected pressure enrollment");
            auto s=std::make_shared<YieldSession>(key,id); if (!m.submit(s)) qFatal("rejected pressure submit"); return s; };
        auto a=add(1,TransferPurpose::FileShare),b=add(2,TransferPurpose::FileShare); QTRY_VERIFY(a->entered && b->entered);
        auto c=add(3,TransferPurpose::Attachment); if (paused) QVERIFY(m.workStore()->setPaused(c->key(),true)); else QVERIFY(m.workStore()->project(c->key(),WorkState::Queued,false));
        QTest::qWait(2200); QVERIFY(!a->requested); QVERIFY(!b->requested); QVERIFY(!c->entered);
        auto closing=m.shutdown(); QTRY_VERIFY(closing.isReady()); QCOMPARE(m.active(local,remote),size_t(0));
    }
    void contactBottleneckPreservesUnrelatedBulk() {
        auto db=QSqlDatabase::addDatabase("QSQLITE",QUuid::createUuid().toString()); db.setDatabaseName(":memory:"); QVERIFY(db.open());
        QSqlQuery q(db); QVERIFY(q.exec("CREATE TABLE file(id INTEGER PRIMARY KEY)")); QVERIFY(q.exec("INSERT INTO file VALUES(1),(2),(3),(4)"));
        TransferManager m; auto joining = qScopeGuard([&] { settle(m.shutdown()); }); QVERIFY(m.openWorkStore(db)); const auto local=QUuid::createUuid(),remote=QUuid::createUuid(),unrelated=QUuid::createUuid();
        auto add=[&](int id,TransferPurpose purpose,QUuid contact) { TransferKey key{local,contact,QUuid::createUuid(),TransferSide::Receiver};
            if (!m.workStore()->enroll(id,key,purpose,3,QCryptographicHash::hash("abc",QCryptographicHash::Sha256),true) || !m.workStore()->project(key,WorkState::Queued,true)) qFatal("contact fixture enrollment");
            auto s=std::make_shared<YieldSession>(key,id); if (!m.submit(s)) qFatal("contact fixture submit"); return s; };
        auto other=add(1,TransferPurpose::FileShare,unrelated),a=add(2,TransferPurpose::FileShare,remote),b=add(3,TransferPurpose::FileShare,remote); QTRY_VERIFY(other->entered && a->entered && b->entered);
        auto c=add(4,TransferPurpose::Attachment,remote); QTRY_VERIFY_WITH_TIMEOUT(a->requested || b->requested,5000); QVERIFY(!other->requested); QVERIFY(!c->entered);
        auto closing=m.shutdown(); QTRY_VERIFY(closing.isReady()); QCOMPARE(m.active(local,remote),size_t(0)); QCOMPARE(m.active(local,unrelated),size_t(0));
    }
    void immediateContactResumeWaitsForRetirement() {
        // Use a distinct contact so connecting this test peer cannot dispatch
        // paused work retained by earlier offline fixture cases.
        auto old_contact=contact_; auto old_conversation=conversation_;
        auto contact=std::make_unique<ContactData>(); contact->identity=identity_->getId(); contact->state=Contact::ACCEPTED;
        auto certificate=ds::crypto::DsCert::create(); contact->cert=ds::crypto::DsCert::createFromPubkey(certificate->getSigningPubKey().toByteArray()); contact->name="Immediate resume"; contact->address="fixture-no-network"; contact->autoConnect=false;
        contact_=engine_->getContactManager()->addContact(std::move(contact)); conversation_=engine_->getConversationManager()->getConversation(contact_);
        auto restore=qScopeGuard([&] { contact_->disconnectFromContact(true); contact_=old_contact; conversation_=old_conversation; });
        auto peer=std::make_shared<PauseReceivingPeer>(); const auto bytes=payload(); peer->file=outgoing(bytes);
        bool resume_arrived_before_retirement=false;
        peer->after_pause_ack=[&,weak=std::weak_ptr(peer)] {
            auto active=weak.lock(); if (!active) return;
            resume_arrived_before_retirement=!active->owner->outcome();
            PeerFileResume resume(active,active->getConnectionId(),71,active->file->getFileId(),active->fenced.offset,active->file->getHash(),quint64(active->file->getSize()));
            resume.checkpoint_interval=minimum_checkpoint_interval;
            resume.retained_checkpoints={{0,0,QCryptographicHash::hash(QByteArray{},QCryptographicHash::Sha256),false,false},active->fenced};
            emit active->receivedFileResume(resume);
        };
        peer->begin(); auto first=peer->owner; contact_->onConnectedToPeer(peer);
        QTRY_COMPARE_WITH_TIMEOUT(peer->file->getState(),File::FS_DONE,10000);
        QVERIFY(resume_arrived_before_retirement); QVERIFY(first->outcome()); QCOMPARE(first->outcome()->pause,TransferPauseReason::FeaturePaused);
        QCOMPARE(peer->accepted_resumes,1); QCOMPARE(peer->rejected_resumes,0); QVERIFY(peer->owner!=first);
        QCOMPARE(peer->resumed_offset,quint64(minimum_checkpoint_interval)); QCOMPARE(peer->sent,bytes);
        QTRY_VERIFY(!engine_->transfers().find(first->key())); QCOMPARE(engine_->transfers().active(identity_->getUuid(),contact_->getUuid()),size_t(0));
    }
    void cleanupTestCase() {
        settle(engine_->transfers().shutdown()); QVERIFY(settle(engine_->getFileManager()->storage().shutdown()));
        // ConversationManager's cache owns the remaining shared references.
        // Drop our fixture reference before QObject parent destruction.
        conversation_.reset(); contact_=nullptr; identity_=nullptr;
        engine_.reset(); crypto_.reset();
    }
};
QTEST_GUILESS_MAIN(TransferPreemptionTest)
#include "transfer_preemption_test.moc"
