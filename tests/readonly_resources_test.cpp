#include "ds/readonlyresources.h"
#include "appearance.h"
#include <dsvfs/vfs.h>
#include <QCoroTask>
#include <QTest>
#include <QSignalSpy>
#include <QAbstractItemModelTester>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <unistd.h>

using namespace dsfm;
using ds::core::ReadOnlyResources;
namespace {
struct Fixture {
    QTemporaryDir temporary;
    ReadOnlyFileManager backend{temporary.filePath("index")};
    dsvfs::Vfs vfs{temporary.filePath("vfs")};
    dsvfs::Request admin;
    QString registry = temporary.filePath("registry.json");
    Fixture() {
        QDir().mkpath(temporary.filePath("source/sub"));
        QDir().mkpath(temporary.filePath("second"));
        QDir().mkpath(temporary.filePath("private"));
        QFile file(temporary.filePath("source/file.txt")); file.open(QIODevice::WriteOnly); file.write("bounded test content");
        admin.scope = QUuid::createUuid();
        admin.generation = 1;
        admin.current_generation = std::make_shared<std::atomic<quint64>>(1);
        admin.authorize = [](dsvfs::Target, dsvfs::Operation) -> Result<bool> { return true; };
        admin.commit_permit = [](dsvfs::Target, dsvfs::Operation) -> Result<std::shared_ptr<void>> { return std::static_pointer_cast<void>(std::make_shared<int>(1)); };
        if (!QCoro::waitFor(vfs.open())) qFatal("test VFS failed to open");
    }
    ~Fixture() { QCoro::waitFor(backend.shutdown()); }
    ReadOnlyResources::Host host() {
        ReadOnlyResources::Host host;
        host.open = [this](ReadOnlyNamespaceConfig config, bool create) -> QCoro::Task<Result<NamespaceInfo>> {
            config.exclusions.directory_roots.append(temporary.filePath("private"));
            auto result = create ? co_await backend.createSource(config) : co_await backend.openSource(config);
            if (result) {
                auto registered = vfs.registerNamespace(config.common.id, std::shared_ptr<IFileManager>(&backend, [](auto*) {}), config.symlinks);
                if (!registered) co_return registered.error();
            }
            co_return result;
        };
        host.close = [this](NamespaceId ns) { return backend.closeSource(ns); };
        host.remove = [this](NamespaceId ns) -> QCoro::Task<Result<void>> {
            auto removed = co_await vfs.unregisterReadOnlyNamespace(ns, admin);
            if (!removed) co_return removed.error();
            co_return co_await backend.removeSource(ns);
        };
        host.usage = [this](NamespaceId ns) { return backend.sourceUsage(ns); };
        host.config = [this](NamespaceId ns) { return backend.sourceConfig(ns); };
        host.info = [this](NamespaceId ns) { return backend.namespaceInfo(ns); };
        return host;
    }
    void draft(ReadOnlyResources &model, QString path = {}) {
        model.setProperty("draftName", "Example source");
        model.setProperty("draftPath", path.isEmpty() ? temporary.filePath("source") : path);
    }
    QUuid id(const ReadOnlyResources &model) { return QUuid(model.data(model.index(0), ReadOnlyResources::ResourceId).toString()); }
};
}
class ReadOnlyResourcesTest : public QObject {
    Q_OBJECT
private slots:
    void pollingOnlyNotifiesChangedFields() {
        Fixture f;
        auto host = f.host();
        ReadOnlySourceUsage usage{1, 21};
        int inspections = 0;
        bool queue_rename = false;
        ReadOnlyResources *current_model = nullptr;
        host.usage = [&](NamespaceId ns) -> QCoro::Task<Result<ReadOnlySourceUsage>> {
            if (queue_rename) {
                queue_rename = false;
                current_model->rename(ns.toString(), "Queued rename");
            }
            co_return usage;
        };
        host.config = [&](NamespaceId ns) -> QCoro::Task<Result<ReadOnlyNamespaceConfig>> {
            auto result = co_await f.backend.sourceConfig(ns);
            ++inspections;
            co_return result;
        };
        ReadOnlyResources model(f.registry, std::move(host));
        current_model = &model;
        QCoro::waitFor(model.load()); f.draft(model);
        QCoro::waitFor(model.create());
        QTRY_VERIFY_WITH_TIMEOUT(QCoro::waitFor(f.backend.namespaceInfo(f.id(model))).value().status == NamespaceStatus::Ready, 10000);
        model.refresh(); QTRY_VERIFY(!model.busy());
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy state(&model, &ReadOnlyResources::stateChanged);
        auto previous = inspections;
        QTRY_VERIFY_WITH_TIMEOUT(inspections > previous, 4000);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(state.count(), 0); // background polling never flashes busy controls
        usage.bytes = 1700000000000ULL;
        previous = inspections;
        QTRY_VERIFY_WITH_TIMEOUT(inspections > previous, 4000);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(qvariant_cast<QList<int>>(changed.takeFirst().at(2)), QList<int>{ReadOnlyResources::Size});
        QCOMPARE(state.count(), 0);
        QCOMPARE(model.data(model.index(0), ReadOnlyResources::Size).toULongLong(), usage.bytes);
        queue_rename = true; // a user action arriving inside a silent poll must run
        QTRY_COMPARE_WITH_TIMEOUT(model.data(model.index(0), ReadOnlyResources::Name).toString(), "Queued rename", 4000);
        QTRY_VERIFY(!model.busy());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(qvariant_cast<QList<int>>(changed.takeFirst().at(2)), QList<int>{ReadOnlyResources::Name});
        QCOMPARE(reset.count(), 0);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(removed.count(), 0);
    }
    void directoryNameUsageAndRename() {
        Fixture f;
        QUuid source;
        {
            ReadOnlyResources model(f.registry, f.host());
            QCoro::waitFor(model.load());
            f.draft(model); model.setProperty("draftName", "   ");
            QCoro::waitFor(model.create());
            QCOMPARE(model.errorCode(), -1);
            QCOMPARE(model.data(model.index(0), ReadOnlyResources::Name).toString(), "source");
            source = f.id(model);
            QTRY_VERIFY_WITH_TIMEOUT(QCoro::waitFor(f.backend.namespaceInfo(source)).value().status == NamespaceStatus::Ready, 10000);
            model.refresh(); QTRY_VERIFY(!model.busy());
            QCOMPARE(model.data(model.index(0), ReadOnlyResources::FileCount).toULongLong(), 1ULL);
            QCOMPARE(model.data(model.index(0), ReadOnlyResources::Size).toULongLong(), quint64(QByteArray("bounded test content").size()));
            QCoro::waitFor(model.renameSource(source, "Renamed source"));
            QCOMPARE(model.errorCode(), -1);
            QCOMPARE(f.id(model), source);
            QCoro::waitFor(model.renameSource(source, QString(129, u'x')));
            QCOMPARE(model.errorCode(), int(ErrorCode::InvalidName));
            QCOMPARE(model.data(model.index(0), ReadOnlyResources::Name).toString(), "Renamed source");
            QVERIFY(QCoro::waitFor(f.backend.closeSource(source)));
        }
        ReadOnlyResources reopened(f.registry, f.host());
        QCoro::waitFor(reopened.load());
        QCOMPARE(f.id(reopened), source);
        QCOMPARE(reopened.data(reopened.index(0), ReadOnlyResources::Name).toString(), "Renamed source");
        QCoro::waitFor(reopened.renameSource(source, ""));
        QCOMPARE(reopened.errorCode(), -1);
        QCOMPARE(reopened.data(reopened.index(0), ReadOnlyResources::Name).toString(), "source");
        QVERIFY(QFile::exists(f.temporary.filePath("source/file.txt")));
    }
    void versionOneRegistryFixture() {
        Fixture f;
        QFile fixture(QStringLiteral(DS_STORAGE_FIXTURES "/readonly-resources-v1.json")); QVERIFY(fixture.open(QIODevice::ReadOnly));
        auto object = QJsonDocument::fromJson(fixture.readAll()).object();
        auto row = object["sources"].toArray().first().toObject();
        ReadOnlyNamespaceConfig config;
        config.common.id = QUuid(row["id"].toString()); config.common.storage_root = f.temporary.filePath("source");
        config.symlinks = SymlinkPolicy(row["symlinks"].toInt()); config.verification = ReadVerification(row["verification"].toInt());
        auto host = f.host(); QVERIFY(QCoro::waitFor(host.open(config, true))); QVERIFY(QCoro::waitFor(f.backend.closeSource(config.common.id)));
        row["path"] = config.common.storage_root; object["sources"] = QJsonArray{row};
        QFile registry(f.registry); QVERIFY(registry.open(QIODevice::WriteOnly)); registry.write(QJsonDocument(object).toJson()); registry.close();
        ReadOnlyResources model(f.registry, f.host()); QCoro::waitFor(model.load());
        QCOMPARE(model.rowCount(), 1); QCOMPARE(f.id(model), config.common.id);
        QCOMPARE(model.data(model.index(0), ReadOnlyResources::Name).toString(), "Fixture source");
        QCOMPARE(model.data(model.index(0), ReadOnlyResources::Verification).toInt(), 1);
        QVERIFY(model.data(model.index(0), ReadOnlyResources::Exclusions).toString().contains(f.temporary.filePath("private")));
    }
    void registerStatusRestartRemove() {
        Fixture f;
        QUuid id;
        {
            ReadOnlyResources model(f.registry, f.host());
            QAbstractItemModelTester notifications(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
            QCoro::waitFor(model.load()); f.draft(model);
            QCoro::waitFor(model.create());
            QCOMPARE(model.errorCode(), -1); QCOMPARE(model.rowCount(), 1);
            id = f.id(model);
            QVERIFY(!id.isNull());
            QVERIFY(f.vfs.resources().value().isEmpty()); // registration grants no remote access
            QTRY_VERIFY_WITH_TIMEOUT(QCoro::waitFor(f.backend.namespaceInfo(id)).value().status == NamespaceStatus::Ready, 10000);
            model.refresh(); QTRY_VERIFY(!model.busy());
            QCOMPARE(model.data(model.index(0), ReadOnlyResources::Status).toString(), "Ready");
            QVERIFY(QCoro::waitFor(f.backend.closeSource(id)));
        }
        ReadOnlyResources reopened(f.registry, f.host());
        QCoro::waitFor(reopened.load());
        QCOMPARE(reopened.rowCount(), 1); QCOMPARE(f.id(reopened), id);
        QVERIFY(QCoro::waitFor(f.backend.namespaceInfo(id)));
        QCoro::waitFor(reopened.erase(id));
        QCOMPARE(reopened.errorCode(), -1);
        QCOMPARE(reopened.rowCount(), 0);
        QVERIFY(QFile::exists(f.temporary.filePath("source/file.txt")));
        QVERIFY(!QFile::exists(f.temporary.filePath("index/" + id.toString(QUuid::WithoutBraces) + "/catalog.db")));
        ReadOnlyResources empty(f.registry, f.host()); QCoro::waitFor(empty.load()); QCOMPARE(empty.rowCount(), 0);
    }
    void rejectOverlapAndAlias() {
        Fixture f; ReadOnlyResources model(f.registry, f.host()); QCoro::waitFor(model.load());
        f.draft(model); QCoro::waitFor(model.create()); QCOMPARE(model.rowCount(), 1);
        for (const auto &path : {f.temporary.filePath("source"), f.temporary.filePath("source/sub"), f.temporary.path()}) {
            f.draft(model, path); QCoro::waitFor(model.create());
            QCOMPARE(model.rowCount(), 1); QCOMPARE(model.errorCode(), int(ErrorCode::Conflict));
        }
        const auto alias = f.temporary.filePath("alias");
        QVERIFY(::symlink(QFile::encodeName(f.temporary.filePath("source")).constData(), QFile::encodeName(alias).constData()) == 0);
        f.draft(model, alias); QCoro::waitFor(model.create()); QCOMPARE(model.errorCode(), int(ErrorCode::Conflict));
    }
    void validateExclusionsAndPolicies() {
        Fixture f; ReadOnlyResources model(f.registry, f.host()); QCoro::waitFor(model.load());
        f.draft(model, f.temporary.filePath("private")); model.setProperty("draftSymlinks", 2);
        QCoro::waitFor(model.create()); QCOMPARE(model.rowCount(), 0); QVERIFY(model.errorCode() >= 0);
        f.draft(model); model.setProperty("draftSymlinks", 9);
        QCoro::waitFor(model.create()); QCOMPARE(model.errorCode(), int(ErrorCode::InvalidArgument));
        model.setProperty("draftSymlinks", 0); model.setProperty("draftExclusions", "/missing-exclusion-for-darkspeak-test");
        QCoro::waitFor(model.create()); QCOMPARE(model.rowCount(), 0); QVERIFY(model.errorCode() >= 0);
        model.setProperty("draftExclusions", f.temporary.filePath("source/sub"));
        QCoro::waitFor(model.create()); QCOMPARE(model.rowCount(), 1);
        auto config = QCoro::waitFor(f.backend.sourceConfig(f.id(model)));
        QVERIFY(config); QVERIFY(config.value().exclusions.directory_roots.contains(f.temporary.filePath("source/sub")));
    }
    void refuseAssignmentDependencyAndUnauthorizedRemoval() {
        Fixture f; ReadOnlyResources model(f.registry, f.host()); QCoro::waitFor(model.load());
        f.draft(model); QCoro::waitFor(model.create());
        const auto ns = f.id(model);
        auto resource = QCoro::waitFor(f.vfs.addResource({{}, ns, {}}, f.admin)); QVERIFY(resource);
        const auto identity = QUuid::createUuid();
        auto info = QCoro::waitFor(f.backend.namespaceInfo(ns)); QVERIFY(info);
        auto assignment = QCoro::waitFor(f.vfs.assignStorage({{}, identity, resource.value().id, info.value().root, false}, f.admin)); QVERIFY(assignment);
        QCoro::waitFor(model.erase(ns)); QCOMPARE(model.rowCount(), 1); QCOMPARE(model.errorCode(), int(ErrorCode::Busy));
        QVERIFY(QCoro::waitFor(f.backend.namespaceInfo(ns)));
        dsvfs::Request denied = f.admin;
        denied.authorize = [](dsvfs::Target, dsvfs::Operation) -> Result<bool> { return false; };
        QVERIFY(!QCoro::waitFor(f.vfs.unregisterReadOnlyNamespace(ns, denied)));
        QVERIFY(QCoro::waitFor(f.vfs.withdrawAssignment(assignment.value().id, f.admin)));
        QCoro::waitFor(model.erase(ns)); QCOMPARE(model.rowCount(), 0); QVERIFY(f.vfs.resources().value().isEmpty());
    }
    void rejectCorruptRegistryAndKeepResponsive() {
        Fixture f;
        QFile registry(f.registry); QVERIFY(registry.open(QIODevice::WriteOnly)); registry.write("{\"version\":999,\"sources\":[]}"); registry.close();
        ReadOnlyResources model(f.registry, f.host()); QCoro::waitFor(model.load()); QVERIFY(model.errorCode() >= 0);
        f.draft(model); QCoro::waitFor(model.create()); QCOMPARE(model.rowCount(), 0);
        QVERIFY(registry.remove()); QCoro::waitFor(model.load());
        bool event_ran = false;
        QTimer::singleShot(0, &model, [&] { event_ran = true; });
        f.draft(model); QCoro::waitFor(model.create()); QVERIFY(event_ran); QCOMPARE(model.rowCount(), 1);
        model.stop(); f.draft(model, f.temporary.filePath("second")); QCoro::waitFor(model.create()); QCOMPARE(model.rowCount(), 1);
    }
    void failedPublicationIsRecoverable() {
        Fixture f; ReadOnlyResources model(f.registry, f.host()); QCoro::waitFor(model.load());
        QVERIFY(QDir().mkdir(f.registry)); // prevents atomic registry publication
        f.draft(model); QCoro::waitFor(model.create());
        QCOMPARE(model.rowCount(), 0); QVERIFY(model.errorCode() >= 0);
        QVERIFY(QCoro::waitFor(f.backend.namespaces()).value().isEmpty());
        QVERIFY(QDir().rmdir(f.registry));
        QCoro::waitFor(model.create()); QCOMPARE(model.rowCount(), 1);
        const auto ns = f.id(model);
        QVERIFY(QFile::remove(f.registry)); QVERIFY(QDir().mkdir(f.registry));
        QCoro::waitFor(model.erase(ns)); QCOMPARE(model.rowCount(), 1); QVERIFY(model.errorCode() >= 0);
        QVERIFY(QFile::exists(f.temporary.filePath("source/file.txt")));
        QVERIFY(QDir().rmdir(f.registry));
        QCoro::waitFor(model.erase(ns)); QCOMPARE(model.rowCount(), 0);
    }
    void externalOverlapClosesBackend() {
        Fixture f; ReadOnlyResources model(f.registry, f.host()); QCoro::waitFor(model.load());
        f.draft(model); QCoro::waitFor(model.create()); const auto first = f.id(model);
        f.draft(model, f.temporary.filePath("second")); QCoro::waitFor(model.create()); QCOMPARE(model.rowCount(), 2);
        QVERIFY(QDir(f.temporary.filePath("second")).removeRecursively());
        QVERIFY(::symlink(QFile::encodeName(f.temporary.filePath("source/sub")).constData(), QFile::encodeName(f.temporary.filePath("second")).constData()) == 0);
        model.refresh(); QTRY_VERIFY(!model.busy());
        QVERIFY(!QCoro::waitFor(f.backend.namespaceInfo(first)));
        QVERIFY(model.data(model.index(0), ReadOnlyResources::Status).toString().contains("Conflict"));
    }
    void qmlDialogUsesProductionModel() {
        Fixture f; ReadOnlyResources model(f.registry, f.host()); QCoro::waitFor(model.load());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, f.temporary.filePath("settings"));
        Appearance appearance;
        QQuickWindow window; window.resize(900, 800);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("appearance", &appearance);
        engine.rootContext()->setContextProperty("readOnlyResources", &model);
        engine.rootContext()->setContextProperty("mainWindow", &window);
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/ReadOnlyStorageDialog.qml")));
        std::unique_ptr<QObject> dialog(component.create());
        QVERIFY2(dialog, qPrintable(component.errorString()));
        QVERIFY(dialog->setProperty("parent", QVariant::fromValue(window.contentItem())));
        QVERIFY(QMetaObject::invokeMethod(dialog.get(), "open"));
        QTRY_VERIFY(!model.busy());
        f.draft(model); model.setProperty("draftSymlinks", 2); QCoro::waitFor(model.create());
        QCOMPARE(model.rowCount(), 1);
        window.show(); QTest::qWait(50);
        QCOMPARE(warnings.count(), 0);
        QVERIFY(QMetaObject::invokeMethod(dialog.get(), "close"));
        QCOMPARE(model.property("draftPath").toString(), QString{});
    }
};
QTEST_MAIN(ReadOnlyResourcesTest)
#include "readonly_resources_test.moc"
