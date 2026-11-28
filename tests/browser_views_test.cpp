#include "file_browser_fixture.h"
#include "appearance.h"
#include <QSettings>
#include <QSignalSpy>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickItem>

namespace {
QCoro::Task<void> deliver(Fixture &fixture, Envelope request) {
    // Match production asynchronous delivery: never spin a nested waitFor loop
    // while a QML delegate's mouse/keyboard handler is on the stack.
    auto response = co_await fixture.service->handle(fixture.session, request);
    if (!response) co_return;
    Envelope decoded;
    if (!ds::prot::codec::deserialize(ds::prot::codec::serialize(*response), decoded)) qFatal("response codec");
    emit fixture.service->responseReceived(fixture.connection, decoded);
}
FileBrowserModel::Factory asyncFactory(Fixture &fixture) {
    return [&fixture](FileBrowserModel::Location location, BrowserScope scope) -> Result<std::shared_ptr<BrowseProvider>> {
        if (scope.identity != fixture.identity || (location == FileBrowserModel::Remote && scope.contact != fixture.contact))
            return Error{ErrorCode::PermissionDenied};
        if (location == FileBrowserModel::Local)
            return std::static_pointer_cast<BrowseProvider>(std::make_shared<LocalBrowseProvider>(scope.identity, fixture.localHost()));
        auto context = fixture.remoteContext(scope);
        context.send = [&fixture](Envelope request) {
            ++fixture.sends; const auto id = ++fixture.next; request.setRequestId(id);
            Envelope decoded;
            if (!ds::prot::codec::deserialize(ds::prot::codec::serialize(request), decoded)) qFatal("request codec");
            QTimer::singleShot(0, fixture.service.get(), [&fixture, decoded] { deliver(fixture, decoded); });
            return id;
        };
        return std::static_pointer_cast<BrowseProvider>(std::make_shared<RemoteBrowseProvider>(std::move(context)));
    };
}
}

class BrowserViewsTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        qmlRegisterType<FileBrowserModel>("org.darkspeak", 1, 0, "FileBrowserModel");
    }
    void init() { QSettings().clear(); }
    void preferencesAndValidation() {
        {
            FileBrowserModel model;
            QCOMPARE(model.viewMode(), FileBrowserModel::Details);
            model.setViewMode(FileBrowserModel::SmallIcons);
            model.setColumnVisible("name", false);
            model.setColumnVisible("modified", true);
            model.moveColumn("size", 0);
            model.setColumnWidth("size", 320);
            const auto columns = model.detailColumns();
            model.setColumnWidth("size", 100000);
            model.setColumnVisible("nativePath", true);
            model.moveColumn("size", 20);
            model.setViewMode(FileBrowserModel::ViewMode(42));
            QCOMPARE(model.detailColumns(), columns);
        }
        FileBrowserModel restored;
        QCOMPARE(restored.viewMode(), FileBrowserModel::SmallIcons);
        const auto columns = restored.detailColumns();
        QCOMPARE(columns.first().toMap()["id"].toString(), "size");
        QCOMPARE(columns.first().toMap()["width"].toInt(), 320);
        for (auto column : columns) {
            auto c = column.toMap();
            if (c["id"] == "name" || c["id"] == "modified") QVERIFY(c["visible"].toBool());
        }
        QSettings settings;
        settings.setValue("fileBrowser/viewMode", -1);
        settings.setValue("fileBrowser/details/columnOrder", QStringList{"name", "name", "size", "status", "nativePath"});
        settings.setValue("fileBrowser/details/name/width", -20);
        FileBrowserModel bounded;
        QCOMPARE(bounded.viewMode(), FileBrowserModel::Details);
        QCOMPARE(bounded.detailColumns().first().toMap()["id"].toString(), "name");
        QCOMPARE(bounded.detailColumns().first().toMap()["width"].toInt(), 80);
    }
    void localRemoteModesAndNoContentFetch() {
        Fixture f; FileBrowserModel model; model.setProviderFactory(asyncFactory(f));
        for (bool remote : {false, true}) {
            if (remote) model.openRemote(f.identity, f.contact); else model.openLocal(f.identity);
            QTRY_VERIFY(!model.busy());
            model.openEntry(model.data(model.index(0), FileBrowserModel::Key).toString());
            QTRY_VERIFY(!model.busy()); QCOMPARE(model.rowCount(), 3);
            const int sends = f.sends;
            const auto key = model.data(model.index(0), FileBrowserModel::Key);
            for (auto mode : {FileBrowserModel::Thumbnails, FileBrowserModel::SmallIcons, FileBrowserModel::Details}) {
                model.setViewMode(mode);
                QCOMPARE(model.data(model.index(0), FileBrowserModel::Key), key);
                for (int row = 0; row < model.rowCount(); ++row) {
                    QVERIFY(!model.columnText(row, "name").isEmpty());
                    QVERIFY(!model.columnText(row, "type").isEmpty());
                    QVERIFY(model.columnText(row, "nativePath").isEmpty());
                    if (model.data(model.index(row), FileBrowserModel::Directory).toBool()) QVERIFY(model.columnText(row, "size").isEmpty());
                }
            }
            QCOMPARE(f.sends, sends); // View changes never issue STAT/GET/preview requests.
        }
        f.allowed = false;
        f.session = f.service->bind({f.identity, f.contact, f.connection, [] { return true; }, [&f] { return f.live; }, [&f] { return f.utf8; }});
        model.refresh(); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.state(), FileBrowserModel::Denied);
        for (auto mode : {FileBrowserModel::Thumbnails, FileBrowserModel::SmallIcons, FileBrowserModel::Details}) {
            model.setViewMode(mode); QCOMPARE(model.rowCount(), 0); QVERIFY(model.columnText(0, "name").isEmpty());
        }
    }
    void qmlModes_data() { QTest::addColumn<bool>("remote"); QTest::newRow("local") << false; QTest::newRow("remote") << true; }
    void qmlModes() {
        QFETCH(bool, remote);
        Fixture f; Appearance appearance; QQuickWindow window; window.resize(1000, 700);
        FileBrowserSources sources(asyncFactory(f)); QQmlEngine engine;
        engine.rootContext()->setContextProperty("appearance", &appearance);
        engine.rootContext()->setContextProperty("mainWindow", &window);
        engine.rootContext()->setContextProperty("fileBrowsers", &sources);
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/FileBrowserDialog.qml")));
        std::unique_ptr<QObject> dialog(component.createWithInitialProperties({{"identityId", f.identity}, {"remote", remote}, {"contactId", remote ? f.contact : QUuid{}}}));
        QVERIFY2(dialog, qPrintable(component.errorString()));
        dialog->setProperty("parent", QVariant::fromValue(window.contentItem()));
        QVERIFY(QMetaObject::invokeMethod(dialog.get(), "open")); window.show();
        auto *model = dialog->findChild<FileBrowserModel*>(); QVERIFY(model); QTRY_VERIFY(!model->busy());
        model->openEntry(model->data(model->index(0), FileBrowserModel::Key).toString()); QTRY_VERIFY(!model->busy());
        QCOMPARE(model->rowCount(), 3);
        auto *view = dialog->findChild<QQuickItem*>("browserEntries"); QVERIFY(view);
        auto *control = dialog->findChild<QObject*>("browserViewMode"); QVERIFY(control);
        for (int mode = 0; mode < 3; ++mode) {
            const int sends = f.sends;
            control->setProperty("currentIndex", mode);
            QVERIFY(QMetaObject::invokeMethod(control, "activated", Q_ARG(int, mode)));
            QCOMPARE(int(model->viewMode()), mode); QTest::qWait(30);
            QCOMPARE(view->property("count").toInt(), 3);
            QVERIFY(view->property("cellWidth").toReal() > 0);
            auto *content = view->property("contentItem").value<QQuickItem*>(); QVERIFY(content);
            int rows = 0;
            for (auto *item : content->childItems()) if (!item->property("entryName").toString().isEmpty()) ++rows;
            QCOMPARE(rows, 3);
            QCOMPARE(f.sends, sends);
            QQuickItem *folder = nullptr;
            for (auto *item : content->childItems()) if (item->property("entryName").toString() == "sub") folder = item;
            QVERIFY(folder);
            QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, folder->mapToScene(QPointF(20, folder->height() / 2)).toPoint());
            QTRY_VERIFY(!model->busy()); QCOMPARE(model->currentPath(), "/share/sub");
            model->up(); QTRY_VERIFY(!model->busy()); QCOMPARE(model->currentPath(), "/share");
        }
        const int sends = f.sends;
        auto *columns = dialog->findChild<QObject*>("browserColumns"); QVERIFY(columns);
        QVERIFY(QMetaObject::invokeMethod(columns, "clicked")); QTest::qWait(30);
        QCOMPARE(f.sends, sends);
        QCOMPARE(warnings.size(), 0);
    }
};
QTEST_MAIN(BrowserViewsTest)
#include "browser_views_test.moc"
