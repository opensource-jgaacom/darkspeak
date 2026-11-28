#include "file_browser_fixture.h"
#include "appearance.h"
#include <ds/dsengine.h>
#include <ds/crypto.h>
#include <ds/peer.h>
#include <ds/filemanager.h>
using namespace ds::core;
#include <QAbstractItemModelTester>
#include <QSettings>
#include <QSignalSpy>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QCoroTimer>

namespace {
class SelectionProvider final : public BrowseProvider {
public:
    SelectionProvider() : BrowseProvider({QUuid::createUuid(), QUuid::createUuid()}) {}
    int count = 5, downloads = 0, delay = 1;
    bool live = true, permit = true;
    bool current() const override { return live; }
    void cancel() override { live = false; }
    bool canDownload(const BrowserNode &node) const override { return live && permit && node.info.type == EntryType::File; }
    QCoro::Task<Result<QByteArray>> download(BrowserNode node) override {
        QTimer timer; timer.setSingleShot(true); timer.start(delay); co_await qCoro(timer).waitForTimeout();
        if (!canDownload(node)) co_return dsfm::Error{ErrorCode::PermissionDenied};
        ++downloads; co_return QUuid::createUuid().toRfc4122();
    }
    QCoro::Task<Result<BrowserNode>> root() override { BrowserNode node; node.provider = id_; node.info.id = "root"; node.path = "/"; co_return node; }
    QCoro::Task<Result<BrowserNode>> stat(BrowserNode node) override { co_return node; }
    QCoro::Task<Result<BrowserNode>> resolveChild(BrowserNode, QString value) override {
        BrowserNode node; node.provider = id_; node.info.id = "folder"; node.info.name = value; node.path = "/folder"; co_return node;
    }
    QCoro::Task<Result<BrowserPage>> listPage(BrowserNode directory, QByteArray cursor) override {
        BrowserPage page; if (directory.path != "/") co_return page;
        const int begin = cursor.toInt();
        for (int i = begin; i < std::min(count, begin + 100); ++i) {
            BrowserNode node; node.provider = id_; node.info.id = QByteArray::number(i); node.info.name = "file" + QString::number(i);
            node.info.type = i == 2 ? EntryType::Directory : EntryType::File; if (i == 2) node.info.name = "folder";
            node.path = "/" + node.info.name; page.entries.append(node);
        }
        if (begin + 100 < count) page.continuation = QByteArray::number(begin + 100);
        co_return page;
    }
};
void configure(FileBrowserModel &model, std::shared_ptr<SelectionProvider> provider) {
    model.setProviderFactory([provider](auto, BrowserScope scope) -> Result<std::shared_ptr<BrowseProvider>> {
        if (scope != provider->scope()) return dsfm::Error{ErrorCode::PermissionDenied};
        return std::static_pointer_cast<BrowseProvider>(provider);
    });
    model.openRemote(provider->scope().identity, provider->scope().contact);
}
QCoro::Task<void> deliver(Fixture &f, Envelope request) {
    auto response = co_await f.service->handle(f.session, request); if (!response) co_return;
    Envelope decoded; if (!ds::prot::codec::deserialize(ds::prot::codec::serialize(*response), decoded)) qFatal("response codec");
    emit f.service->responseReceived(f.connection, decoded);
    if (decoded.hasRequestDownloadResult() && int(decoded.requestDownloadResult().status()) == 1)
        if (!f.service->publishCatalog(f.session, request.requestId())) qFatal("publish");
}
class OfferPeer final : public Peer {
public:
    OfferPeer() : Peer(ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return INCOMING; }
    bool isConnected() const noexcept override { return false; }
    uint64_t sendFileOfferResponse(const QByteArray &, FileOfferResponseStatus) override { ++rejected; return 0; }
    int rejected = 0;
};
}
class BrowserSelectionTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        ds::crypto::Crypto crypto; // Match production initialization before any VFS fixture.
        QVERIFY(settings_.isValid()); QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        qmlRegisterType<FileBrowserModel>("org.darkspeak", 1, 0, "FileBrowserModel");
    }
    void stableSelectionRangesAndBounds() {
        FileBrowserModel model; auto provider = std::make_shared<SelectionProvider>(); provider->count = 250;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        configure(model, provider); QVERIFY(model.ready()); model.setViewport(0, 10, 0, 2, 0);
        model.selectRow(0); model.selectRow(1, FileBrowserModel::Toggle); QCOMPARE(model.selectedCount(), 2);
        const auto key = model.data(model.index(1), FileBrowserModel::Key);
        model.nextPage(); QCOMPARE(model.rowCount(), 200); QCOMPARE(model.selectedCount(), 2);
        QCOMPARE(model.data(model.index(1), FileBrowserModel::Key), key); QVERIFY(model.data(model.index(1), FileBrowserModel::Selected).toBool());
        model.selectRow(4, FileBrowserModel::Range); QCOMPARE(model.selectedCount(), 4); // Anchor remains file1.
        model.selectAllLoaded(); QCOMPARE(model.selectedCount(), 4); QVERIFY(model.error().contains("128"));
        model.selectRow(199, FileBrowserModel::Range); QCOMPARE(model.selectedCount(), 4);
        model.clearSelection(); QCOMPARE(model.selectedCount(), 0);
        model.selectRow(0);
        for (int row = 1; row <= 17; ++row) if (row != 2) model.selectRow(row, FileBrowserModel::Toggle);
        QCOMPARE(model.selectedCount(), 17); QVERIFY(!model.canDownloadSelection()); QVERIFY(model.downloadReason().contains("16"));
        model.clearSelection();
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        model.selectRow(190); QCOMPARE(changes.size(), 0); // No far-off-screen repaint.
        model.setViewport(185, 199, 190, 2, 1); QVERIFY(changes.size() <= 1);
        QVERIFY(model.data(model.index(190), FileBrowserModel::Selected).toBool());
        model.prepareContext(191); QCOMPARE(model.selectedCount(), 1);
        model.selectRow(190, FileBrowserModel::Toggle); model.prepareContext(191); QCOMPARE(model.selectedCount(), 2);
        QVERIFY(model.contextCanDownload()); provider->permit = false; QVERIFY(!model.contextCanDownload());
        model.requestContextDownloads(); QCOMPARE(provider->downloads, 0);
        provider->permit = true; model.prepareContext(2); QVERIFY(model.contextCanOpen()); QVERIFY(!model.contextCanDownload());
        model.openContext(); QCOMPARE(model.currentPath(), "/folder"); QCOMPARE(model.selectedCount(), 0);
        QVERIFY(!model.contextCanOpen()); model.stop(); QCOMPARE(model.selectedCount(), 0);
    }
    void immutableBatchAndScopeCancellation() {
        FileBrowserModel model; auto provider = std::make_shared<SelectionProvider>(); configure(model, provider);
        model.selectRow(0); model.selectRow(1, FileBrowserModel::Toggle); model.prepareContext(0);
        model.requestContextDownloads(); QVERIFY(model.downloading()); model.selectRow(4); // Batch remains file0/file1.
        QTRY_VERIFY(!model.downloading()); QCOMPARE(provider->downloads, 2); QCOMPARE(model.downloadResults().size(), 2);
        QCOMPARE(model.downloadResults().first().toMap()["name"].toString(), "file0");
        model.selectRow(0); model.selectRow(2, FileBrowserModel::Toggle); QVERIFY(!model.canDownloadSelection());
        model.requestDownloads(); QCOMPARE(provider->downloads, 2);
        model.selectRow(0); model.selectRow(1, FileBrowserModel::Toggle); provider->delay = 50;
        model.requestDownloads(); model.refresh(); QTRY_VERIFY(!model.downloading()); QTRY_VERIFY(!model.busy());
        QCOMPARE(provider->downloads, 3); // Only the already dispatched request; no second stale target.
        QCOMPARE(model.downloadResults().size(), 0); QCOMPARE(model.selectedCount(), 0);
        model.selectRow(0); model.prepareContext(0); model.openRemote(QUuid::createUuid(), QUuid::createUuid());
        QVERIFY(!model.contextCanDownload()); model.requestContextDownloads(); QCOMPARE(provider->downloads, 3);
    }
    void realRemoteGetAndDenial() {
        Fixture f; int staged = 0, admitted = 0; bool receive = true, withdraw_after_admit = false;
        QVector<QByteArray> digests;
        FileShareService::Binding binding{f.identity, f.contact, f.connection, [] { return true; }, [&f] { return f.live; }, [&f] { return f.utf8; }};
        binding.stage_download = [&](Vfs &, NodeId, Request, dsvfs::PreparedRead prepared) -> Result<FileShareService::CatalogDelivery> {
            ++staged; digests.append(prepared.info.sha256);
            auto reader = std::shared_ptr<IFileReader>(std::move(prepared.reader));
            return FileShareService::CatalogDelivery{QUuid::createUuid().toRfc4122(), []() -> Result<void> { return {}; },
                [reader] { reader->close(); }, [] { return false; }};
        };
        f.session = f.service->bind(binding);
        FileBrowserModel model;
        model.setProviderFactory([&](auto, BrowserScope scope) -> Result<std::shared_ptr<BrowseProvider>> {
            auto context = f.remoteContext(scope);
            context.send = [&](Envelope request) {
                const auto id = ++f.next; request.setRequestId(id); Envelope decoded;
                if (!ds::prot::codec::deserialize(ds::prot::codec::serialize(request), decoded)) qFatal("request codec");
                QTimer::singleShot(0, f.service.get(), [&f, decoded] { deliver(f, decoded); }); return id;
            };
            context.accept_download = [&](BrowserNode node, QByteArray transfer) -> Result<void> {
                if (!receive) return dsfm::Error{ErrorCode::PermissionDenied};
                if (transfer.size() != 16 || !node.size_known || node.info.size != 4) qFatal("bad consent");
                ++admitted; if (withdraw_after_admit) f.readable = false; return {};
            };
            context.downloads_allowed = [&] { return receive; };
            return std::static_pointer_cast<BrowseProvider>(std::make_shared<RemoteBrowseProvider>(context));
        });
        model.openRemote(f.identity, f.contact); QTRY_VERIFY(!model.busy());
        model.openEntry(model.data(model.index(0), FileBrowserModel::Key).toString()); QTRY_VERIFY(!model.busy());
        for (int row = 0; row < model.rowCount(); ++row) if (!model.data(model.index(row), FileBrowserModel::Directory).toBool()) model.selectRow(row, FileBrowserModel::Toggle);
        QCOMPARE(model.selectedCount(), 2); QVERIFY(model.canDownloadSelection()); model.requestDownloads(); QTRY_VERIFY(!model.downloading());
        QCOMPARE(staged, 2); QCOMPARE(admitted, 2); QCOMPARE(model.downloadResults().size(), 2);
        for (const auto &digest : digests) QCOMPARE(digest, QCryptographicHash::hash("data", QCryptographicHash::Sha256));
        f.readable = false; model.prepareContext(model.focusedRow()); QVERIFY(model.contextCanDownload());
        model.requestContextDownloads(); QTRY_VERIFY(!model.downloading()); QCOMPARE(staged, 2); QCOMPARE(admitted, 2);
        for (const auto &row : model.downloadResults()) QVERIFY(!row.toMap()["success"].toBool());
        f.readable = true; withdraw_after_admit = true;
        model.requestDownloads(); QTRY_VERIFY(!model.downloading()); QCOMPARE(staged, 3); QCOMPARE(admitted, 3);
        QCOMPARE(model.downloadResults().size(), 2);
        QVERIFY(model.downloadResults().first().toMap()["success"].toBool());
        QVERIFY(!model.downloadResults().last().toMap()["success"].toBool());
        receive = false; QVERIFY(!model.canDownloadSelection());
        f.live = false; QTRY_VERIFY(model.rowCount() == 0); QVERIFY(!model.canDownloadSelection());
    }
    void localAndOfflineSelectionCannotDownload() {
        Fixture f; f.offline_permit = true;
        FileBrowserModel local; local.setProviderFactory(f.factory()); local.openLocal(f.identity); QTRY_VERIFY(!local.busy());
        local.openEntry(local.data(local.index(0), FileBrowserModel::Key).toString()); QTRY_VERIFY(!local.busy());
        local.selectRow(0); local.prepareContext(0); QVERIFY(!local.contextCanDownload());
        local.requestDownloads(); QVERIFY(local.error().contains("remote"));
        FileBrowserModel remote; remote.setProviderFactory(f.factory()); remote.openRemote(f.identity, f.contact); QTRY_VERIFY(!remote.busy());
        remote.openEntry(remote.data(remote.index(0), FileBrowserModel::Key).toString()); QTRY_VERIFY(!remote.busy());
        remote.selectRow(0); f.live = false; QTRY_COMPARE(remote.state(), FileBrowserModel::OfflineCached);
        QCOMPARE(remote.selectedCount(), 0); QCOMPARE(remote.rowCount(), 3);
        remote.selectRow(0); remote.selectRow(1, FileBrowserModel::Range); QCOMPARE(remote.selectedCount(), 2);
        QVERIFY(!remote.canDownloadSelection()); remote.prepareContext(0); QVERIFY(!remote.contextCanDownload());
        remote.up(); QCOMPARE(remote.rowCount(), 1); QCOMPARE(remote.selectedCount(), 0);
        remote.prepareContext(0); QVERIFY(remote.contextCanOpen()); remote.openContext();
        QCOMPARE(remote.rowCount(), 3); QCOMPARE(remote.selectedCount(), 0);
        f.offline_permit = false; QTRY_COMPARE(remote.rowCount(), 0); QCOMPARE(remote.selectedCount(), 0);
    }
    void qmlMouseKeyboardAndContextMenu() {
        FileBrowserModel model; auto provider = std::make_shared<SelectionProvider>(); configure(model, provider);
        Appearance appearance; QQmlEngine engine; QQuickWindow window; window.resize(1000, 800);
        engine.rootContext()->setContextProperty("appearance", &appearance); QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/FileBrowser.qml")));
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{"browser", QVariant::fromValue(&model)}}));
        QVERIFY2(object, qPrintable(component.errorString())); auto *root = qobject_cast<QQuickItem*>(object.get()); QVERIFY(root);
        root->setParentItem(window.contentItem()); root->setWidth(1000); root->setHeight(800); window.show(); QTest::qWait(80);
        auto *view = root->findChild<QQuickItem*>("browserEntries"); QVERIFY(view);
        auto point = [&](int row) {
            auto *content = view->property("contentItem").value<QQuickItem*>();
            for (auto *item : content->childItems()) if (item->property("index").toInt() == row && item->property("entryName").isValid()) return item->mapToScene(QPointF(20, item->height()/2)).toPoint();
            return QPoint(-1,-1);
        };
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point(0)); QCOMPARE(model.selectedCount(), 1);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::ControlModifier, point(1)); QCOMPARE(model.selectedCount(), 2);
        QTest::mouseClick(&window, Qt::RightButton, Qt::NoModifier, point(0)); QCOMPARE(model.selectedCount(), 2);
        auto *menu = root->findChild<QObject*>("browserContextMenu"); QVERIFY(menu); QVERIFY(menu->property("visible").toBool());
        auto *download = root->findChild<QObject*>("browserContextDownload"); QVERIFY(download); QVERIFY(download->property("enabled").toBool());
        provider->permit = false; QTRY_VERIFY(!download->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(download, "triggered")); QCOMPARE(provider->downloads, 0);
        provider->permit = true; QTRY_VERIFY(download->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(download, "triggered")); QTRY_VERIFY(!model.downloading()); QCOMPARE(provider->downloads, 2);
        QMetaObject::invokeMethod(menu, "close"); view->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_Escape); QCOMPARE(model.selectedCount(), 0);
        QTest::keyClick(&window, Qt::Key_Home); QCOMPARE(model.focusedRow(), 0);
        QTest::keyClick(&window, Qt::Key_Down, Qt::ShiftModifier); QVERIFY(model.selectedCount() > 1);
        QTest::keyClick(&window, Qt::Key_Down, Qt::ShiftModifier);
        QTest::keyClick(&window, Qt::Key_F10, Qt::ShiftModifier); QVERIFY(menu->property("visible").toBool());
        QVERIFY(!download->property("enabled").toBool()); // Range includes the folder in Details mode.
        QMetaObject::invokeMethod(menu, "close"); view->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_A, Qt::ControlModifier); QCOMPARE(model.selectedCount(), 5);
        QTest::mouseClick(&window, Qt::RightButton, Qt::NoModifier, point(2)); QCOMPARE(model.selectedCount(), 5); QVERIFY(!download->property("enabled").toBool());
        QMetaObject::invokeMethod(menu, "close");
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point(0));
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, point(1)); QTest::qWait(900); QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, point(1));
        QVERIFY(menu->property("visible").toBool());
        QCOMPARE(warnings.size(), 0);
    }
    void scopedGenericOfferConsent() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        qputenv("DARKSPEAK_PROFILE_ROOT", profile.path().toUtf8());
        qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE", "disposable-browser-test");
        ds::crypto::Crypto crypto;
        auto settings = std::make_unique<QSettings>(profile.filePath("settings.ini"), QSettings::IniFormat);
        settings->setValue("dbpath", profile.filePath("app.sqlite"));
        settings->setValue("downloadLocation", profile.filePath("downloads")); QDir().mkpath(profile.filePath("downloads"));
        settings->setValue("autoAcceptIncomingFiles", false);
        auto engine = std::make_unique<DsEngine>(std::move(settings));
        auto *files = engine->getFileManager(); QVERIFY(QCoro::waitFor(files->storage().ready()));
        IdentityData identity; identity.uuid = QUuid::createUuid(); identity.name = "Fixture";
        identity.cert = ds::crypto::DsCert::create(); identity.hash = identity.cert->getHash().toByteArray(); identity.autoConnect = false;
        auto *local = engine->getIdentityManager()->addIdentity(identity);
        auto data = std::make_unique<ContactData>(); data->identity = local->getId(); data->state = Contact::ACCEPTED;
        auto cert = ds::crypto::DsCert::create(); data->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        data->name = "Fixture peer"; data->address = "fixture-not-networked"; data->autoConnect = false;
        auto *contact = engine->getContactManager()->addContact(std::move(data));
        auto conversation = engine->getConversationManager()->getConversation(contact);
        auto peer = std::make_shared<OfferPeer>(); const auto connection = peer->getConnectionId();
        const auto digest = QCryptographicHash::hash("data", QCryptographicHash::Sha256);
        auto offer = [&]() {
            PeerFileOffer value(peer, connection, 1, {}, QUuid::createUuid().toRfc4122(), "data.bin", 4, "application/octet-stream", digest);
            value.checkpoint_attempt = TransferAttempt{QUuid::createUuid(), minimum_checkpoint_interval, 1,
                {0, 0, QCryptographicHash::hash(QByteArray{}, QCryptographicHash::Sha256), false, false}};
            return value;
        };
        bool live = true;
        auto consent = [&](const PeerFileOffer &value) { return files->expectDownload(local->getUuid(), contact->getUuid(), connection,
            value.fileId, 4, digest, [&] { return live; }); };
        auto first = offer(); QVERIFY(consent(first)); QVERIFY(!consent(first)); files->receivedFileOffer(*conversation, first);
        auto incoming = files->getFileFromId(first.fileId, *contact, File::INCOMING); QVERIFY(incoming);
        QCOMPARE(incoming->getState(), File::FS_QUEUED); incoming->cancel();
        auto early = offer(); files->receivedFileOffer(*conversation, early);
        incoming = files->getFileFromId(early.fileId, *contact, File::INCOMING); QVERIFY(incoming); QCOMPARE(incoming->getState(), File::FS_OFFERED);
        QVERIFY(consent(early)); QCOMPARE(incoming->getState(), File::FS_QUEUED);
        QVERIFY(consent(early)); incoming->cancel(); // Already accepted by generic policy is not an integrity failure.
        auto wrong = offer(); QVERIFY(consent(wrong)); wrong.connectionId = QUuid::createUuid();
        int rejections = peer->rejected; files->receivedFileOffer(*conversation, wrong); QCOMPARE(peer->rejected, rejections + 1);
        QVERIFY(!files->getFileFromId(wrong.fileId, *contact, File::INCOMING));
        wrong.connectionId = connection; wrong.sha512 = QByteArray(32, 'x');
        files->receivedFileOffer(*conversation, wrong); QCOMPARE(peer->rejected, rejections + 2);
        auto unrelated = offer(); files->receivedFileOffer(*conversation, unrelated);
        incoming = files->getFileFromId(unrelated.fileId, *contact, File::INCOMING); QVERIFY(incoming); QCOMPARE(incoming->getState(), File::FS_OFFERED);
        auto revoked = offer(); QVERIFY(consent(revoked)); live = false; files->receivedFileOffer(*conversation, revoked);
        incoming = files->getFileFromId(revoked.fileId, *contact, File::INCOMING); QVERIFY(incoming); QCOMPARE(incoming->getState(), File::FS_OFFERED);
        live = true; local->setAllowMimeTypes({"image/jpeg"}); QVERIFY(!consent(offer())); local->setAllowMimeTypes({"*"});
        QVERIFY(!files->expectDownload(QUuid::createUuid(), contact->getUuid(), connection, offer().fileId, 4, digest, [] { return true; }));
        for (int i = 0; i < 32; ++i) QVERIFY(consent(offer()));
        auto full = consent(offer()); QVERIFY(!full); QCOMPARE(full.error().code, ErrorCode::QueueFull);
        auto shutdown = engine->transfers().shutdown(); QTRY_VERIFY(shutdown.isReady()); QVERIFY(QCoro::waitFor(files->storage().shutdown()));
        QVERIFY(!consent(offer())); // Shutdown closes new explicit consent.
        incoming.reset(); conversation.reset(); engine.reset();
        qunsetenv("DARKSPEAK_PROFILE_ROOT"); qunsetenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE");
    }
};
QTEST_MAIN(BrowserSelectionTest)
#include "browser_selection_test.moc"
