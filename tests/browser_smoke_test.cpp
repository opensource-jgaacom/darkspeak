#include "file_browser_fixture.h"
#include "appearance.h"
#include <ds/crypto.h>
#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>
#include <QSettings>
#include <QSignalSpy>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QEventLoop>
#include <logfault/logfault.h>
#include <iostream>

namespace {
template<class Predicate> bool waitForModel(FileBrowserModel &model, Predicate done) {
    // QTRY's sleep between event batches compounds across asynchronous filesystem
    // calls. Wait on the model signal so this fixture runs a normal event loop.
    QEventLoop loop;
    QTimer deadline; deadline.setSingleShot(true); deadline.start(10000);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&model, &FileBrowserModel::changed, &loop, &QEventLoop::quit);
    QObject::connect(&model, &FileBrowserModel::selectionChanged, &loop, &QEventLoop::quit);
    while (!done() && deadline.isActive()) loop.exec();
    return done();
}
bool waitForListing(FileBrowserModel &model) {
    return waitForModel(model, [&] { return !model.busy(); });
}
int rowNamed(FileBrowserModel &model, const QString &name) {
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.data(model.index(row), FileBrowserModel::Name).toString() == name) return row;
    return -1;
}
void openNamed(FileBrowserModel &model, const QString &name) {
    model.openEntry(model.data(model.index(rowNamed(model, name)), FileBrowserModel::Key).toString());
}
QQuickItem *visualItem(QQuickItem *root, const QString &name) {
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems()) if (auto *found = visualItem(child, name)) return found;
    return nullptr;
}
QCoro::Task<void> deliver(Fixture &fixture, Envelope request) {
    auto response = co_await fixture.service->handle(fixture.session, request);
    if (!response) co_return;
    Envelope decoded;
    if (!ds::prot::codec::deserialize(ds::prot::codec::serialize(*response), decoded)) qFatal("response codec");
    emit fixture.service->responseReceived(fixture.connection, decoded);
    if (decoded.hasRequestDownloadResult() && int(decoded.requestDownloadResult().status()) == 1)
        if (!fixture.service->publishCatalog(fixture.session, request.requestId())) qFatal("download publication");
}
}

class BrowserSmokeTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        if (qEnvironmentVariableIsSet("DS_BROWSER_TRACE"))
            logfault::LogManager::Instance().AddHandler(std::make_unique<logfault::StreamHandler>(std::clog, logfault::LogLevel::TRACE));
        ds::crypto::Crypto crypto;
        QVERIFY(settings_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        qmlRegisterType<FileBrowserModel>("org.darkspeak", 1, 0, "FileBrowserModel");
    }

    void realProviderPagesViewportAndLocationFence() {
        Fixture fixture(205);
        FileBrowserModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setProviderFactory(fixture.factory());
        model.openLocal(fixture.identity); QVERIFY(waitForListing(model));
        QCOMPARE(model.location(), FileBrowserModel::Local);
        QCOMPARE(model.contactId(), QUuid());
        openNamed(model, "share"); QVERIFY(waitForListing(model));
        model.selectRow(rowNamed(model, "a")); QVERIFY(!model.canDownloadSelection());
        const auto local_key = model.data(model.index(0), FileBrowserModel::Key);

        fixture.delay = 40;
        model.openRemote(fixture.identity, fixture.contact);
        QVERIFY(model.busy()); QCOMPARE(model.rowCount(), 0); QCOMPARE(model.selectedCount(), 0);
        QVERIFY(waitForListing(model)); openNamed(model, "share"); QVERIFY(waitForListing(model));
        QCOMPARE(model.location(), FileBrowserModel::Remote); QCOMPARE(model.contactId(), fixture.contact);
        QCOMPARE(model.state(), FileBrowserModel::PartiallyListed); QCOMPARE(model.rowCount(), 100);
        QVERIFY(model.data(model.index(0), FileBrowserModel::Key) != local_key);
        QPersistentModelIndex first(model.index(0)); const auto first_key = first.data(FileBrowserModel::Key);
        model.selectRow(rowNamed(model, "a"));
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        model.setViewport(90, 99, -1, 2, 1);
        changes.clear(); // Ignore the one bounded repaint when changing the viewport.
        QVERIFY(waitForModel(model, [&] { return model.rowCount() == 200 && !model.busy(); }));
        QCOMPARE(model.selectedCount(), 1); QCOMPARE(first.data(FileBrowserModel::Key), first_key);
        QCOMPARE(resets.size(), 0); QCOMPARE(inserts.size(), 1); QCOMPARE(changes.size(), 0);
        QCOMPARE(inserts.first()[1].toInt(), 100); QCOMPARE(inserts.first()[2].toInt(), 199);
        model.selectRow(150); // Neither the old nor new selection is near the viewport.
        QCOMPARE(changes.size(), 0);
        model.setViewport(145, 155, 150, 2, 1);
        QVERIFY(changes.size() <= 1);
        for (const auto &args : changes) {
            QVERIFY(args[0].value<QModelIndex>().row() >= 143);
            QVERIFY(args[1].value<QModelIndex>().row() <= 157);
        }
        QVERIFY(model.data(model.index(150), FileBrowserModel::Selected).toBool());
        model.nextPage(); QVERIFY(model.busy());
        model.openLocal(fixture.identity); QVERIFY(waitForListing(model));
        QTest::qWait(100); // Allow the old connection's delayed page to arrive.
        QCOMPARE(model.location(), FileBrowserModel::Local); QCOMPARE(model.currentPath(), "/");
        QCOMPARE(model.contactId(), QUuid()); QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.selectedCount(), 0); QVERIFY(!first.isValid());
        QVERIFY(!model.canBack()); QVERIFY(!model.canDownloadSelection());
        model.openRemote(fixture.identity, QUuid::createUuid()); QVERIFY(waitForListing(model));
        QCOMPARE(model.state(), FileBrowserModel::Unavailable); QCOMPARE(model.rowCount(), 0);
    }

    void qmlRealRemoteDownloadAndAuthorization() {
        Fixture fixture(105);
        int staged = 0, admitted = 0;
        bool receive = true;
        QVector<QByteArray> digests;
        FileShareService::Binding binding{fixture.identity, fixture.contact, fixture.connection,
            [] { return true; }, [&] { return fixture.live; }, [&] { return fixture.utf8; }};
        binding.stage_download = [&](Vfs &, NodeId, Request, dsvfs::PreparedRead prepared) -> Result<FileShareService::CatalogDelivery> {
            ++staged; digests.append(prepared.info.sha256);
            auto reader = std::shared_ptr<IFileReader>(std::move(prepared.reader));
            return FileShareService::CatalogDelivery{QUuid::createUuid().toRfc4122(), []() -> Result<void> { return {}; },
                [reader] { reader->close(); }, [] { return false; }};
        };
        fixture.session = fixture.service->bind(binding);
        FileBrowserModel model;
        model.setViewMode(FileBrowserModel::Details);
        model.setProviderFactory([&](auto location, BrowserScope scope) -> Result<std::shared_ptr<BrowseProvider>> {
            if (scope.identity != fixture.identity || (location == FileBrowserModel::Remote && scope.contact != fixture.contact))
                return Error{ErrorCode::PermissionDenied};
            if (location == FileBrowserModel::Local)
                return std::static_pointer_cast<BrowseProvider>(std::make_shared<LocalBrowseProvider>(scope.identity, fixture.localHost()));
            auto context = fixture.remoteContext(scope);
            context.send = [&](Envelope request) {
                ++fixture.sends; const auto id = ++fixture.next; request.setRequestId(id);
                Envelope decoded;
                if (!ds::prot::codec::deserialize(ds::prot::codec::serialize(request), decoded)) qFatal("request codec");
                QTimer::singleShot(40, fixture.service.get(), [&fixture, decoded] { deliver(fixture, decoded); });
                return id;
            };
            context.accept_download = [&](BrowserNode node, QByteArray transfer) -> Result<void> {
                if (!receive) return Error{ErrorCode::PermissionDenied};
                if (transfer.size() != 16 || !node.size_known || node.info.size != 4) qFatal("download consent");
                ++admitted; return {};
            };
            context.downloads_allowed = [&] { return receive; };
            return std::static_pointer_cast<BrowseProvider>(std::make_shared<RemoteBrowseProvider>(context));
        });
        Appearance appearance; QQmlEngine engine; QQuickWindow window; window.resize(1000, 700);
        engine.rootContext()->setContextProperty("appearance", &appearance);
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/FileBrowser.qml")));
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{"browser", QVariant::fromValue(&model)}}));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *root = qobject_cast<QQuickItem*>(object.get()); QVERIFY(root);
        root->setParentItem(window.contentItem()); root->setWidth(1000); root->setHeight(700); window.show();
        auto *download = root->findChild<QQuickItem*>("browserDownloadSelection"); QVERIFY(download);
        auto *status = root->findChild<QObject*>("browserState"); QVERIFY(status);
        model.openLocal(fixture.identity); QVERIFY(waitForListing(model));
        openNamed(model, "share"); QVERIFY(waitForListing(model)); model.selectRow(rowNamed(model, "a"));
        QVERIFY(!download->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(download, "clicked")); QCOMPARE(staged, 0);
        model.openRemote(fixture.identity, fixture.contact); QVERIFY(waitForListing(model));
        openNamed(model, "share"); QVERIFY(model.busy());
        QVERIFY(waitForListing(model)); QCOMPARE(model.rowCount(), 100);
        QCOMPARE(model.state(), FileBrowserModel::PartiallyListed);
        QCOMPARE(status->property("text").toString(), model.stateText());
        auto *next = root->findChild<QQuickItem*>("nextBrowserPage"); QVERIFY(next); QVERIFY(next->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(next, "clicked")); QVERIFY(model.busy());
        QVERIFY(waitForListing(model)); QCOMPARE(model.rowCount(), 108);
        QCOMPARE(model.state(), FileBrowserModel::Complete);
        auto *view = root->findChild<QQuickItem*>("browserEntries"); QVERIFY(view);
        QTRY_VERIFY(view->property("contentItem").value<QQuickItem*>());
        auto point = [&](int row) {
            auto *content = view->property("contentItem").value<QQuickItem*>();
            for (auto *item : content->childItems())
                if (item->property("entryName").isValid() && item->property("index").toInt() == row)
                    return item->mapToScene(QPointF(20, item->height() / 2)).toPoint();
            return QPoint(-1, -1);
        };
        const int first_file = rowNamed(model, "a"), second_file = rowNamed(model, "f000");
        QVERIFY(first_file >= 0); QVERIFY(second_file >= 0);
        QTRY_VERIFY(point(first_file).x() >= 0 && point(second_file).x() >= 0);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point(first_file));
        QTest::mouseClick(&window, Qt::LeftButton, Qt::ControlModifier, point(second_file));
        QCOMPARE(model.selectedCount(), 2); QTRY_VERIFY(download->isEnabled());
        QVERIFY(download->property("text").toString().contains("(2)")); QCOMPARE(staged, 0);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, download->mapToScene(QPointF(download->width()/2, download->height()/2)).toPoint());
        QVERIFY(waitForModel(model, [&] { return !model.downloading(); })); QCOMPARE(admitted, 2); QCOMPARE(staged, 2);
        QCOMPARE(model.downloadResults().size(), 2);
        for (const auto &result : model.downloadResults()) QVERIFY(result.toMap()["success"].toBool());
        for (const auto &digest : digests) QCOMPARE(digest, QCryptographicHash::hash("data", QCryptographicHash::Sha256));

        fixture.readable = false; // LIST visibility cannot authorize a GET.
        QVERIFY(QMetaObject::invokeMethod(download, "clicked"));
        QVERIFY(waitForModel(model, [&] { return !model.downloading(); }));
        QCOMPARE(staged, 2); QCOMPARE(admitted, 2); QCOMPARE(model.downloadResults().size(), 2);
        for (const auto &result : model.downloadResults()) QVERIFY(!result.toMap()["success"].toBool());
        receive = false; QTRY_VERIFY(!download->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(download, "clicked")); QCOMPARE(staged, 2); QVERIFY(!model.downloading());

        openNamed(model, "sub"); QVERIFY(waitForListing(model));
        QCOMPARE(model.currentPath(), "/share/sub"); QCOMPARE(model.selectedCount(), 0);
        auto *parent = visualItem(root, "browserCrumb1"); QVERIFY(parent);
        QVERIFY(QMetaObject::invokeMethod(parent, "clicked")); QVERIFY(waitForListing(model)); QCOMPARE(model.currentPath(), "/share");
        auto *home = visualItem(root, "browserCrumb0"); QVERIFY(home);
        QVERIFY(QMetaObject::invokeMethod(home, "clicked")); QVERIFY(waitForListing(model)); QCOMPARE(model.currentPath(), "/");
        fixture.allowed = false; model.refresh(); QVERIFY(waitForListing(model));
        QCOMPARE(model.state(), FileBrowserModel::Denied); QCOMPARE(model.rowCount(), 0); QVERIFY(!download->isEnabled());
        QCOMPARE(warnings.size(), 0);
        model.stop();
    }
};
QTEST_MAIN(BrowserSmokeTest)
#include "browser_smoke_test.moc"
