#include <ds/dsengine.h>
#include <ds/crypto.h>
#include <ds/filemanager.h>
#include <ds/transfermanager.h>
#include <ds/transferqueuemodel.h>
#include <ds/peer.h>
#include "appearance.h"
#include <QAbstractItemModelTester>
#include <QCryptographicHash>
#include <QFile>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QCoroTimer>
#include <QtTest>
using namespace ds::core;
using namespace ds::models;
using namespace dsfm;

template<class T> T finish(QCoro::Task<T> task) { return QCoro::waitFor(std::move(task)); }

class QueueOfferPeer final : public ds::prot::Peer {
public:
    QueueOfferPeer() : Peer(ds::prot::ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return INCOMING; }
    bool isConnected() const noexcept override { return false; }
    uint64_t sendFileOfferResponse(const QByteArray &, FileOfferResponseStatus) override { ++rejected; return 0; }
    int rejected = 0;
};

// Retain the sole attempt owner during cleanup so tests can observe the durable
// command independently of its eventual File/history projection.
class QueueOwner final : public TransferSession {
public:
    explicit QueueOwner(File::ptr_t file) : TransferSession(file->checkpointKey().value(),QUuid::createUuid(),QUuid::createUuid(),file->getId()),file_{std::move(file)} {}
    bool entered=false,finalizing=false;
    void releaseCleanup() { cleanup_.start(0); }
protected:
    QCoro::Task<Outcome> execute() override {
        entered=true; setPhase(Phase::Transferring); file_->setState(File::FS_TRANSFERRING);
        while (co_await nextEvent()) {}
        co_return interrupted();
    }
    QCoro::Task<Outcome> finalize(Outcome result) override {
        finalizing=true; cleanup_.setSingleShot(true); cleanup_.start(10000); co_await cleanup_;
        file_->setState(File::FS_CANCELLED); co_return result;
    }
private:
    File::ptr_t file_;
    QTimer cleanup_;
};

