#include "file_browser_fixture.h"
#include "appearance.h"
#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QCoroTimer>

namespace {
QString entryKey(FileBrowserModel &model, const QString &name) {
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.data(model.index(row), FileBrowserModel::Name).toString() == name)
            return model.data(model.index(row), FileBrowserModel::Key).toString();
    return {};
}
QQuickItem *visualItem(QQuickItem *root, const QString &name) {
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems()) if (auto *found = visualItem(child, name)) return found;
    return nullptr;
}
class Alias final : public BrowseProvider {
public:
    explicit Alias(QUuid identity) : BrowseProvider({identity, {}}) {}
    int lists = 0;
    bool live = true;
    bool current() const override { return live; }
    void cancel() override { live = false; }
    QCoro::Task<Result<BrowserNode>> root() override {
        BrowserNode node; node.provider = id_; node.info.id = "root"; node.path = "/"; co_return node;
    }
    QCoro::Task<Result<BrowserNode>> stat(BrowserNode node) override { co_return node; }
    QCoro::Task<Result<BrowserNode>> resolveChild(BrowserNode parent, QString name) override {
        BrowserNode node; node.provider = id_; node.info.id = "same-target"; node.info.name = name;
        node.path = parent.path == "/" ? "/" + name : parent.path + "/" + name; co_return node;
    }
    QCoro::Task<Result<BrowserPage>> listPage(BrowserNode parent, QByteArray) override {
        ++lists; auto child = co_await resolveChild(parent, "again");
        BrowserPage page; page.entries.append(child.value()); co_return page;
    }
};

}
class BrowserNavigationTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        qmlRegisterType<FileBrowserModel>("org.darkspeak", 1, 0, "FileBrowserModel");
    }
    void localHistoryBreadcrumbsAndRefresh() {
        Fixture f; FileBrowserModel model; model.setProviderFactory(f.factory());
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVector<FileBrowserModel::State> states;
        connect(&model, &FileBrowserModel::changed, &model, [&] { states.append(model.state()); });
        model.openLocal(f.identity); QTRY_VERIFY(!model.busy()); QCOMPARE(model.currentPath(), "/");
        QVERIFY(!model.canBack()); QVERIFY(!model.canUp());
        model.openEntry(entryKey(model, "share")); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.currentPath(), "/share"); QCOMPARE(model.rowCount(), 3);
        model.openEntry(entryKey(model, "a")); QCOMPARE(model.currentPath(), "/share"); // Files never navigate or enqueue.
        model.openEntry(entryKey(model, "sub")); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.currentPath(), "/share/sub"); QCOMPARE(model.rowCount(), 1);
        model.openEntry(entryKey(model, "deep")); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.state(), FileBrowserModel::Empty); QCOMPARE(model.rowCount(), 0);
        auto crumbs = model.breadcrumbs(); QCOMPARE(crumbs.size(), 4);
        QCOMPARE(crumbs.first().toMap()["label"].toString(), "/");
        QCOMPARE(crumbs[2].toMap()["label"].toString(), "sub");
        model.back(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.currentPath(), "/share/sub");
        model.forward(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.currentPath(), "/share/sub/deep");
        model.up(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.currentPath(), "/share/sub");
        model.openBreadcrumb(0); QTRY_VERIFY(!model.busy()); QCOMPARE(model.currentPath(), "/");
        model.back(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.currentPath(), "/share/sub");
        QVERIFY(model.canForward()); model.openBreadcrumb(1); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.currentPath(), "/share"); QVERIFY(!model.canForward());
        model.refresh(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.rowCount(), 3);
        QVERIFY(states.contains(FileBrowserModel::Known)); QVERIFY(states.contains(FileBrowserModel::Loading));
        QVERIFY(states.contains(FileBrowserModel::Updating)); QVERIFY(states.contains(FileBrowserModel::Complete));
        QVERIFY(states.contains(FileBrowserModel::Empty)); QVERIFY(model.error().isEmpty());
        auto path = model.currentPath(); model.openBreadcrumb(-1); model.openBreadcrumb(99); model.openEntry("stale-key");
        QCOMPARE(model.currentPath(), path);
    }
    void remoteEscapedNavigationAndDenial() {
        Fixture f; f.utf8 = false;
        QVERIFY(QDir().mkpath(f.temp.filePath(QString::fromUtf8("source/selected/unicodé%FF"))));
        QVERIFY(QDir().mkpath(f.temp.filePath("source/selected/" + QString(200, '%'))));
        const auto resources = f.vfs->resources(); QVERIFY(resources);
        QVERIFY(QCoro::waitFor(f.backend->reconcile(resources.value().first().namespace_id, {})));
        FileBrowserModel model; model.setProviderFactory(f.factory());
        model.openRemote(f.identity, f.contact); QTRY_VERIFY(!model.busy());
        model.openEntry(entryKey(model, "share")); QTRY_VERIFY(!model.busy()); QVERIFY(model.ready());
        const auto long_name = QString("%2525").repeated(200);
        QVERIFY(!entryKey(model, long_name).isEmpty());
        model.openEntry(entryKey(model, long_name)); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.currentPath(), "/share/" + long_name); QCOMPARE(model.state(), FileBrowserModel::Empty);
        model.up(); QTRY_VERIFY(!model.busy());
        const auto presented = QString("unicod%C3%A9%2525FF");
        QVERIFY(!entryKey(model, presented).isEmpty());
        int before = f.sends; model.openEntry(entryKey(model, presented)); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.currentPath(), "/share/" + presented); QCOMPARE(model.state(), FileBrowserModel::Empty);
        QCOMPARE(model.breadcrumbs().last().toMap()["label"].toString(), presented);
        QCOMPARE(f.sends - before, 2); // One STAT of the whole route, then one nonrecursive LIST.
        model.up(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.currentPath(), "/share");
        f.allowed = false;
        f.session = f.service->bind({f.identity, f.contact, f.connection, [] { return true; }, [&f] { return f.live; }, [] { return false; }});
        model.refresh(); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.state(), FileBrowserModel::Denied); QCOMPARE(model.rowCount(), 0);
        QVERIFY(!model.more()); QVERIFY(!model.error().isEmpty());
        f.live = false; f.offline_permit = true; QTest::qWait(300);
        QVERIFY(model.state() != FileBrowserModel::OfflineCached); // Denial discarded historical pages.
    }
    void slowRepliesAndLatestNavigationWins() {
        Fixture f; FileBrowserModel model; model.setProviderFactory(f.factory());
        model.openRemote(f.identity, f.contact); QTRY_VERIFY(!model.busy());
        const auto old_key = entryKey(model, "share"); f.delay = 80;
        model.openEntry(old_key); QVERIFY(model.busy()); QCOMPARE(model.currentPath(), "/share");
        QVERIFY(model.state() == FileBrowserModel::Known || model.state() == FileBrowserModel::Loading);
        model.up(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.currentPath(), "/");
        QTest::qWait(110); QCOMPARE(model.rowCount(), 1); QCOMPARE(model.currentPath(), "/");
        const auto before = f.sends;
        for (int i = 0; i < 20; ++i) model.refresh();
        QTRY_VERIFY(!model.busy()); QVERIFY(model.ready());
        QVERIFY(f.sends - before <= 4); // One operation, one cancellation, one latest request.
        model.openEntry(entryKey(model, "share")); QVERIFY(model.busy());
        model.openRemote(f.identity, QUuid::createUuid());
        QTest::qWait(120); QCOMPARE(model.state(), FileBrowserModel::Unavailable);
        QCOMPARE(model.rowCount(), 0); QCOMPARE(model.breadcrumbs().size(), 1);
        model.openEntry(old_key); QCOMPARE(model.rowCount(), 0);
    }
    void offlineCacheAndPrivacyWithdrawal() {
        Fixture f; f.offline_permit = true;
        FileBrowserModel model; model.setProviderFactory(f.factory());
        model.openRemote(f.identity, f.contact); QTRY_VERIFY(!model.busy());
        model.openEntry(entryKey(model, "share")); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.rowCount(), 3); const auto observed = model.lastRefreshed(); QVERIFY(observed.isValid());
        f.live = false; QTRY_COMPARE(model.state(), FileBrowserModel::OfflineCached);
        QCOMPARE(model.rowCount(), 3); QVERIFY(!model.more()); QVERIFY(!model.busy());
        QCOMPARE(model.lastRefreshed(), observed); QVERIFY(model.stateText().contains("cached"));
        const auto requests = f.sends;
        model.up(); QCOMPARE(model.state(), FileBrowserModel::OfflineCached); QCOMPARE(model.rowCount(), 1);
        model.back(); QCOMPARE(model.currentPath(), "/share"); QCOMPARE(model.rowCount(), 3);
        QCOMPARE(f.sends, requests); // Offline navigation never sends or grants a live operation.
        f.offline_permit = false; QTRY_COMPARE(model.state(), FileBrowserModel::Unavailable);
        QCOMPARE(model.rowCount(), 0); QVERIFY(!model.canBack()); QCOMPARE(model.currentPath(), "/");
        f.live = true; model.refresh(); QTRY_VERIFY(!model.busy()); QVERIFY(model.ready());
        QCOMPARE(model.currentPath(), "/");
        model.openRemote(f.identity, QUuid::createUuid()); QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.state(), FileBrowserModel::Unavailable);
    }
    void listingStatesAndEmptyContinuation() {
        class Pages final : public BrowseProvider {
        public:
            explicit Pages(QUuid identity) : BrowseProvider({identity, {}}) {}
            bool live = true;
            bool current() const override { return live; }
            void cancel() override { live = false; }
            QCoro::Task<Result<BrowserNode>> root() override {
                BrowserNode node; node.provider = id_; node.info.id = "root"; node.path = "/"; co_return node;
            }
            QCoro::Task<Result<BrowserNode>> stat(BrowserNode node) override { co_return node; }
            QCoro::Task<Result<BrowserNode>> resolveChild(BrowserNode, QString) override { co_return Error{ErrorCode::UnsupportedCapability}; }
            QCoro::Task<Result<BrowserPage>> listPage(BrowserNode, QByteArray cursor) override {
                QTimer timer; timer.setSingleShot(true); timer.start(5); co_await qCoro(timer).waitForTimeout();
                BrowserPage page;
                if (cursor.isEmpty()) page.continuation = "one";
                if (cursor == "one") {
                    BrowserNode entry; entry.provider = id_; entry.info.id = "file"; entry.info.name = "file";
                    entry.info.type = EntryType::File; page.entries.append(entry); page.continuation = "last";
                }
                co_return page;
            }
        };
        const auto identity = QUuid::createUuid(); auto provider = std::make_shared<Pages>(identity);
        FileBrowserModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setProviderFactory([provider](auto, auto) -> Result<std::shared_ptr<BrowseProvider>> { return std::static_pointer_cast<BrowseProvider>(provider); });
        model.openLocal(identity); QCOMPARE(model.state(), FileBrowserModel::Loading); QVERIFY(!model.ready());
        QTRY_VERIFY(!model.busy()); QCOMPARE(model.state(), FileBrowserModel::PartiallyListed); QCOMPARE(model.rowCount(), 0);
        model.nextPage(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.rowCount(), 1); QCOMPARE(model.state(), FileBrowserModel::PartiallyListed);
        model.nextPage(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.state(), FileBrowserModel::Complete); // An empty last page does not mean an empty folder.
        QVERIFY(!model.more()); model.refresh(); QCOMPARE(model.state(), FileBrowserModel::Updating);
        QTRY_VERIFY(!model.busy()); QCOMPARE(model.state(), FileBrowserModel::PartiallyListed);
    }
    void boundedHistory() {
        auto provider = std::make_shared<Alias>(QUuid::createUuid()); FileBrowserModel model;
        model.setProviderFactory([provider](auto, auto) -> Result<std::shared_ptr<BrowseProvider>> { return std::static_pointer_cast<BrowseProvider>(provider); });
        model.openLocal(provider->scope().identity);
        QTRY_VERIFY(!model.busy());
        for (int i = 0; i < 20; ++i) {
            model.openEntry(entryKey(model, "again")); QTRY_VERIFY(!model.busy());
            model.up(); QTRY_VERIFY(!model.busy());
        }
        int backs = 0;
        while (model.canBack()) { model.back(); QTRY_VERIFY(!model.busy()); QVERIFY(++backs <= 31); }
        QCOMPARE(backs, 31);
    }
    void explicitAliasStepsAreDepthBounded() {
        auto provider = std::make_shared<Alias>(QUuid::createUuid()); FileBrowserModel model;
        model.setProviderFactory([provider](auto, auto) -> Result<std::shared_ptr<BrowseProvider>> { return std::static_pointer_cast<BrowseProvider>(provider); });
        model.openLocal(provider->scope().identity); QTRY_VERIFY(!model.busy()); QCOMPARE(provider->lists, 1);
        for (int step = 1; step <= 64; ++step) {
            model.openEntry(entryKey(model, "again")); QTRY_VERIFY(!model.busy());
            QCOMPARE(model.breadcrumbs().size(), step + 1); QCOMPARE(provider->lists, step + 1);
        }
        const auto path = model.currentPath(); model.openEntry(entryKey(model, "again"));
        QCOMPARE(model.currentPath(), path); QCOMPARE(provider->lists, 65); QVERIFY(!model.error().isEmpty());
    }
    void qmlNavigationControls() {
        Fixture f; Appearance appearance; QQuickWindow window; window.resize(1000, 700);
        FileBrowserSources sources(f.factory()); QQmlEngine engine;
        engine.rootContext()->setContextProperty("appearance", &appearance);
        engine.rootContext()->setContextProperty("mainWindow", &window);
        engine.rootContext()->setContextProperty("fileBrowsers", &sources);
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/FileBrowserDialog.qml")));
        std::unique_ptr<QObject> dialog(component.createWithInitialProperties({{"identityId", f.identity}}));
        QVERIFY2(dialog, qPrintable(component.errorString()));
        dialog->setProperty("parent", QVariant::fromValue(window.contentItem()));
        QVERIFY(QMetaObject::invokeMethod(dialog.get(), "open"));
        auto *model = dialog->findChild<FileBrowserModel*>(); QVERIFY(model); QTRY_VERIFY(!model->busy());
        window.show(); QTest::qWait(30);
        auto *list = dialog->findChild<QQuickItem*>("browserEntries"); QVERIFY(list);
        auto *content = list->property("contentItem").value<QQuickItem*>(); QVERIFY(content);
        QQuickItem *row = nullptr;
        for (auto *item : content->childItems()) if (item->property("entryName").toString() == "share") row = item;
        QVERIFY(row);
        QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, row->mapToScene(QPointF(20, row->height() / 2)).toPoint());
        QTRY_VERIFY(!model->busy()); QCOMPARE(model->currentPath(), "/share");
        model->openEntry(entryKey(*model, "sub")); QTRY_VERIFY(!model->busy());
        QTest::qWait(20);
        auto *crumb = visualItem(window.contentItem(), "browserCrumb0"); QVERIFY(crumb);
        QVERIFY(QMetaObject::invokeMethod(crumb, "clicked")); QTRY_VERIFY(!model->busy()); QCOMPARE(model->currentPath(), "/");
        auto *back = visualItem(window.contentItem(), "browserBack"); QVERIFY(back); QVERIFY(back->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(back, "clicked")); QTRY_VERIFY(!model->busy()); QCOMPARE(model->currentPath(), "/share/sub");
        auto *up = visualItem(window.contentItem(), "browserUp"); QVERIFY(up);
        QVERIFY(QMetaObject::invokeMethod(up, "clicked")); QTRY_VERIFY(!model->busy()); QCOMPARE(model->currentPath(), "/share");
        QVERIFY(QMetaObject::invokeMethod(back, "clicked")); QTRY_VERIFY(!model->busy());
        auto *forward = visualItem(window.contentItem(), "browserForward"); QVERIFY(forward);
        QVERIFY(QMetaObject::invokeMethod(forward, "clicked")); QTRY_VERIFY(!model->busy()); QCOMPARE(model->currentPath(), "/share");
        auto *refresh = visualItem(window.contentItem(), "browserRefresh"); QVERIFY(refresh);
        QVERIFY(QMetaObject::invokeMethod(refresh, "clicked")); QTRY_VERIFY(!model->busy());
        auto *status = dialog->findChild<QObject*>("browserState"); QVERIFY(status);
        QCOMPARE(status->property("text").toString(), model->stateText());
        QCOMPARE(warnings.size(), 0);
    }
};
QTEST_MAIN(BrowserNavigationTest)
#include "browser_navigation_test.moc"
