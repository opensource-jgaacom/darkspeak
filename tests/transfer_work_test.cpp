#include <ds/transfermanager.h>
#include "transfer_sender_fixture.h"
#include <ds/dsengine.h>
#include <ds/crypto.h>
#include <ds/filemanager.h>
#include <QCryptographicHash>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QFile>
#include <QCoroSignal>
#include <QtTest>

using namespace ds::core;
using namespace dsfm;

class QueueSession final : public TransferSession {
public:
    explicit QueueSession(TransferKey key,int file) : TransferSession(key,QUuid::createUuid(),QUuid::createUuid(),file) {}
    int entered=0,finalized=0;
protected:
    QCoro::Task<Outcome> execute() override {
        ++entered;
        while(auto event=co_await nextEvent()) { Q_UNUSED(event); }
        co_return interrupted();
    }
    QCoro::Task<Outcome> finalize(Outcome value) override { ++finalized; co_return value; }
};

class TransferWorkTest : public QObject {
    Q_OBJECT
    QUuid local_=QUuid::createUuid(),remote_=QUuid::createUuid();
    QByteArray hash_=QCryptographicHash::hash("payload",QCryptographicHash::Sha256);
    TransferKey key(QUuid remote={}) const { return {local_,remote.isNull()?remote_:remote,QUuid::createUuid(),TransferSide::Receiver}; }
    QSqlDatabase database(const QString &path) {
        auto db=QSqlDatabase::addDatabase("QSQLITE",QUuid::createUuid().toString()); db.setDatabaseName(path);
        if(!db.open()) qFatal("Cannot open fixture DB");
        QSqlQuery q(db);
        if(!q.exec("PRAGMA foreign_keys=ON") || !q.exec("CREATE TABLE IF NOT EXISTS file(id INTEGER PRIMARY KEY)")) qFatal("Cannot initialize fixture");
        q.exec("WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<5000) INSERT OR IGNORE INTO file SELECT x FROM n");
        return db;
    }
    template<class T> T wait(QCoro::Task<T> task) { return QCoro::waitFor(std::move(task)); }
private slots:
    void durableOrderPauseAndVersionFixture() {
        QTemporaryDir dir; auto db=database(dir.filePath("work.sqlite"));
        TransferWorkStore store(db); QVERIFY(store.open());
        const auto a=key(),b=key(),c=key();
        const auto enrolled=store.enroll(1,a,TransferPurpose::Attachment,7,hash_,true); QVERIFY(enrolled);
        QVERIFY(store.enroll(2,b,TransferPurpose::Attachment,7,hash_,true));
        QVERIFY(store.enroll(3,c,TransferPurpose::Catalog,7,hash_,false));
        QVERIFY(store.project(a,WorkState::Queued,true)); QVERIFY(store.project(b,WorkState::Queued,true));
        auto duplicate=store.enroll(1,a,TransferPurpose::FileShare,7,hash_,false); QVERIFY(duplicate);
        QCOMPARE(duplicate.value().id,enrolled.value().id); QCOMPARE(duplicate.value().purpose,TransferPurpose::Attachment);
        QVERIFY(!store.enroll(2,a,TransferPurpose::Attachment,7,hash_,true));
        QVERIFY(!store.enroll(1,a,TransferPurpose::Attachment,8,hash_,true));
        QVERIFY(store.reorder(b,a)); QVERIFY(store.setPaused(a,true));
        TransferWorkStore restarted(db); QVERIFY(restarted.open());
        auto page=restarted.page(); QVERIFY(page); QCOMPARE(page.value().size(),3);
        QCOMPARE(page.value()[0].key,b); QCOMPARE(page.value()[1].id,enrolled.value().id);
        QVERIFY(page.value()[1].user_paused); QVERIFY(!page.value()[0].eligible);
        QCOMPARE(page.value()[2].state,WorkState::AwaitingAcceptance);
        QFile fixture(QStringLiteral(DS_WORK_FIXTURE)); QVERIFY(fixture.open(QIODevice::ReadOnly));
        QSqlQuery q(db); QVERIFY(q.exec(QString::fromUtf8(fixture.readAll())));
        const TransferKey fixed{QUuid("{11111111-1111-1111-1111-111111111111}"),QUuid("{22222222-2222-2222-2222-222222222222}"),
            QUuid("{33333333-3333-3333-3333-333333333333}"),TransferSide::Sender};
        auto f=restarted.find(fixed); QVERIFY(f); QCOMPARE(f.value().purpose,TransferPurpose::Preview); QVERIFY(f.value().user_paused);
        QVERIFY(!restarted.page(-1)); QVERIFY(!restarted.page(0,101));
    }
    void untrustedBoundsAndJournalAuthority() {
        auto db=database(":memory:"); TransferWorkStore store(db); QVERIFY(store.open());
        auto k=key(); QVERIFY(!store.enroll(1,k,TransferPurpose::Attachment,1,QByteArray(31,'x'),false));
        auto bad=k; bad.side=TransferSide(77); QVERIFY(!store.enroll(1,bad,TransferPurpose::Attachment,7,hash_,false));
        QVERIFY(store.enroll(1,k,TransferPurpose::Attachment,7,hash_,false));
        QCOMPARE(store.find(k).value().state,WorkState::AwaitingAcceptance); QVERIFY(!store.find(k).value().eligible);
        QVERIFY(store.accept(k)); QVERIFY(store.setPaused(k,true));
        DurableTransferProgress journal; journal.description={k,7,hash_,minimum_checkpoint_interval,k.side};
        journal.verified_prefix=7; journal.confirmed_offset=7;
        QVERIFY(store.reconcile(k,journal)); auto w=store.find(k); QVERIFY(w.value().user_paused); QVERIFY(!w.value().eligible);
        journal.complete=true; QVERIFY(store.reconcile(k,journal)); QCOMPARE(store.find(k).value().state,WorkState::Completed);
        journal.description.sha256=QByteArray(32,'x'); QVERIFY(store.reconcile(k,journal)); QCOMPARE(store.find(k).value().state,WorkState::Attention);
        for(int i=2;i<=128;++i) QVERIFY(store.enroll(i,key(),TransferPurpose::Attachment,7,hash_,false));
        auto overflow=store.enroll(129,key(),TransferPurpose::Attachment,7,hash_,true); QVERIFY(!overflow); QCOMPARE(overflow.error().code,ErrorCode::QueueFull);
        // Identity/contact scope is part of idempotency; no cross-scope alias.
        auto scoped=k; scoped.remote_scope=QUuid::createUuid();
        QVERIFY(store.enroll(129,scoped,TransferPurpose::FileShare,7,hash_,false));
        for(int i=130;i<=4096;++i)
            QVERIFY(store.enroll(i,key(QUuid::createUuid()),TransferPurpose::FileShare,7,hash_,false));
        auto full=store.enroll(4097,key(QUuid::createUuid()),TransferPurpose::Attachment,7,hash_,true);
        QVERIFY(!full); QCOMPARE(full.error().code,ErrorCode::QueueFull);
    }
    void sameManagerAdmissionAndDurableReorder() {
        auto db=database(":memory:"); TransferManager manager; QVERIFY(manager.openWorkStore(db));
        QVector<std::shared_ptr<QueueSession>> sessions;
        for(int i=1;i<=4;++i) {
            const auto k=key(); QVERIFY(manager.workStore()->enroll(i,k,TransferPurpose::Attachment,7,hash_,true));
            QVERIFY(manager.workStore()->project(k,WorkState::Queued,true));
            auto session=std::make_shared<QueueSession>(k,i); QVERIFY(manager.submit(session)); sessions.push_back(session);
        }
        QTRY_COMPARE(sessions[0]->entered,1); QTRY_COMPARE(sessions[1]->entered,1); QCOMPARE(sessions[2]->entered,0);
        QVERIFY(manager.pauseQueuedWork(sessions[2]->key(),true));
        QVERIFY(manager.reorderWork(sessions[3]->key(),sessions[2]->key()));
        sessions[0]->stop(TransferSession::Stop::Cancelled);
        QTRY_COMPARE(sessions[3]->entered,1); QCOMPARE(sessions[2]->entered,0);
        auto unaccepted=key(); QVERIFY(manager.workStore()->enroll(5,unaccepted,TransferPurpose::FileShare,7,hash_,false));
        QVERIFY(!manager.submit(std::make_shared<QueueSession>(unaccepted,5)));
        auto paused=key(); QVERIFY(manager.workStore()->enroll(6,paused,TransferPurpose::FileShare,7,hash_,true));
        QVERIFY(manager.workStore()->setPaused(paused,true));
        QVERIFY(!manager.submit(std::make_shared<QueueSession>(paused,6)));
        // Cleanup commands run even when payload eligibility has been withdrawn.
        auto cleanup=std::make_shared<QueueSession>(unaccepted,5); cleanup->stop(TransferSession::Stop::Cancelled); QVERIFY(manager.submit(cleanup));
        for(auto &session:sessions) session->stop(TransferSession::Stop::Disconnected);
        auto closing=manager.shutdown(); QTRY_VERIFY(closing.isReady());
        for(auto &session:sessions) QCOMPARE(session->finalized,1);
        QCOMPARE(manager.active(local_,remote_),size_t(0));
    }
    void finiteGlobalAdmission() {
        auto db=database(":memory:"); TransferManager manager; QVERIFY(manager.openWorkStore(db));
        QVector<std::shared_ptr<QueueSession>> sessions;
        for(int i=1;i<=10;++i) {
            const auto k=key(QUuid::createUuid());
            QVERIFY(manager.workStore()->enroll(i,k,TransferPurpose::FileShare,7,hash_,true));
            QVERIFY(manager.workStore()->project(k,WorkState::Queued,true));
            auto session=std::make_shared<QueueSession>(k,i); QVERIFY(manager.submit(session)); sessions.push_back(session);
        }
        QTRY_COMPARE(sessions[7]->entered,1); QCOMPARE(sessions[8]->entered,0); QCOMPARE(sessions[9]->entered,0);
        sessions[0]->stop(TransferSession::Stop::Disconnected); QTRY_COMPARE(sessions[8]->entered,1);
        for(auto &session:sessions) session->stop(TransferSession::Stop::Disconnected);
        auto closing=manager.shutdown(); QTRY_VERIFY(closing.isReady());
        for(auto &session:sessions) QCOMPARE(session->finalized,1);
    }
    void applicationAdaptersAndAcceptedRestart() {
        QTemporaryDir profile; qputenv("DARKSPEAK_PROFILE_ROOT",profile.path().toUtf8());
        qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE","disposable-work-test");
        ds::crypto::Crypto crypto;
        auto settings=[&] { auto s=std::make_unique<QSettings>(profile.filePath("settings.ini"),QSettings::IniFormat);
            s->setValue("dbpath",profile.filePath("app.sqlite")); s->setValue("downloadLocation",profile.filePath("downloads"));
            QDir().mkpath(profile.filePath("downloads")); return s; };
        TransferKey first_key,second_key,journal_key,failed_key,source_key,payload_key; QUuid work_id; int first_id=0;
        {
            auto engine=std::make_unique<DsEngine>(settings()); auto *files=engine->getFileManager(); QVERIFY(wait(files->storage().ready()));
            IdentityData data; data.uuid=QUuid::createUuid(); data.name="Queue fixture"; data.cert=ds::crypto::DsCert::create();
            data.hash=data.cert->getHash().toByteArray(); data.autoConnect=false;
            auto *identity=engine->getIdentityManager()->addIdentity(data);
            auto peer=std::make_unique<ContactData>(); peer->identity=identity->getId(); peer->state=Contact::ACCEPTED;
            auto certificate=ds::crypto::DsCert::create(); peer->cert=ds::crypto::DsCert::createFromPubkey(certificate->getSigningPubKey().toByteArray());
            peer->name="Peer"; peer->address="fixture-no-network"; peer->autoConnect=false;
            auto *contact=engine->getContactManager()->addContact(std::move(peer));
            auto conversation=engine->getConversationManager()->getConversation(contact);
            auto incoming=[&] {
                auto file=std::make_unique<FileData>(); file->direction=File::INCOMING; file->state=File::FS_OFFERED;
                file->identity=identity->getId(); file->contact=contact->getId(); file->conversation=conversation->getId();
                file->name="payload.bin"; file->mime_type="application/octet-stream"; file->hash=hash_; file->size=7;
                file->fileId=QUuid::createUuid().toRfc4122(); file->checkpoint_interval=minimum_checkpoint_interval;
                return files->addFile(std::move(file));
            };
            auto refused=incoming();
            QVERIFY(engine->transfers().bindWork(refused));
            contact->setState(Contact::REJECTED); refused->accept();
            QCOMPARE(refused->getState(),File::FS_OFFERED); QVERIFY(refused->pendingId().isNull());
            QVERIFY(!engine->transfers().workStore()->find(refused->checkpointKey().value()).value().eligible);
            contact->setState(Contact::ACCEPTED);
            auto first=incoming(),second=incoming(); first_id=first->getId();
            auto &manager=engine->transfers();
            QVERIFY(manager.bindWork(first,TransferPurpose::FileShare)); QVERIFY(manager.bindWork(second,TransferPurpose::Catalog));
            first_key=first->checkpointKey().value(); second_key=second->checkpointKey().value();
            QVERIFY(!manager.workStore()->find(first_key).value().eligible);
            first->accept(); second->accept(); QCOMPARE(first->getState(),File::FS_QUEUED); QCOMPARE(second->getState(),File::FS_QUEUED);
            QVERIFY(first->pendingId().isNull()); QVERIFY(second->pendingId().isNull()); QCOMPARE(manager.active(identity->getUuid(),contact->getUuid()),size_t(0));
            QVERIFY(manager.pauseQueuedWork(first_key,true)); QVERIFY(!manager.reorderWork(second_key,first_key));
            work_id=manager.workStore()->find(first_key).value().id;
            auto journal_file=incoming(),failed_file=incoming();
            journal_file->accept(); failed_file->accept();
            journal_key=journal_file->checkpointKey().value(); failed_key=failed_file->checkpointKey().value();
            auto &progress=files->storage().transferProgress();
            auto retained=wait(progress.create({journal_key,7,hash_,minimum_checkpoint_interval,journal_key.side})); QVERIFY(retained);
            const auto nonce=QUuid::createUuid();
            const DurableCheckpoint zero{0,0,QCryptographicHash::hash(QByteArray{},QCryptographicHash::Sha256),false,false};
            QVERIFY(wait(progress.beginAttempt(journal_key,nonce,zero)));
            QVERIFY(wait(progress.pause(journal_key,nonce,TransferPauseReason::FeaturePaused)));
            journal_file->setState(File::FS_PAUSED);
            QVERIFY(manager.workStore()->project(journal_key,WorkState::Active,false));
            QVERIFY(wait(progress.create({failed_key,7,hash_,minimum_checkpoint_interval,failed_key.side})));
            QVERIFY(wait(progress.fail(failed_key,TransferFailureReason::SourceIntegrityMismatch)));
            failed_file->setState(File::FS_PAUSED);
            QVERIFY(manager.workStore()->project(failed_key,WorkState::Active,false));

            // A transient authorized source is never rebuilt from a suggested name.
            auto outgoing=std::make_unique<FileData>(); outgoing->state=File::FS_WAITING; outgoing->identity=identity->getId();
            outgoing->contact=contact->getId(); outgoing->conversation=conversation->getId(); outgoing->name="catalog.jsonl";
            outgoing->hash=hash_; outgoing->size=7; auto missing=files->addFile(std::move(outgoing));
            QVERIFY(manager.bindWork(missing,TransferPurpose::Catalog));
            QVERIFY(!manager.workStore()->find(missing->checkpointKey().value()).value().eligible);
            QCOMPARE(manager.workStore()->find(missing->checkpointKey().value()).value().state,WorkState::AwaitingAcceptance);
            QVERIFY(manager.acceptWork(*missing));
            QCOMPARE(manager.workStore()->find(missing->checkpointKey().value()).value().state,WorkState::Attention);
            auto source_data=std::make_unique<FileData>(); source_data->state=File::FS_WAITING;
            source_data->identity=identity->getId(); source_data->contact=contact->getId(); source_data->conversation=conversation->getId();
            source_data->name="live-source.bin"; source_data->mime_type="application/octet-stream"; source_data->hash=hash_; source_data->size=7;
            auto source=files->addFile(std::move(source_data)); source_key=source->checkpointKey().value();
            QVERIFY(source->setPreparedSource(identity->getUuid(),contact->getUuid(),[] { return true; },
                [](ReadPreparationOptions) -> QCoro::Task<Result<PreparedRead>> { co_return dsfm::Error{ErrorCode::MissingEntry}; }));
            QVERIFY(manager.bindWork(source,TransferPurpose::FileShare));
            QVERIFY(!manager.workStore()->find(source_key).value().eligible); // Staged only.
            contact->queueFile(source); QVERIFY(manager.workStore()->find(source_key).value().eligible);
            const QByteArray payload(2*minimum_checkpoint_interval+37,'q');
            QFile input(profile.filePath("source.bin")); QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write(payload),payload.size()); input.close();
            auto imported=wait(files->storage().importFile(input.fileName(),"payload.bin")); QVERIFY(imported);
            auto payload_data=std::make_unique<FileData>(); payload_data->state=File::FS_WAITING;
            payload_data->identity=identity->getId(); payload_data->contact=contact->getId(); payload_data->conversation=conversation->getId();
            payload_data->name="payload.bin"; payload_data->mime_type="application/octet-stream";
            payload_data->hash=QCryptographicHash::hash(payload,QCryptographicHash::Sha256); payload_data->size=payload.size();
            payload_data->storage_namespace=imported.value().namespace_id; payload_data->storage_id=imported.value().file.id;
            auto outgoing_payload=files->addFile(std::move(payload_data)); payload_key=outgoing_payload->checkpointKey().value();
            QVERIFY(manager.bindWork(outgoing_payload,TransferPurpose::Attachment));
            QVERIFY(!manager.workStore()->find(payload_key).value().eligible);
            auto sender=std::make_shared<CheckpointSendingPeer>(); sender->begin(outgoing_payload);
            QTRY_COMPARE_WITH_TIMEOUT(outgoing_payload->getState(),File::FS_DONE,10000);
            QTRY_VERIFY(!manager.find(payload_key));
            QCOMPARE(sender->sent,payload); QCOMPARE(QCryptographicHash::hash(sender->sent,QCryptographicHash::Sha256),outgoing_payload->getHash());
            QCOMPARE(manager.workStore()->find(payload_key).value().state,WorkState::Completed);
            auto completed=wait(progress.load(payload_key)); QVERIFY(completed); QVERIFY(completed.value().complete);
            QCOMPARE(completed.value().verified_prefix,quint64(payload.size()));
            QVERIFY(wait(files->storage().shutdown()));
        }
        {
            auto engine=std::make_unique<DsEngine>(settings()); auto *files=engine->getFileManager(); QVERIFY(wait(files->storage().ready()));
            engine->transfers().restoreWork();
            QTRY_VERIFY(engine->transfers().workStore()->find(first_key));
            QTRY_VERIFY(!files->getFile(first_id)->recoveryPending());
            auto &store=*engine->transfers().workStore();
            QCOMPARE(store.find(payload_key).value().state,WorkState::Completed);
            QCOMPARE(store.find(first_key).value().id,work_id); QVERIFY(store.find(first_key).value().user_paused);
            QVERIFY(!store.find(first_key).value().eligible); QCOMPARE(store.find(first_key).value().purpose,TransferPurpose::FileShare);
            QVERIFY(store.find(second_key).value().eligible); QVERIFY(store.find(second_key).value().position>store.find(first_key).value().position);
            QVERIFY(engine->transfers().pauseQueuedWork(first_key,false)); QVERIFY(store.find(first_key).value().eligible);
            QTRY_COMPARE(store.find(journal_key).value().state,WorkState::Paused);
            QVERIFY(!store.find(journal_key).value().eligible); // Journal feature pause wins over the crashed Active mirror.
            QVERIFY(engine->transfers().pauseQueuedWork(journal_key,false)); QVERIFY(store.find(journal_key).value().eligible);
            QTRY_COMPARE(store.find(failed_key).value().state,WorkState::Failed);
            QVERIFY(!store.find(failed_key).value().eligible); QVERIFY(!engine->transfers().pauseQueuedWork(failed_key,false));
            QCOMPARE(store.find(source_key).value().state,WorkState::Attention); QVERIFY(!store.find(source_key).value().eligible);
            QVERIFY(files->getFile(first_id)->pendingId().isNull());
            QVERIFY(wait(files->storage().shutdown()));
        }
    }
};
QTEST_GUILESS_MAIN(TransferWorkTest)
#include "transfer_work_test.moc"
