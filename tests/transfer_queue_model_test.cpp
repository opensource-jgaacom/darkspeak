#include <ds/transferqueuemodel.h>
#include <ds/crypto.h>
#include <ds/manager.h>
#include <ds/filemanager.h>
#include "transfer_sender_fixture.h"
#include "appearance.h"
#include <QAbstractItemModelTester>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickItem>
#include <QtTest>
#include <QCoroTimer>
using namespace ds::core;
using namespace ds::models;
using namespace dsfm;

class TransferQueueModelTest final:public QObject {
    Q_OBJECT
    QUuid local_=QUuid::createUuid(),remote_=QUuid::createUuid();
    QByteArray hash_=QCryptographicHash::hash("fixture",QCryptographicHash::Sha256);
    QSqlDatabase database(QString path=":memory:") {
        auto db=QSqlDatabase::addDatabase("QSQLITE",QUuid::createUuid().toString()); db.setDatabaseName(path);
        if(!db.open()) qFatal("fixture db");
        QSqlQuery q(db); if(!q.exec("CREATE TABLE IF NOT EXISTS file(id INTEGER PRIMARY KEY)")) qFatal("fixture table");
        return db;
    }
    TransferWork add(TransferManager &manager,QSqlDatabase db,int id,WorkState state=WorkState::Queued,
                     TransferSide side=TransferSide::Sender,QUuid local={},QUuid remote={}) {
        QSqlQuery q(db); q.prepare("INSERT INTO file VALUES(?)"); q.addBindValue(id); if(!q.exec()) qFatal("fixture file");
        TransferKey key{local.isNull()?local_:local,remote.isNull()?remote_:remote,QUuid::createUuid(),side};
        auto w=manager.workStore()->enroll(id,key,TransferPurpose::FileShare,100,hash_,true); if(!w) qFatal("fixture work");
        if(!manager.workStore()->project(key,state,state==WorkState::Queued)) qFatal("fixture state");
        return manager.workStore()->find(key).value();
    }
    static QCoro::Task<TransferQueueModel::Metadata> read(TransferWork w) {
        TransferQueueModel::Metadata meta; meta.title="<b>payload</b>\n\u202e"; meta.identity="Same name"; meta.contact="Same name";
        meta.provisional=80; meta.verified=64; meta.journal_known=true; meta.can_accept=false; Q_UNUSED(w); co_return meta;
    }
    static QObject *visualChild(QQuickItem *item,const QString &name) {
        if(item->objectName()==name) return item;
        for(auto *child:item->childItems()) if(auto *found=visualChild(child,name)) return found;
        return nullptr;
    }
    QVariantMap row(TransferQueueModel &m,int at=0) { return m.data(m.index(at),Qt::UserRole).toMap(); }
    template<class T> T wait(QCoro::Task<T> task) { return QCoro::waitFor(std::move(task)); }
private slots:
    void scopesCountsAndCommands() {
        auto db=database(); TransferManager manager; QVERIFY(manager.openWorkStore(db));
        const auto a=add(manager,db,1),b=add(manager,db,2,WorkState::Paused,TransferSide::Receiver);
        const auto other_remote=QUuid::createUuid(),other_local=QUuid::createUuid();
        const auto hidden=add(manager,db,3,WorkState::Active,TransferSide::Sender,{},other_remote);
        add(manager,db,4,WorkState::Completed,TransferSide::Receiver,other_local,remote_);
        auto offer=add(manager,db,5,WorkState::AwaitingAcceptance,TransferSide::Receiver);
        TransferQueueModel model(manager,read,[](TransferWork,bool)->Result<void>{return dsfm::Error{ErrorCode::PermissionDenied};});
        QAbstractItemModelTester tester(&model,QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy reset(&model,&QAbstractItemModel::modelReset),result(&model,&TransferQueueModel::commandFinished);
        QTRY_COMPARE(model.rowCount(),5); QCOMPARE(model.counts()["sending"].toInt(),2); QCOMPARE(model.counts()["receiving"].toInt(),3);
        QCOMPARE(row(model)["provisionalBytes"].toULongLong(),80ULL); QCOMPARE(row(model)["verifiedBytes"].toULongLong(),64ULL);
        QVERIFY(!row(model)["title"].toString().contains(QChar(0x202e)));
        QVERIFY(model.setScope(local_,remote_,"Local","Contact")); QTRY_COMPARE(model.rowCount(),3);
        QCOMPARE(model.counts()["total"].toInt(),3); QCOMPARE(model.applicationCounts()["active"].toInt(),1);
        model.command(hidden.id,"cancel"); QCOMPARE(result.size(),1); QCOMPARE(manager.workStore()->find(hidden.key).value().state,WorkState::Active);
        QVERIFY(!model.setScope({},remote_)); QCOMPARE(model.contactId(),remote_);
        model.command(a.id,"priority",0); QTRY_COMPARE(result.size(),2); QCOMPARE(manager.workStore()->find(a.key).value().priority,TransferPriority::High);
        model.command(a.id,"pause"); QTRY_COMPARE(result.size(),3); QVERIFY(manager.workStore()->find(a.key).value().user_paused);
        QTRY_VERIFY(row(model)["canResume"].toBool()); model.command(a.id,"resume"); QTRY_COMPARE(result.size(),4);
        QVERIFY(!manager.workStore()->find(a.key).value().user_paused);
        model.command(offer.id,"accept"); QTRY_COMPARE(result.size(),5); QCOMPARE(manager.workStore()->find(offer.key).value().state,WorkState::AwaitingAcceptance);
        QTest::qWait(30); QVERIFY(!model.error().isEmpty());
        model.setDirection(int(TransferSide::Receiver)); QTRY_COMPARE(model.rowCount(),2);
        model.setCategory(3); QTRY_COMPARE(model.rowCount(),2);
        model.setScope(); QTRY_COMPARE(model.rowCount(),2); // tabs/direction survive navigation
        model.setCategory(4); QTRY_COMPARE(model.rowCount(),1);
        QCOMPARE(row(model)["state"].toString(),"Completed");
        reset.clear(); model.refresh(); QTest::qWait(30); QCOMPARE(reset.size(),0);
        Q_UNUSED(b);
        auto closing=manager.shutdown(); QTRY_VERIFY(closing.isReady());
    }
    void reorderPersistenceAndStaleScope() {
        QTemporaryDir dir; const auto path=dir.filePath("work.sqlite");
        TransferWork a,b,hidden;
        {
            auto db=database(path); TransferManager manager; QVERIFY(manager.openWorkStore(db));
            a=add(manager,db,1); hidden=add(manager,db,2,WorkState::Queued,TransferSide::Sender,{},QUuid::createUuid()); b=add(manager,db,3);
            TransferQueueModel m(manager,read); m.setScope(local_,remote_); QTRY_COMPARE(m.rowCount(),2);
            QSignalSpy done(&m,&TransferQueueModel::commandFinished);
            m.command(b.id,"up"); QTRY_COMPARE(done.size(),1); QTRY_COMPARE(row(m)["workId"].toUuid(),b.id);
            QCOMPARE(manager.workStore()->find(hidden.key).value().position,hidden.position);
            m.command(a.id,"priority",0); m.setScope(QUuid::createUuid()); QTRY_COMPARE(done.size(),2);
            QCOMPARE(manager.workStore()->find(a.key).value().priority,TransferPriority::Normal);
            auto closing=manager.shutdown(); QTRY_VERIFY(closing.isReady());
        }
        {
            auto db=database(path); TransferManager manager; QVERIFY(manager.openWorkStore(db));
            TransferQueueModel m(manager,read); m.setScope(local_,remote_); QTRY_COMPARE(m.rowCount(),2);
            QCOMPARE(row(m)["workId"].toUuid(),b.id);
            auto closing=manager.shutdown(); QTRY_VERIFY(closing.isReady());
        }
    }
    void pagingAndStaleJournalRead() {
        auto db=database(); TransferManager manager; QVERIFY(manager.openWorkStore(db));
        for(int i=1;i<=230;++i) add(manager,db,i,WorkState::Queued,TransferSide::Receiver,{},QUuid::createUuid());
        int reads=0;
        TransferQueueModel m(manager,[&](TransferWork w)->QCoro::Task<TransferQueueModel::Metadata>{ ++reads; co_return co_await read(w); });
        QTRY_COMPARE(m.rowCount(),100); QVERIFY(reads<=100); QCOMPARE(m.counts()["total"].toInt(),230);
        m.fetchMore({}); QTRY_COMPARE(m.rowCount(),200); m.fetchMore({}); QTRY_COMPARE(m.rowCount(),230); QVERIFY(!m.canFetchMore({}));
        QVERIFY(!manager.workStore()->viewPage({{},remote_},0)); QVERIFY(!manager.workStore()->viewPage({},0,101));
        TransferQueueModel slow(manager,[](TransferWork w)->QCoro::Task<TransferQueueModel::Metadata>{
            QTimer delay; delay.setSingleShot(true); delay.start(20); co_await delay; co_return co_await read(w);
        });
        QTest::qWait(5); slow.setScope(QUuid::createUuid()); QTRY_COMPARE(slow.counts()["total"].toInt(),0); QTest::qWait(40); QCOMPARE(slow.rowCount(),0);
        // A view can be destroyed with its journal read in flight.
        auto *destroyed=new TransferQueueModel(manager,[](TransferWork w)->QCoro::Task<TransferQueueModel::Metadata>{
            QTimer delay; delay.setSingleShot(true); delay.start(20); co_await delay; co_return co_await read(w);
        });
        QTest::qWait(5); delete destroyed; QTest::qWait(30);
        auto closing=manager.shutdown(); QTRY_VERIFY(closing.isReady());
    }
    void productionJournalAndContent() {
        QTemporaryDir profile; qputenv("DARKSPEAK_PROFILE_ROOT",profile.path().toUtf8());
        qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE","disposable-transfer-ui-test");
        ds::crypto::Crypto crypto;
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,profile.path());
        auto settings=std::make_unique<QSettings>();
        settings->setValue("dbpath",profile.filePath("app.sqlite")); settings->setValue("downloadLocation",profile.filePath("downloads")); QDir().mkpath(profile.filePath("downloads"));
        settings->sync(); settings.reset();
        Manager shell;
        shell.setCurrentPage(Manager::TRANSFERS); QCOMPARE(shell.getCurrentPage(),int(Manager::TRANSFERS));
        shell.setCurrentPage(Manager::TRANSFERS+1); QCOMPARE(shell.getCurrentPage(),int(Manager::TRANSFERS));
        auto &engine=DsEngine::instance(); auto *files=engine.getFileManager(); QVERIFY(wait(files->storage().ready()));
        IdentityData data; data.uuid=QUuid::createUuid(); data.name="Queue fixture"; data.cert=ds::crypto::DsCert::create(); data.hash=data.cert->getHash().toByteArray(); data.autoConnect=false;
        auto *identity=engine.getIdentityManager()->addIdentity(data);
        auto peer=std::make_unique<ContactData>(); peer->identity=identity->getId(); peer->state=Contact::ACCEPTED;
        auto certificate=ds::crypto::DsCert::create(); peer->cert=ds::crypto::DsCert::createFromPubkey(certificate->getSigningPubKey().toByteArray()); peer->name="Peer"; peer->address="fixture-no-network"; peer->autoConnect=false;
        auto *contact=engine.getContactManager()->addContact(std::move(peer)); auto conversation=engine.getConversationManager()->getConversation(contact);
        const QByteArray payload(2*minimum_checkpoint_interval+37,'q');
        QFile input(profile.filePath("source.bin")); QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write(payload),payload.size()); input.close();
        auto imported=wait(files->storage().importFile(input.fileName(),"payload.bin")); QVERIFY(imported);
        auto fd=std::make_unique<FileData>(); fd->state=File::FS_WAITING; fd->identity=identity->getId(); fd->contact=contact->getId(); fd->conversation=conversation->getId(); fd->name="payload.bin";
        fd->hash=QCryptographicHash::hash(payload,QCryptographicHash::Sha256); fd->size=payload.size(); fd->storage_namespace=imported.value().namespace_id; fd->storage_id=imported.value().file.id;
        auto file=files->addFile(std::move(fd)); auto key=file->checkpointKey().value(); auto &manager=engine.transfers(); QVERIFY(manager.bindWork(file));
        auto work=manager.workStore()->find(key).value(); auto reader=applicationTransferReader(manager);
        file->setFlowProgress(123); auto before=wait(reader(work)); QCOMPARE(before.provisional,123ULL); QVERIFY(!before.journal_known); QCOMPARE(before.verified,0ULL);
        auto sender=std::make_shared<CheckpointSendingPeer>(); sender->begin(file); QTRY_COMPARE_WITH_TIMEOUT(file->getState(),File::FS_DONE,10000);
        QTRY_VERIFY(!manager.find(key)); QCOMPARE(sender->sent,payload); QCOMPARE(QCryptographicHash::hash(sender->sent,QCryptographicHash::Sha256),file->getHash());
        work=manager.workStore()->find(key).value(); auto after=wait(reader(work)); QVERIFY(after.journal_known); QCOMPARE(after.verified,quint64(payload.size())); QCOMPARE(after.title,"payload.bin");
        auto wrong=work; wrong.sha256=QByteArray(32,'x'); QVERIFY(!wait(reader(wrong)).journal_known);
        wrong=work; wrong.key.local_scope=QUuid::createUuid(); auto isolated=wait(reader(wrong)); QVERIFY(isolated.title.isEmpty()); QVERIFY(!isolated.journal_known);
        TransferQueueModel m(manager,reader,applicationTransferDecision(manager)); QTRY_COMPARE(m.rowCount(),1); QCOMPARE(row(m)["verifiedBytes"].toULongLong(),quint64(payload.size())); QVERIFY(!row(m)["canCancel"].toBool());
        QVERIFY(wait(files->storage().shutdown()));
    }
    void populatedQml() {
        auto db=database(); TransferManager manager; QVERIFY(manager.openWorkStore(db)); add(manager,db,1); add(manager,db,2,WorkState::Active,TransferSide::Receiver);
        TransferQueueModel model(manager,read); QTRY_COMPARE(model.rowCount(),2);
        QTemporaryDir settings; QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
        Appearance appearance; QQuickWindow window; window.resize(1366,768); QQmlEngine engine;
        engine.rootContext()->setContextProperty("appearance",&appearance); engine.rootContext()->setContextProperty("transferQueue",&model);
        QSignalSpy warnings(&engine,&QQmlEngine::warnings);
        QQmlComponent component(&engine,QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/Transfers.qml")));
        std::unique_ptr<QObject> page(component.create()); QVERIFY2(page,qPrintable(component.errorString())); auto *item=qobject_cast<QQuickItem*>(page.get()); QVERIFY(item); item->setParentItem(window.contentItem()); item->setWidth(1366); item->setHeight(768); window.show();
        auto *list=page->findChild<QQuickItem*>("transferList"); QVERIFY(list); QTRY_COMPARE(list->property("count").toInt(),2);
        QTRY_VERIFY(visualChild(list,"transferPriority")); auto *priority=visualChild(list,"transferPriority"); priority->setProperty("currentIndex",0); QVERIFY(QMetaObject::invokeMethod(priority,"activated",Q_ARG(int,0)));
        QTRY_COMPARE(row(model)["priority"].toInt(),0);
        auto *direction=page->findChild<QObject*>("transferDirection"); QVERIFY(direction); direction->setProperty("currentIndex",2); QVERIFY(QMetaObject::invokeMethod(direction,"activated",Q_ARG(int,2))); QTRY_COMPARE(model.rowCount(),1);
        QCOMPARE(row(model)["canPause"].toBool(),false); // active pause is not offered
        appearance.setScale(200); QTest::qWait(30); appearance.setScale(20); QTest::qWait(30);
        QCOMPARE(warnings.size(),0);
        auto closing=manager.shutdown(); QTRY_VERIFY(closing.isReady());
    }
};
QTEST_MAIN(TransferQueueModelTest)
#include "transfer_queue_model_test.moc"