class QueueIntegrationTest final : public QObject {
    Q_OBJECT
    QTemporaryDir profile_;
    std::unique_ptr<ds::crypto::Crypto> crypto_;
    std::unique_ptr<DsEngine> engine_;
    Identity *identity_=nullptr;
    Contact *contact_=nullptr;
    Conversation::ptr_t conversation_;
    static QCoro::Task<TransferQueueModel::Metadata> metadata(TransferWork) { co_return TransferQueueModel::Metadata{}; }
    static QVariantMap entry(TransferQueueModel &m,QUuid id) {
        for (int i=0;i<m.rowCount();++i) { auto row=m.data(m.index(i),Qt::UserRole).toMap(); if (row["workId"].toUuid()==id) return row; }
        return {};
    }
    File::ptr_t file(bool incoming=false) {
        const QByteArray payload="queue-shared-command";
        auto data=std::make_unique<FileData>(); data->identity=identity_->getId(); data->contact=contact_->getId(); data->conversation=conversation_->getId();
        data->direction=incoming?File::INCOMING:File::OUTGOING; data->state=incoming?File::FS_PAUSED:File::FS_WAITING;
        data->name="payload.bin"; data->mime_type="application/octet-stream"; data->size=payload.size(); data->hash=QCryptographicHash::hash(payload,QCryptographicHash::Sha256);
        if (incoming) data->checkpoint_interval=minimum_checkpoint_interval;
        else {
            QFile input(profile_.filePath(QUuid::createUuid().toString()+".bin"));
            if (!input.open(QIODevice::WriteOnly) || input.write(payload)!=payload.size()) qFatal("queue source fixture"); input.close();
            auto imported=finish(engine_->getFileManager()->storage().importFile(input.fileName(),"payload.bin")); if (!imported) qFatal("queue import fixture");
            data->storage_namespace=imported.value().namespace_id; data->storage_id=imported.value().file.id;
        }
        auto result=engine_->getFileManager()->addFile(std::move(data));
        if (!engine_->transfers().acceptWork(*result)) qFatal("queue acceptance fixture");
        return result;
    }
private slots:
    void initTestCase() {
        QVERIFY(profile_.isValid()); qputenv("DARKSPEAK_PROFILE_ROOT",profile_.path().toUtf8()); qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE","disposable-queue-integration");
        crypto_=std::make_unique<ds::crypto::Crypto>();
        auto settings=std::make_unique<QSettings>(profile_.filePath("settings.ini"),QSettings::IniFormat);
        settings->setValue("dbpath",profile_.filePath("app.sqlite")); settings->setValue("downloadLocation",profile_.filePath("downloads")); QDir().mkpath(profile_.filePath("downloads"));
        engine_=std::make_unique<DsEngine>(std::move(settings)); QVERIFY(finish(engine_->getFileManager()->storage().ready()));
        IdentityData identity; identity.uuid=QUuid::createUuid(); identity.name="Queue integration"; identity.cert=ds::crypto::DsCert::create(); identity.hash=identity.cert->getHash().toByteArray(); identity.autoConnect=false;
        identity_=engine_->getIdentityManager()->addIdentity(identity); identity_->setAllowFiles(true);
        auto contact=std::make_unique<ContactData>(); contact->identity=identity_->getId(); contact->state=Contact::ACCEPTED;
        auto cert=ds::crypto::DsCert::create(); contact->cert=ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray()); contact->name="Peer"; contact->address="fixture-no-network"; contact->autoConnect=false;
        contact_=engine_->getContactManager()->addContact(std::move(contact)); conversation_=engine_->getConversationManager()->getConversation(contact_);
        qmlRegisterUncreatableType<File>("org.darkspeak",1,0,"File","Files are owned by C++");
    }
    void cardAndTransfersCancelPersistBeforeCleanup_data() {
        QTest::addColumn<bool>("card"); QTest::newRow("conversation-card")<<true; QTest::newRow("scoped-transfers")<<false;
    }
    void cardAndTransfersCancelPersistBeforeCleanup() {
        QFETCH(bool,card); auto f=file(); auto &manager=engine_->transfers(); auto key=f->checkpointKey().value(); auto work=manager.workStore()->find(key).value();
        auto owner=std::make_shared<QueueOwner>(f); QVERIFY(manager.submit(owner));
        auto joining=qScopeGuard([&] { owner->releaseCleanup(); });
        QTRY_VERIFY(owner->entered);
        TransferQueueModel all(manager,metadata),identity(manager,metadata),contact(manager,metadata);
        identity.setScope(identity_->getUuid()); contact.setScope(identity_->getUuid(),contact_->getUuid());
        QAbstractItemModelTester tester(&contact,QAbstractItemModelTester::FailureReportingMode::QtTest);
        QTRY_VERIFY(!entry(all,work.id).isEmpty()); QTRY_VERIFY(!entry(identity,work.id).isEmpty()); QTRY_VERIFY(!entry(contact,work.id).isEmpty());
        Appearance appearance; QQuickWindow window; window.resize(1024,768); QQuickItem list; list.setWidth(1024);
        QQmlEngine qml; qml.rootContext()->setContextProperty("appearance",&appearance); qml.rootContext()->setContextProperty("cList",&list);
        qml.rootContext()->setContextProperty("cFile",f.get()); qml.rootContext()->setContextProperty("cData",QVariantMap{{"stateName","Transferring"}});
        QSignalSpy warnings(&qml,&QQmlEngine::warnings);
        QQmlComponent component(&qml,QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/FileComponent.qml")));
        std::unique_ptr<QObject> object(component.create()); QVERIFY2(object,qPrintable(component.errorString()));
        auto *item=qobject_cast<QQuickItem*>(object.get()); QVERIFY(item); item->setParentItem(window.contentItem()); window.show();
        if (card) {
            QVERIFY(QMetaObject::invokeMethod(object.get(),"showMenu"));
            auto *cancel=object->findChild<QObject*>("transferCancelSend"); QVERIFY(cancel); QVERIFY(cancel->property("enabled").toBool());
            QVERIFY(QMetaObject::invokeMethod(cancel,"triggered"));
            // No event loop turn separates the UI command from this assertion.
            QCOMPARE(manager.workStore()->find(key).value().state,WorkState::Cancelled);
        } else {
            QSignalSpy done(&contact,&TransferQueueModel::commandFinished); contact.command(work.id,"cancel"); QTRY_COMPARE(done.size(),1);
            QCOMPARE(done.first()[1].toInt(),0); QCOMPARE(manager.workStore()->find(key).value().state,WorkState::Cancelled);
        }
        QTRY_VERIFY(owner->finalizing); QVERIFY(!owner->outcome()); QCOMPARE(manager.active(identity_->getUuid(),contact_->getUuid()),size_t(1));
        // A separate SQLite connection must observe committed intent while the
        // attempt still holds its cleanup reservation.
        const auto connection=QUuid::createUuid().toString();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",connection); db.setDatabaseName(engine_->getDb().databaseName()); QVERIFY(db.open());
            TransferWorkStore reopened(db); QVERIFY(reopened.open()); auto saved=reopened.find(key); QVERIFY(saved); QCOMPARE(saved.value().state,WorkState::Cancelled); QVERIFY(!saved.value().eligible);
        }
        QSqlDatabase::removeDatabase(connection);
        all.refresh(); identity.refresh(); contact.refresh();
        QTRY_COMPARE(entry(all,work.id)["state"].toString(),QString("Cancelled"));
        QTRY_COMPARE(entry(identity,work.id)["state"].toString(),QString("Cancelled"));
        QTRY_COMPARE(entry(contact,work.id)["state"].toString(),QString("Cancelled"));
        owner->releaseCleanup(); QTRY_VERIFY(owner->outcome()); QTRY_VERIFY(!manager.find(key)); QCOMPARE(f->getState(),File::FS_CANCELLED);
        QCOMPARE(manager.active(identity_->getUuid(),contact_->getUuid()),size_t(0)); QCOMPARE(warnings.size(),0);
        // Cancellation remains terminal even if a stale feature card asks to resume.
        f->resume(); QCOMPARE(manager.workStore()->find(key).value().state,WorkState::Cancelled);
    }
    void cardResumeAndScopedHoldShareWork() {
        auto f=file(true); auto &manager=engine_->transfers(); const auto key=f->checkpointKey().value();
        QVERIFY(manager.workStore()->project(key,WorkState::Paused,false)); QVERIFY(manager.pauseQueuedWork(key,true)); QVERIFY(f->isResumable());
        const auto work=manager.workStore()->find(key).value();
        TransferQueueModel all(manager,metadata),contact(manager,metadata),foreign(manager,metadata);
        contact.setScope(identity_->getUuid(),contact_->getUuid()); foreign.setScope(QUuid::createUuid());
        QTRY_VERIFY(!entry(contact,work.id).isEmpty()); QTRY_VERIFY(entry(contact,work.id)["canResume"].toBool());
        f->resume(); QVERIFY(!manager.workStore()->find(key).value().user_paused); QVERIFY(manager.workStore()->find(key).value().resume_requested);
        contact.refresh(); QTRY_VERIFY(!entry(contact,work.id)["canResume"].toBool());
        QSignalSpy done(&contact,&TransferQueueModel::commandFinished); contact.command(work.id,"pause"); QTRY_COMPARE(done.size(),1); QCOMPARE(done.first()[1].toInt(),0);
        QVERIFY(manager.workStore()->find(key).value().user_paused); all.refresh(); QTRY_COMPARE(entry(all,work.id)["state"].toString(),QString("Paused by you"));
        QSignalSpy denied(&foreign,&TransferQueueModel::commandFinished); foreign.command(work.id,"resume"); QCOMPARE(denied.size(),1); QCOMPARE(denied.first()[1].toInt(),int(ErrorCode::PermissionDenied));
        QVERIFY(manager.workStore()->find(key).value().user_paused); QCOMPARE(manager.active(identity_->getUuid(),contact_->getUuid()),size_t(0));
    }
    void scopedReorderSurvivesRestart() {
        QTemporaryDir temp; QVERIFY(temp.isValid()); const auto path=temp.filePath("work.sqlite"); const auto local=QUuid::createUuid(),remote=QUuid::createUuid();
        TransferWork first,second,hidden;
        const auto database=[&] { auto db=QSqlDatabase::addDatabase("QSQLITE",QUuid::createUuid().toString()); db.setDatabaseName(path); if (!db.open()) qFatal("reorder database"); QSqlQuery q(db); if (!q.exec("CREATE TABLE IF NOT EXISTS file(id INTEGER PRIMARY KEY)")) qFatal("reorder schema"); return db; };
        {
            auto db=database(); TransferManager manager; QVERIFY(manager.openWorkStore(db)); auto joining=qScopeGuard([&] { finish(manager.shutdown()); });
            const auto add=[&](int id,QUuid contact) { QSqlQuery q(db); q.prepare("INSERT INTO file VALUES(?)"); q.addBindValue(id); if (!q.exec()) qFatal("reorder row");
                TransferKey key{local,contact,QUuid::createUuid(),TransferSide::Receiver}; auto work=manager.workStore()->enroll(id,key,TransferPurpose::FileShare,3,QCryptographicHash::hash("abc",QCryptographicHash::Sha256),true); if (!work || !manager.workStore()->project(key,WorkState::Queued,true)) qFatal("reorder enrollment"); return manager.workStore()->find(key).value(); };
            first=add(1,remote); hidden=add(2,QUuid::createUuid()); second=add(3,remote);
            TransferQueueModel scoped(manager,metadata); scoped.setScope(local,remote); QTRY_COMPARE(scoped.rowCount(),2);
            QSignalSpy done(&scoped,&TransferQueueModel::commandFinished); scoped.command(second.id,"up"); QTRY_COMPARE(done.size(),1); QCOMPARE(done.first()[1].toInt(),0);
            QCOMPARE(manager.workStore()->find(hidden.key).value().position,hidden.position); QVERIFY(manager.workStore()->find(second.key).value().position<manager.workStore()->find(first.key).value().position);
        }
        {
            auto db=database(); TransferManager manager; QVERIFY(manager.openWorkStore(db)); auto joining=qScopeGuard([&] { finish(manager.shutdown()); });
            TransferQueueModel scoped(manager,metadata); scoped.setScope(local,remote); QTRY_COMPARE(scoped.rowCount(),2);
            QCOMPARE(scoped.data(scoped.index(0),Qt::UserRole).toMap()["workId"].toUuid(),second.id);
            QCOMPARE(manager.workStore()->find(hidden.key).value().position,hidden.position);
        }
    }
    void queuedCancelReplaysDurableIntent() {
        auto f=file(); auto &manager=engine_->transfers(); const auto key=f->checkpointKey().value();
        QVERIFY(!manager.find(key));
        f->cancel();
        QCOMPARE(manager.workStore()->find(key).value().state,WorkState::Cancelled);
        QCOMPARE(f->getState(),File::FS_CANCELLED);
        f->cancel(); f->resume();
        QVERIFY(!manager.workStore()->find(key).value().eligible);
        QCOMPARE(manager.active(identity_->getUuid(),contact_->getUuid()),size_t(0));
    }
    void browserConsentUsesSharedQueue() {
        auto *files=engine_->getFileManager(); auto &manager=engine_->transfers();
        auto peer=std::make_shared<QueueOfferPeer>(); const auto connection=peer->getConnectionId();
        const auto digest=QCryptographicHash::hash("data",QCryptographicHash::Sha256);
        const auto offer=[&] {
            PeerFileOffer value(peer,connection,1,{},QUuid::createUuid().toRfc4122(),"data.bin",4,"application/octet-stream",digest);
            value.checkpoint_attempt=TransferAttempt{QUuid::createUuid(),minimum_checkpoint_interval,1,
                {0,0,QCryptographicHash::hash(QByteArray{},QCryptographicHash::Sha256),false,false}};
            return value;
        };
        bool live=true;
        const auto consent=[&](const PeerFileOffer &value) {
            return files->expectDownload(identity_->getUuid(),contact_->getUuid(),connection,value.fileId,4,digest,[&] { return live; });
        };
        TransferQueueModel all(manager,metadata),scoped(manager,metadata);
        scoped.setScope(identity_->getUuid(),contact_->getUuid());
        for (bool consent_first : {true,false}) {
            auto value=offer();
            if (consent_first) QVERIFY(consent(value));
            files->receivedFileOffer(*conversation_,value);
            auto incoming=files->getFileFromId(value.fileId,*contact_,File::INCOMING); QVERIFY(incoming);
            if (!consent_first) { QCOMPARE(incoming->getState(),File::FS_OFFERED); QVERIFY(consent(value)); }
            QCOMPARE(incoming->getState(),File::FS_QUEUED);
            const auto key=incoming->checkpointKey().value(); const auto work=manager.workStore()->find(key); QVERIFY(work);
            QCOMPARE(work.value().purpose,TransferPurpose::FileShare);
            QCOMPARE(work.value().state,WorkState::Queued);
            all.refresh(); scoped.refresh();
            QTRY_VERIFY(!entry(all,work.value().id).isEmpty()); QTRY_VERIFY(!entry(scoped,work.value().id).isEmpty());
            QCOMPARE(entry(scoped,work.value().id)["direction"].toString(),QString("Receiving"));
            incoming->cancel(); QCOMPARE(manager.workStore()->find(key).value().state,WorkState::Cancelled);
            QTRY_VERIFY(!manager.find(key)); QTRY_COMPARE(incoming->getState(),File::FS_CANCELLED);
        }
        auto revoked=offer(); QVERIFY(consent(revoked)); live=false;
        files->receivedFileOffer(*conversation_,revoked);
        auto incoming=files->getFileFromId(revoked.fileId,*contact_,File::INCOMING); QVERIFY(incoming);
        QCOMPARE(incoming->getState(),File::FS_OFFERED);
        const auto work=manager.workStore()->find(incoming->checkpointKey().value()); QVERIFY(work);
        QCOMPARE(work.value().state,WorkState::AwaitingAcceptance); QVERIFY(!work.value().eligible);
        QVERIFY(!files->expectDownload(QUuid::createUuid(),contact_->getUuid(),connection,offer().fileId,4,digest,[] { return true; }));
        QCOMPARE(manager.active(identity_->getUuid(),contact_->getUuid()),size_t(0));
        incoming->cancel(); QTRY_VERIFY(!manager.find(incoming->checkpointKey().value()));
    }
    void cleanupTestCase() {
        finish(engine_->transfers().shutdown()); QVERIFY(finish(engine_->getFileManager()->storage().shutdown()));
        conversation_.reset(); contact_=nullptr; identity_=nullptr; engine_.reset(); crypto_.reset();
    }
};
QTEST_MAIN(QueueIntegrationTest)
#include "queue_integration_test.moc"
