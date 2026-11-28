#include <ds/transfermanager.h>
#include "transfer_sender_fixture.h"
#include <ds/crypto.h>
#include <ds/filemanager.h>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QFile>
#include <QtTest>
using namespace ds::core;
using namespace dsfm;

class ScheduledSession final : public TransferSession {
public:
    ScheduledSession(TransferKey key,int file):TransferSession(key,QUuid::createUuid(),QUuid::createUuid(),file) {}
    int entered=0,finalized=0;
protected:
    QCoro::Task<Outcome> execute() override {
        ++entered;
        while(auto event=co_await nextEvent()) { Q_UNUSED(event); }
        co_return interrupted();
    }
    QCoro::Task<Outcome> finalize(Outcome result) override { ++finalized; co_return result; }
};
class TransferSchedulingTest final : public QObject {
    Q_OBJECT
    QUuid local_=QUuid::createUuid(),remote_=QUuid::createUuid();
    QByteArray hash_=QCryptographicHash::hash("payload",QCryptographicHash::Sha256);
    TransferKey key(QUuid remote={}) { return {local_,remote.isNull()?remote_:remote,QUuid::createUuid(),TransferSide::Receiver}; }
    QSqlDatabase database(const QString &path=":memory:") {
        auto db=QSqlDatabase::addDatabase("QSQLITE",QUuid::createUuid().toString()); db.setDatabaseName(path);
        if(!db.open()) qFatal("fixture DB open");
        QSqlQuery q(db);
        if(!q.exec("PRAGMA foreign_keys=ON") || !q.exec("CREATE TABLE IF NOT EXISTS file(id INTEGER PRIMARY KEY)")) qFatal("fixture schema");
        if(!q.exec("WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<1000) INSERT OR IGNORE INTO file SELECT x FROM n")) qFatal("fixture rows");
        return db;
    }
    std::shared_ptr<ScheduledSession> enroll(TransferManager &m,int id,TransferPurpose purpose,QUuid remote={}) {
        auto k=key(remote); auto *store=m.workStore();
        if(!store->enroll(id,k,purpose,7,hash_,true) || !store->project(k,WorkState::Queued,true)) qFatal("fixture enrollment");
        auto s=std::make_shared<ScheduledSession>(k,id);
        if(!m.submit(s)) qFatal("fixture submit");
        return s;
    }
    template<class T> T wait(QCoro::Task<T> task) { return QCoro::waitFor(std::move(task)); }
private slots:
    void currentVersionPriorityFixture() {
        auto db=database(); TransferWorkStore store(db); QVERIFY(store.open());
        QFile fixture(QStringLiteral(DS_WORK_FIXTURE)); QVERIFY(fixture.open(QIODevice::ReadOnly));
        for(const auto &statement:fixture.readAll().split(';')) {
            if(statement.trimmed().isEmpty()) continue;
            QSqlQuery q(db); QVERIFY(q.exec(QString::fromUtf8(statement)));
        }
        auto rows=store.page(); QVERIFY(rows); QCOMPARE(rows.value().size(),1);
        QCOMPARE(rows.value().front().priority,TransferPriority::High);
        QCOMPARE(schedulingClass(rows.value().front().purpose),TransferClass::Conversation);
    }
    void conversationAndLocalPriorityBeforeBulk() {
        TransferManager m; QVERIFY(m.openWorkStore(database()));
        auto bulk=enroll(m,1,TransferPurpose::FileShare);
        auto normal=enroll(m,2,TransferPurpose::Attachment);
        auto high=enroll(m,3,TransferPurpose::Attachment);
        auto utility=enroll(m,4,TransferPurpose::Catalog);
        QVERIFY(m.setWorkPriority(bulk->key(),TransferPriority::High));
        QVERIFY(m.setWorkPriority(high->key(),TransferPriority::High));
        QTRY_COMPARE(high->entered,1); QTRY_COMPARE(normal->entered,1); QCOMPARE(bulk->entered,0);
        QVERIFY(!m.pauseQueuedWork(high->key(),true)); // No unsafe active pause.
        QVERIFY(m.cancelWork(high->key())); QTRY_COMPARE(utility->entered,1); QCOMPARE(bulk->entered,0);
        QVERIFY(m.cancelWork(normal->key())); QTRY_COMPARE(bulk->entered,1);
        QTRY_COMPARE(high->finalized,1);
        auto closing=m.shutdown(); QTRY_VERIFY(closing.isReady()); QCOMPARE(m.active(local_,remote_),size_t(0));
    }
    void finiteBulkTurnDespiteConversationBacklog() {
        TransferManager m; QVERIFY(m.openWorkStore(database()));
        auto bulk=enroll(m,1,TransferPurpose::FileShare);
        QVERIFY(m.setWorkPriority(bulk->key(),TransferPriority::Low));
        QVector<std::shared_ptr<ScheduledSession>> conversations;
        for(int i=2;i<42;++i) conversations.push_back(enroll(m,i,TransferPurpose::Attachment));
        int completed=0;
        while(!bulk->entered && completed<20) {
            QTRY_VERIFY(std::any_of(conversations.begin(),conversations.end(),[](auto s) { return s->entered && !s->finalized; }) || bulk->entered);
            if(bulk->entered) break;
            auto it=std::find_if(conversations.begin(),conversations.end(),[](auto s) { return s->entered && !s->finalized; });
            auto s=*it; QVERIFY(m.cancelWork(s->key())); QTRY_COMPARE(s->finalized,1); ++completed;
        }
        QTRY_COMPARE(bulk->entered,1); QVERIFY(completed<=16);
        auto closing=m.shutdown(); QTRY_VERIFY(closing.isReady());
        QCOMPARE(m.active(local_,remote_),size_t(0));
    }
    void identityContactFairnessAndGlobalBound() {
        TransferManager m; QVERIFY(m.openWorkStore(database()));
        QVector<std::shared_ptr<ScheduledSession>> sessions;
        auto other=QUuid::createUuid();
        for(int i=1;i<=8;++i) sessions.push_back(enroll(m,i,TransferPurpose::Attachment,i<=4?remote_:other));
        QTRY_COMPARE(sessions[0]->entered,1); QTRY_COMPARE(sessions[4]->entered,1);
        QTRY_COMPARE(sessions[1]->entered,1); QTRY_COMPARE(sessions[5]->entered,1);
        QCOMPARE(sessions[2]->entered,0); QCOMPARE(sessions[6]->entered,0);
        QCOMPARE(m.active(local_,remote_),size_t(2)); QCOMPARE(m.active(local_,other),size_t(2));
        for(int i=9;i<=18;++i) sessions.push_back(enroll(m,i,TransferPurpose::Attachment,QUuid::createUuid()));
        QTRY_COMPARE(sessions[11]->entered,1); QCOMPARE(sessions[12]->entered,0);
        auto closing=m.shutdown(); QTRY_VERIFY(closing.isReady());
        for(auto s:sessions) QCOMPARE(s->finalized,1);
    }
    void shutdownDuringPagedDispatchSettlesOwners() {
        TransferManager m; QVERIFY(m.openWorkStore(database()));
        QVector<std::shared_ptr<ScheduledSession>> sessions;
        for(int i=1;i<=110;++i) sessions.push_back(enroll(m,i,TransferPurpose::FileShare,QUuid::createUuid()));
        m.dispatchWork(); // First page yields with admission still fenced.
        auto closing=m.shutdown(); QTRY_VERIFY_WITH_TIMEOUT(closing.isReady(),5000);
        for(auto s:sessions) QCOMPARE(s->finalized,1);
    }
    void durablePriorityScopedOrderAndSecurityFences() {
        QTemporaryDir dir; auto db=database(dir.filePath("work.sqlite"));
        TransferWorkStore store(db); QVERIFY(store.open());
        auto a=key(),b=key(),hidden=key(QUuid::createUuid()),bulk=key();
        QVERIFY(store.enroll(1,a,TransferPurpose::Attachment,7,hash_,true));
        QVERIFY(store.enroll(2,hidden,TransferPurpose::Attachment,7,hash_,true));
        QVERIFY(store.enroll(3,b,TransferPurpose::Attachment,7,hash_,true));
        QVERIFY(store.enroll(4,bulk,TransferPurpose::FileShare,7,hash_,true));
        const auto hidden_position=store.find(hidden).value().position;
        QVERIFY(store.reorder(b,a)); QCOMPARE(store.find(hidden).value().position,hidden_position);
        QVERIFY(!store.reorder(b,hidden)); QVERIFY(!store.reorder(b,bulk));
        QVERIFY(store.setPriority(b,TransferPriority::High)); QVERIFY(!store.setPriority(a,TransferPriority(77)));
        QVERIFY(!store.reorder(b,a)); // Different explicit priority.
        QVERIFY(store.setPaused(a,true));
        TransferWorkStore restart(db); QVERIFY(restart.open());
        QCOMPARE(restart.find(b).value().priority,TransferPriority::High);
        QVERIFY(restart.find(b).value().position<restart.find(a).value().position);
        QVERIFY(restart.find(a).value().user_paused); QVERIFY(!restart.find(b).value().eligible);
        TransferManager m; QVERIFY(m.openWorkStore(db));
        auto paused=std::make_shared<ScheduledSession>(a,1); QVERIFY(!m.submit(paused));
        auto unapproved=key(); QVERIFY(m.workStore()->enroll(5,unapproved,TransferPurpose::Attachment,7,hash_,false));
        QVERIFY(m.setWorkPriority(unapproved,TransferPriority::High));
        QVERIFY(!m.submit(std::make_shared<ScheduledSession>(unapproved,5)));
        QVERIFY(!m.workStore()->classify(bulk,TransferPurpose::Attachment));
        QVERIFY(m.cancelWork(a)); QCOMPARE(m.workStore()->find(a).value().state,WorkState::Cancelled);
        QVERIFY(!m.pauseQueuedWork(a,false));
        DurableTransferProgress journal; journal.description={a,7,hash_,minimum_checkpoint_interval,a.side};
        QVERIFY(m.workStore()->reconcile(a,journal)); QCOMPARE(m.workStore()->find(a).value().state,WorkState::Cancelled);
        auto closing=m.shutdown(); QTRY_VERIFY(closing.isReady());
    }
    void checkpointedPayloadRemainsIntact() {
        QTemporaryDir profile; qputenv("DARKSPEAK_PROFILE_ROOT",profile.path().toUtf8());
        qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE","disposable-work-test");
        ds::crypto::Crypto crypto;
        auto settings=[&] { auto s=std::make_unique<QSettings>(profile.filePath("settings.ini"),QSettings::IniFormat);
            s->setValue("dbpath",profile.filePath("app.sqlite")); s->setValue("downloadLocation",profile.filePath("downloads"));
            QDir().mkpath(profile.filePath("downloads")); return s; };
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
            auto &manager=engine->transfers();
            const QByteArray payload(2*minimum_checkpoint_interval+37,'q');
            QFile input(profile.filePath("source.bin")); QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write(payload),payload.size()); input.close();
            auto imported=wait(files->storage().importFile(input.fileName(),"payload.bin")); QVERIFY(imported);
            auto payload_data=std::make_unique<FileData>(); payload_data->state=File::FS_WAITING;
            payload_data->identity=identity->getId(); payload_data->contact=contact->getId(); payload_data->conversation=conversation->getId();
            payload_data->name="payload.bin"; payload_data->mime_type="application/octet-stream";
            payload_data->hash=QCryptographicHash::hash(payload,QCryptographicHash::Sha256); payload_data->size=payload.size();
            payload_data->storage_namespace=imported.value().namespace_id; payload_data->storage_id=imported.value().file.id;
            auto outgoing_payload=files->addFile(std::move(payload_data)); const auto payload_key=outgoing_payload->checkpointKey().value();
            QVERIFY(manager.bindWork(outgoing_payload,TransferPurpose::Attachment));
            QVERIFY(!manager.workStore()->find(payload_key).value().eligible);
            auto sender=std::make_shared<CheckpointSendingPeer>(); sender->begin(outgoing_payload);
            QTRY_COMPARE_WITH_TIMEOUT(outgoing_payload->getState(),File::FS_DONE,10000);
            QTRY_VERIFY(!manager.find(payload_key));
            QCOMPARE(sender->sent,payload); QCOMPARE(QCryptographicHash::hash(sender->sent,QCryptographicHash::Sha256),outgoing_payload->getHash());
            QCOMPARE(manager.workStore()->find(payload_key).value().state,WorkState::Completed);
            auto completed=wait(files->storage().transferProgress().load(payload_key)); QVERIFY(completed); QVERIFY(completed.value().complete);
            QCOMPARE(completed.value().verified_prefix,quint64(payload.size()));
            QVERIFY(wait(files->storage().shutdown()));
        }
    }
};
QTEST_GUILESS_MAIN(TransferSchedulingTest)
#include "transfer_scheduling_test.moc"
