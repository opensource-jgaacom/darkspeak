#include "file_browser_fixture.h"
#include "appearance.h"
#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QSettings>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QCoroTimer>
#include <QElapsedTimer>
#include <logfault/logfault.h>
#include <iostream>

namespace {
class Synthetic final : public BrowseProvider {
public:
    explicit Synthetic(int count) : BrowseProvider({QUuid::createUuid(), {}}), count(count) {}
    int count, calls = 0, delay = 0;
    bool live = true, duplicate = false, cycle = false, empty = false, large = false;
    dsvfs::ChangeCallback callback;
    bool current() const override { return live; }
    void cancel() override { live = false; unwatch(); }
    void unwatch() override { callback = {}; }
    QCoro::Task<Result<void>> watch(BrowserNode, dsvfs::ChangeCallback cb) override { callback = std::move(cb); co_return Result<void>{}; }
    QCoro::Task<Result<BrowserNode>> root() override {
        BrowserNode node; node.provider = id_; node.info.id = "root"; node.path = "/"; co_return node;
    }
    QCoro::Task<Result<BrowserNode>> stat(BrowserNode node) override { co_return node; }
    QCoro::Task<Result<BrowserNode>> resolveChild(BrowserNode, QString) override { co_return Error{ErrorCode::MissingEntry}; }
    QCoro::Task<Result<BrowserPage>> listPage(BrowserNode, QByteArray cursor) override {
        ++calls;
        QTimer timer; timer.setSingleShot(true); timer.start(delay); co_await qCoro(timer).waitForTimeout();
        if (!live) co_return Error{ErrorCode::PermissionDenied};
        const int start = cursor.toInt(); BrowserPage page;
        for (int i = start; !empty && i < std::min(start + 100, count); ++i) {
            BrowserNode node; node.provider = id_; node.info.id = QByteArray::number(duplicate && start ? 0 : i);
            node.info.name = QString("file%1").arg(i, 6, 10, QChar('0'));
            node.path = "/" + node.info.name; node.info.type = EntryType::File;
            if (large) { node.info.name = QString(765, 'x'); node.path = QString(4096, 'x'); }
            page.entries.append(std::move(node));
        }
        if (empty || start + 100 < count) page.continuation = QByteArray::number(cycle && start >= 200 ? 100 : start + 100);
        co_return page;
    }
};
void configure(FileBrowserModel &model, const std::shared_ptr<Synthetic> &provider) {
    model.setProviderFactory([provider](auto, BrowserScope scope) -> Result<std::shared_ptr<BrowseProvider>> {
        if (scope != provider->scope()) return Error{ErrorCode::PermissionDenied};
        return std::static_pointer_cast<BrowseProvider>(provider);
    });
    model.openLocal(provider->scope().identity);
}
QString name(FileBrowserModel &model, int row) { return model.data(model.index(row), FileBrowserModel::Name).toString(); }
}
class BrowserPagingTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        if (qEnvironmentVariableIsSet("DS_BROWSER_TRACE"))
            logfault::LogManager::Instance().AddHandler(std::make_unique<logfault::StreamHandler>(std::clog, logfault::LogLevel::TRACE));
        QVERIFY(settings_.isValid()); QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        qmlRegisterType<FileBrowserModel>("org.darkspeak", 1, 0, "FileBrowserModel");
    }
    void groupedPagesViewportAndPersistentIndexes() {
        FileBrowserModel model; auto provider = std::make_shared<Synthetic>(600); provider->delay = 3;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        configure(model, provider); QTRY_VERIFY(!model.busy()); QCOMPARE(model.rowCount(), 100);
        QPersistentModelIndex first(model.index(0)); const auto key = first.data(FileBrowserModel::Key);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset), changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
        model.setViewport(0, 10, 0, 20, 0); QTest::qWait(60); QCOMPARE(provider->calls, 1);
        model.setViewport(90, 99, 95, 20, 1); QTRY_COMPARE(model.rowCount(), 200);
        QTest::qWait(60); QCOMPARE(provider->calls, 2); // One forward page, no eager drain.
        QCOMPARE(first.data(FileBrowserModel::Key), key); QCOMPARE(first.row(), 0);
        model.setViewport(190, 199, -1, 1000000, 1); QTRY_COMPARE(model.rowCount(), 300);
        QTest::qWait(60); QCOMPARE(provider->calls, 3); // Overscan is clamped.
        QCOMPARE(inserts.size(), 2); QCOMPARE(resets.size(), 0); QCOMPARE(changed.size(), 0);
        for (const auto &args : inserts) QCOMPARE(args[2].toInt() - args[1].toInt() + 1, 100);
        QCOMPARE(name(model, 0), "file000000"); QCOMPARE(name(model, 299), "file000299");
        auto callback = provider->callback;
        for (int i = 0; i < 50; ++i) callback.callback({{}, {}, false});
        QTRY_COMPARE(provider->calls, 4); QTRY_VERIFY(!model.busy()); QTest::qWait(140);
        QCOMPARE(provider->calls, 4); QCOMPARE(model.rowCount(), 100); // One coalesced invalidation refresh.
        model.stop(); callback.callback({{}, {}, true}); QTest::qWait(140);
        QCOMPARE(provider->calls, 4); QCOMPARE(model.rowCount(), 0); QVERIFY(!first.isValid());
    }
    void hostilePagesAndMemoryBudget() {
        for (int scenario = 0; scenario < 5; ++scenario) {
            FileBrowserModel model; auto provider = std::make_shared<Synthetic>(scenario >= 3 ? 120100 : 1000);
            provider->duplicate = scenario == 0; provider->cycle = scenario == 1; provider->empty = scenario == 2;
            configure(model, provider); QTRY_VERIFY(!model.busy());
            if (scenario == 2) {
                model.setViewport(0, 0, -1, 100, 0); QTest::qWait(200);
                QVERIFY(provider->calls <= 4); QVERIFY(!model.busy()); QVERIFY(model.more());
            } else {
                if (scenario == 3) provider->large = true; // Hit byte budget before count budget.
                // Async driver yields at every page. No nested wait per page.
                QTimer driver; driver.setInterval(0);
                connect(&driver, &QTimer::timeout, &model, [&] { if (!model.busy() && model.more()) model.nextPage(); });
                driver.start(); QTRY_COMPARE_WITH_TIMEOUT(model.state(), FileBrowserModel::Unavailable, 15000);
                driver.stop(); QCOMPARE(model.rowCount(), 0); QVERIFY(!model.error().isEmpty());
                if (scenario == 3) QVERIFY(provider->calls < 100); // Conservative 96 MiB admission.
                if (scenario == 4) QCOMPARE(provider->calls, 1201); // Count ceiling, one bounded refused page.
            }
        }
    }
    void remoteSlowPagesAndWithdrawal() {
        Fixture f(205);
        FileBrowserModel model; model.setProviderFactory(f.factory());
        model.openRemote(f.identity, f.contact); QTRY_VERIFY(!model.busy());
        model.openEntry(model.data(model.index(0), FileBrowserModel::Key).toString()); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.rowCount(), 100); const auto key = model.data(model.index(0), FileBrowserModel::Key);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset); f.delay = 30;
        model.setViewport(90, 99, 90, 20, 1); QTRY_COMPARE(model.rowCount(), 200);
        QCOMPARE(model.data(model.index(0), FileBrowserModel::Key), key); QCOMPARE(resets.size(), 0);
        model.nextPage(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.rowCount(), 208); QVERIFY(!model.more());
        f.allowed = false; f.session = f.service->bind({f.identity, f.contact, f.connection, [] { return true; }, [&f] { return f.live; }, [&f] { return f.utf8; }});
        model.refresh(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.state(), FileBrowserModel::Denied); QCOMPARE(model.rowCount(), 0);
    }
    void localVfsLiveInvalidation() {
        Fixture f(0, 100); FileBrowserModel model; model.setProviderFactory(f.factory());
        model.openLocal(f.identity); QTRY_VERIFY(!model.busy());
        model.openEntry(model.data(model.index(0), FileBrowserModel::Key).toString()); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.rowCount(), 3);
        QFile file(f.temp.filePath("source/selected/new-file")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("data"); file.close();
        QTRY_VERIFY_WITH_TIMEOUT(model.rowCount() == 4 || model.state() == FileBrowserModel::Unavailable || model.state() == FileBrowserModel::Denied, 10000);
        QVERIFY2(model.rowCount() == 4, qPrintable(QString("state=%1 busy=%2 error=%3").arg(model.state()).arg(model.busy()).arg(model.error()))); QVERIFY(model.error().isEmpty());
        QVERIFY(QFile::remove(file.fileName())); QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 3, 10000);
        f.exists = false; QTRY_COMPARE(model.rowCount(), 0); QCOMPARE(model.state(), FileBrowserModel::Unavailable);
    }
    void synthetic100kQmlAndHeartbeat() {
        FileBrowserModel model; auto provider = std::make_shared<Synthetic>(100000);
        Appearance appearance; QQuickWindow window; window.resize(1000, 700); QQmlEngine engine;
        engine.rootContext()->setContextProperty("appearance", &appearance);
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/FileBrowser.qml")));
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{"browser", QVariant::fromValue(&model)}}));
        QVERIFY2(object, qPrintable(component.errorString())); auto *root = qobject_cast<QQuickItem*>(object.get()); QVERIFY(root);
        root->setParentItem(window.contentItem()); root->setWidth(1000); root->setHeight(700); window.show();
        configure(model, provider); QTRY_COMPARE(model.rowCount(), 100);
        auto *view = root->findChild<QQuickItem*>("browserEntries"); QVERIFY(view);
        QTest::qWait(80); QCOMPARE(provider->calls, 1);
        QPersistentModelIndex first(model.index(0)); const auto first_key = first.data(FileBrowserModel::Key);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset), changes(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
        int ticks = 0; qint64 longest_gap = 0; QElapsedTimer elapsed; elapsed.start();
        QTimer heartbeat; heartbeat.setInterval(5);
        connect(&heartbeat, &QTimer::timeout, &model, [&] { ++ticks; longest_gap = std::max(longest_gap, elapsed.restart()); }); heartbeat.start();
        QTimer driver; driver.setInterval(0);
        connect(&driver, &QTimer::timeout, &model, [&] { if (!model.busy() && model.more()) model.nextPage(); }); driver.start();
        QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 100000, 20000); driver.stop(); heartbeat.stop();
        QCOMPARE(model.state(), FileBrowserModel::Complete); QCOMPARE(provider->calls, 1000);
        QCOMPARE(inserts.size(), 999); QCOMPARE(resets.size(), 0); QCOMPARE(changes.size(), 0);
        QCOMPARE(first.data(FileBrowserModel::Key), first_key); QVERIFY(ticks > 10); QVERIFY(longest_gap < 1000);
        for (int row = 0; row < model.rowCount(); ++row) QCOMPARE(name(model, row), QString("file%1").arg(row, 6, 10, QChar('0')));
        for (auto mode : {FileBrowserModel::Details, FileBrowserModel::SmallIcons, FileBrowserModel::Thumbnails}) {
            model.setViewMode(mode); QTest::qWait(60);
            for (int position : {0, 50000, 99999}) {
                QVERIFY(QMetaObject::invokeMethod(view, "positionViewAtIndex", Q_ARG(int, position), Q_ARG(int, 0)));
                QTest::qWait(40);
                auto *content = view->property("contentItem").value<QQuickItem*>(); QVERIFY(content);
                int delegates = 0;
                for (auto *item : content->childItems()) if (item->property("entryName").isValid()) ++delegates;
                QVERIFY2(delegates > 0 && delegates < 200, qPrintable(QString::number(delegates)));
            }
        }
        QCOMPARE(warnings.size(), 0);
        qInfo("100k listing: %d heartbeat ticks, max gap %lld ms, 1000 grouped pages, <200 QML delegates", ticks, static_cast<long long>(longest_gap));
    }
};
QTEST_MAIN(BrowserPagingTest)
#include "browser_paging_test.moc"
