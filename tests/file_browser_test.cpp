#include <ds/filebrowsermodel.h>
#include <ds/protocolcodec.h>
#include <dsfilemanager/readonlyfilemanager.h>
#include "appearance.h"
#include <QAbstractItemModelTester>
#include <QFile>
#include <QTemporaryDir>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTest>

using namespace ds::models;
using namespace dsfm;
using namespace dsvfs;
using namespace ds::prot;
using namespace ds::protocol;
#include "file_browser_fixture.h"
class FileBrowserTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        qmlRegisterType<FileBrowserModel>("org.darkspeak", 1, 0, "FileBrowserModel");
    }
    void localProviderUsesAssignedVfsOnly() {
        Fixture f; auto provider = std::make_shared<LocalBrowseProvider>(f.identity, f.localHost());
        auto root = get(provider->root()); auto first = get(provider->listPage(root));
        QCOMPARE(first.entries.size(), 1); QCOMPARE(first.entries.first().info.name, "share");
        auto share = get(provider->resolveChild(root, "share")); auto page = get(provider->listPage(share));
        QCOMPARE(page.entries.size(), 3); QVERIFY(page.continuation.isEmpty());
        QVERIFY(!QCoro::waitFor(provider->resolveChild(root, "private")));
        auto file = get(provider->resolveChild(share, "a"));
        QCOMPARE(get(provider->stat(file)).info.size, quint64(4));
        QVERIFY(!file.info.capabilities.testFlag(Capability::Write));
        auto other = std::make_shared<LocalBrowseProvider>(f.identity, f.localHost());
        QVERIFY(!QCoro::waitFor(other->stat(file)));
        f.exists = false; QVERIFY(!QCoro::waitFor(provider->listPage(share)));
        provider->cancel(); QVERIFY(!provider->current());
    }
    void remoteProviderRoundTrip_data() {
        QTest::addColumn<bool>("utf8"); QTest::newRow("utf8") << true; QTest::newRow("ascii") << false;
    }
    void remoteProviderRoundTrip() {
        QFETCH(bool, utf8); Fixture f; f.utf8 = utf8;
        auto provider = std::make_shared<RemoteBrowseProvider>(f.remoteContext());
        auto root = get(provider->root()); auto first = get(provider->listPage(root));
        QCOMPARE(first.entries.size(), 1); QCOMPARE(first.entries.first().info.name, "share");
        auto share = get(provider->resolveChild(root, first.entries.first().info.name));
        auto page = get(provider->listPage(share)); QCOMPARE(page.entries.size(), 3);
        const QString name = utf8 ? QString::fromUtf8("résumé%25FF") : QString("r%C3%A9sum%C3%A9%2525FF");
        auto file = get(provider->resolveChild(share, name));
        QCOMPARE(file.info.name, name); QCOMPARE(file.path, "/share/" + name);
        QVERIFY(file.size_known); QCOMPARE(file.info.size, quint64(4)); QVERIFY(file.info.capabilities == dsfm::Capabilities{});
        QCOMPARE(get(provider->stat(file)).info.id, file.info.id);
        QVERIFY(!QCoro::waitFor(provider->resolveChild(share, "../private")));
        auto local = std::make_shared<LocalBrowseProvider>(f.identity, f.localHost());
        QVERIFY(!QCoro::waitFor(local->stat(file))); // No foreign ID ever reaches VFS.
        auto foreign = file; foreign.provider = QUuid::createUuid();
        QVERIFY(!QCoro::waitFor(provider->stat(foreign)));
        f.allowed = false;
        // Keep connection availability separate from the server's authorization denial.
        f.session = f.service->bind({f.identity, f.contact, f.connection, [] { return true; }, [live = &f.live] { return *live; }, [utf8] { return utf8; }});
        QVERIFY(!QCoro::waitFor(provider->listPage(share)));
        f.live = false; QVERIFY(!QCoro::waitFor(provider->stat(file)));
    }
    void singleModelScopesAndRejectsLateReplies() {
        Fixture f; FileBrowserModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setProviderFactory(f.factory()); model.openLocal(f.identity);
        QTRY_VERIFY(!model.busy()); QVERIFY(model.ready()); QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.location(), FileBrowserModel::Local); QVERIFY(model.contactId().isNull());
        const auto local_key = model.data(model.index(0), FileBrowserModel::Key);
        model.openRemote(f.identity, f.contact); QTRY_VERIFY(!model.busy()); QVERIFY(model.ready());
        QCOMPARE(model.location(), FileBrowserModel::Remote); QCOMPARE(model.contactId(), f.contact);
        QVERIFY(local_key != model.data(model.index(0), FileBrowserModel::Key));
        f.delay = 40; model.openRemote(f.identity, f.contact); QVERIFY(model.busy());
        model.openLocal(f.identity); QTRY_VERIFY(!model.busy()); QVERIFY(model.ready());
        QTest::qWait(70); QCOMPARE(model.location(), FileBrowserModel::Local); QCOMPARE(model.rowCount(), 1);
        QVERIFY(f.cancels > 0);
        model.openRemote(f.identity, QUuid::createUuid());
        QVERIFY(!model.ready()); QVERIFY(!model.error().isEmpty()); QCOMPARE(model.rowCount(), 0);
        model.openRemote(QUuid::createUuid(), f.contact); QVERIFY(!model.ready()); QCOMPARE(model.rowCount(), 0);
        model.openRemote(f.identity, f.contact); QTRY_VERIFY(!model.busy()); QVERIFY(model.ready());
        f.live = false; QTRY_VERIFY(!model.ready()); QCOMPARE(model.rowCount(), 0); QVERIFY(!model.error().isEmpty());
    }
    void malformedResponsesAndSendFailures() {
        Fixture f; f.corrupt = true; FileBrowserModel model; model.setProviderFactory(f.factory());
        model.openRemote(f.identity, f.contact); QTRY_VERIFY(!model.busy());
        QVERIFY(!model.ready()); QCOMPARE(model.rowCount(), 0); QVERIFY(!model.error().isEmpty());
        f.corrupt = false; f.wrong_connection = true;
        model.openRemote(f.identity, f.contact); QTRY_VERIFY(!model.busy()); QVERIFY(model.ready());
        auto context = f.remoteContext(); context.send = [](Envelope) -> quint64 { throw std::runtime_error("test admission rejection"); };
        RemoteBrowseProvider provider(context); auto root = get(provider.root());
        QVERIFY(!QCoro::waitFor(provider.listPage(root)));
        auto slow = std::make_shared<RemoteBrowseProvider>(f.remoteContext()); f.delay = 40;
        bool completed = false;
        auto slow_root = get(slow->root());
        slow->listPage(slow_root).then([&](Result<BrowserPage> result) { QVERIFY(!result); completed = true; });
        slow->cancel(); QTRY_VERIFY(completed);
        // Cancelling/destroying a view while waiting never publishes into a new view.
        auto view = std::make_unique<FileBrowserModel>(); view->setProviderFactory(f.factory());
        view->openRemote(f.identity, f.contact); view.reset(); QTest::qWait(60);
    }
    void boundedPagesAndEmptyContinuation() {
        class Pages final : public BrowseProvider {
        public:
            explicit Pages(BrowserScope scope) : BrowseProvider(scope) {}
            bool live = true, oversized = false;
            int calls = 0;
            bool current() const override { return live; }
            void cancel() override { live = false; }
            QCoro::Task<Result<BrowserNode>> root() override {
                BrowserNode root; root.provider = id_; root.info.id = "root"; root.path = "/"; co_return root;
            }
            QCoro::Task<Result<BrowserNode>> stat(BrowserNode node) override { co_return node; }
            QCoro::Task<Result<BrowserNode>> resolveChild(BrowserNode, QString) override { co_return Error{ErrorCode::UnsupportedCapability}; }
            QCoro::Task<Result<BrowserPage>> listPage(BrowserNode directory, QByteArray cursor) override {
                if (!owns(directory)) co_return Error{ErrorCode::InvalidArgument};
                ++calls; BrowserPage page;
                if (cursor == "next") { page.continuation = "last"; co_return page; }
                int count = cursor == "last" ? 1 : (oversized ? 101 : 100);
                for (int i = 0; i < count; ++i) {
                    BrowserNode node; node.provider = id_; node.info.id = QByteArray::number(cursor == "last" ? 100 : i);
                    node.info.name = QString::number(cursor == "last" ? 100 : i); page.entries.append(node);
                }
                if (cursor.isEmpty()) page.continuation = "next";
                co_return page;
            }
        };
        BrowserScope scope{QUuid::createUuid(), {}};
        auto provider = std::make_shared<Pages>(scope);
        FileBrowserModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setProviderFactory([provider](auto, auto) -> Result<std::shared_ptr<BrowseProvider>> { return std::static_pointer_cast<BrowseProvider>(provider); });
        model.openLocal(scope.identity); QTRY_VERIFY(!model.busy()); QVERIFY(model.ready());
        QCOMPARE(model.rowCount(), 100); QVERIFY(model.more()); QCOMPARE(provider->calls, 1);
        model.nextPage(); QTRY_VERIFY(!model.busy()); QVERIFY(model.ready());
        QCOMPARE(model.rowCount(), 100); QVERIFY(model.more()); QCOMPARE(provider->calls, 2);
        model.nextPage(); QTRY_VERIFY(!model.busy()); QCOMPARE(model.rowCount(), 101); QVERIFY(!model.more());
        model.nextPage(); QCOMPARE(provider->calls, 3); // No eager prefetch or repeated complete-page work.
        auto oversized = std::make_shared<Pages>(scope); oversized->oversized = true;
        model.setProviderFactory([oversized](auto, auto) -> Result<std::shared_ptr<BrowseProvider>> { return std::static_pointer_cast<BrowseProvider>(oversized); });
        model.openLocal(scope.identity); QTRY_VERIFY(!model.busy());
        QVERIFY(!model.ready()); QCOMPARE(model.rowCount(), 0); QVERIFY(!model.error().isEmpty());
    }
    void sharedQmlView() {
        Fixture f; Appearance appearance;
        QQuickWindow window; window.resize(1000, 700);
        FileBrowserSources sources(f.factory()); QQmlEngine engine;
        engine.rootContext()->setContextProperty("appearance", &appearance);
        engine.rootContext()->setContextProperty("mainWindow", &window);
        engine.rootContext()->setContextProperty("fileBrowsers", &sources);
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/FileBrowserDialog.qml")));
        for (bool remote : {false, true}) {
            std::unique_ptr<QObject> dialog(component.createWithInitialProperties({{"identityId", f.identity},
                {"contactId", f.contact}, {"remote", remote}}));
            QVERIFY2(dialog, qPrintable(component.errorString()));
            dialog->setProperty("parent", QVariant::fromValue(window.contentItem()));
            QVERIFY(QMetaObject::invokeMethod(dialog.get(), "open"));
            QTest::qWait(40);
            auto *model = dialog->findChild<FileBrowserModel*>(); QVERIFY(model);
            QTRY_VERIFY(!model->busy()); QVERIFY2(model->ready(), qPrintable(model->error()));
            QCOMPARE(model->location(), remote ? FileBrowserModel::Remote : FileBrowserModel::Local);
            QCOMPARE(model->rowCount(), 1);
            auto *list = dialog->findChild<QQuickItem*>("browserEntries"); QVERIFY(list);
            QTRY_COMPARE(list->property("count").toInt(), 1);
        }
        sources.stop(); QCOMPARE(warnings.size(), 0);
    }
};
QTEST_MAIN(FileBrowserTest)
#include "file_browser_test.moc"
