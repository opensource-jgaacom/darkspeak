#include "ds/readonlyresources.h"
#include "ds/sharemounts.h"
#include "ds/sharepermissions.h"
#include "appearance.h"
#include <QTest>
#include <QAbstractItemModelTester>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>

using namespace dsfm;
using namespace dsvfs;
using namespace ds::core;

namespace {
QString text(QUuid id) { return id.toString(QUuid::WithoutBraces); }
QString firstId(const QVariantList &rows) { return rows.first().toMap()["id"].toString(); }

// Delegates belong to the visual tree, not necessarily the QObject tree.
QQuickItem *findText(QQuickItem *item, const QString &value) {
    if (!item) return nullptr;
    if (item->property("text").toString() == value) return item;
    for (auto *child : item->childItems())
        if (auto *found = findText(child, value)) return found;
    return nullptr;
}

struct Fixture {
    QTemporaryDir temp;
    const QUuid identity = QUuid::createUuid(), contact = QUuid::createUuid();
    std::shared_ptr<ReadOnlyFileManager> backend;
    std::unique_ptr<Vfs> vfs;
    std::unique_ptr<ReadOnlyResources> resources;
    std::unique_ptr<ShareMounts> mounts;
    std::unique_ptr<SharePermissions> permissions;

    Fixture() {
        if (!QDir().mkpath(temp.filePath("source/allowed"))) qFatal("source setup");
        for (const auto &path : {"source/allowed/file", "source/hidden"}) {
            QFile file(temp.filePath(path));
            if (!file.open(QIODevice::WriteOnly) || file.write("test") != 4) qFatal("file setup");
        }
        open();
    }
    ~Fixture() { close(); }
    void close() {
        if (!QTest::qWaitFor([this] { return !resources->busy() && !mounts->busy()
                && !permissions->property("busy").toBool(); }, 5000)) qFatal("administration did not settle");
        resources->stop(); mounts->stop(); permissions->stop();
        permissions.reset(); mounts.reset(); resources.reset();
        QCoro::waitFor(vfs->shutdown()); vfs.reset();
        QCoro::waitFor(backend->shutdown()); backend.reset();
    }
    void open() {
        backend = std::make_shared<ReadOnlyFileManager>(temp.filePath("index"));
        vfs = std::make_unique<Vfs>(temp.filePath("vfs"));
        if (!QCoro::waitFor(vfs->open())) qFatal("VFS open");
        ReadOnlyResources::Host host;
        host.open = [this](ReadOnlyNamespaceConfig config, bool create) -> QCoro::Task<Result<NamespaceInfo>> {
            auto result = create ? co_await backend->createSource(config) : co_await backend->openSource(config);
            if (result) {
                auto registered = vfs->registerNamespace(config.common.id, backend, config.symlinks);
                if (!registered) co_return registered.error();
            }
            co_return result;
        };
        host.close = [this](NamespaceId ns) { return backend->closeSource(ns); };
        host.remove = [this](NamespaceId ns) -> QCoro::Task<Result<void>> {
            Request admin;
            admin.scope = identity; admin.generation = 1;
            admin.current_generation = std::make_shared<std::atomic<quint64>>(1);
            admin.authorize = [](Target, Operation op) -> Result<bool> { return op == Operation::ConfigureMounts; };
            admin.commit_permit = [](Target, Operation) -> Result<std::shared_ptr<void>> {
                return std::static_pointer_cast<void>(std::make_shared<int>(0));
            };
            auto removed = co_await vfs->unregisterReadOnlyNamespace(ns, admin);
            if (!removed) co_return removed.error();
            co_return co_await backend->removeSource(ns);
        };
        host.config = [this](NamespaceId ns) { return backend->sourceConfig(ns); };
        host.info = [this](NamespaceId ns) { return backend->namespaceInfo(ns); };
        resources = std::make_unique<ReadOnlyResources>(temp.filePath("registry.json"), host);
        QCoro::waitFor(resources->load());
        mounts = std::make_unique<ShareMounts>(ShareMounts::Host{{},
            [this](QUuid id) { return id == identity; },
            [this]() -> QCoro::Task<Result<Vfs*>> { co_return vfs.get(); },
            [this]() -> IFileManager* { return backend.get(); }});
        permissions = std::make_unique<SharePermissions>(temp.filePath("access"), SharePermissions::Host{{},
            [this](QUuid id) { return id == identity; },
            [this](QUuid id, QUuid c) { return id == identity && c == contact; },
            [this](QUuid, QString filter, int page) -> Result<QVariantList> {
                if (page || !QString("Contact").contains(filter, Qt::CaseInsensitive)) return QVariantList{};
                return QVariantList{QVariantMap{{"id", text(contact)}, {"name", "Contact"}}};
            }, [this]() -> QCoro::Task<Result<Vfs*>> { co_return vfs.get(); }});
    }
};

struct Dialog {
    Appearance appearance;
    QQuickWindow window;
    QQmlEngine engine;
    QSignalSpy warnings{&engine, &QQmlEngine::warnings};
    std::unique_ptr<QObject> root;
    Dialog(QString name, Fixture &f) {
        window.resize(1100,1000);
        engine.rootContext()->setContextProperty("appearance", &appearance);
        engine.rootContext()->setContextProperty("mainWindow", &window);
        engine.rootContext()->setContextProperty("readOnlyResources", f.resources.get());
        engine.rootContext()->setContextProperty("shareMounts", f.mounts.get());
        engine.rootContext()->setContextProperty("sharePermissions", f.permissions.get());
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/") + name));
        root.reset(component.create());
        if (!root) qFatal("QML creation failed: %s", qPrintable(component.errorString()));
        root->setProperty("parent", QVariant::fromValue(window.contentItem()));
        window.show();
        QMetaObject::invokeMethod(root.get(), "open");
        if (!QTest::qWaitFor([this, &f] { return root->property("opened").toBool()
                && !f.resources->busy() && !f.mounts->busy() && !f.permissions->property("busy").toBool(); }, 5000))
            qFatal("dialog did not finish opening/refreshing");
    }
    QQuickItem *find(QString label) {
        return findText(root->property("contentItem").value<QQuickItem*>(), label);
    }
    bool click(QString label) {
        auto *button = find(label);
        if (!button || !button->isEnabled())
            qWarning() << "Control unavailable:" << label << button << "dialog opened:" << root->property("opened");
        return button && button->isEnabled() && QMetaObject::invokeMethod(button, "clicked");
    }
    bool confirm() {
        for (auto *child : root->findChildren<QObject*>())
            if (child->property("title").toString() == "Apply identity access change?")
                return QMetaObject::invokeMethod(child, "accept");
        return false;
    }
};
}

class ShareAdministrationTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("DarkSpeakTests");
        QCoreApplication::setApplicationName("ShareAdministration");
    }
    void registerAssignMountGrantRestartAndWithdraw() {
        Fixture f;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, f.temp.filePath("settings"));
        {
            QAbstractItemModelTester notifications(f.resources.get(), QAbstractItemModelTester::FailureReportingMode::QtTest);
            Dialog ui("ReadOnlyStorageDialog.qml", f);
            QTRY_VERIFY(ui.root->property("opened").toBool());
            QVERIFY(ui.click("Register")); // Invalid draft must have an actionable, bound error.
            QTRY_VERIFY(!f.resources->busy());
            QVERIFY(!f.resources->error().isEmpty());
            QVERIFY(ui.find(f.resources->error()));
            f.resources->setProperty("draftName", "Source");
            f.resources->setProperty("draftPath", f.temp.filePath("source"));
            QVERIFY(ui.click("Register"));
            QTRY_VERIFY(!f.resources->busy());
            QCOMPARE(f.resources->errorCode(), -1);
            QCOMPARE(f.resources->rowCount(), 1);
            QCOMPARE(ui.warnings.size(), 0);
        }
        const QUuid ns(f.resources->data(f.resources->index(0), ReadOnlyResources::ResourceId).toString());
        QTRY_VERIFY_WITH_TIMEOUT(QCoro::waitFor(f.backend->namespaceInfo(ns)).value().status == NamespaceStatus::Ready, 10000);
        QVERIFY(f.vfs->resources().value().isEmpty());
        f.mounts->selectIdentity(f.identity); f.permissions->selectIdentity(f.identity);
        QTRY_VERIFY(!f.mounts->busy() && !f.permissions->property("busy").toBool());
        f.mounts->selectSource(text(ns)); QTRY_VERIFY(!f.mounts->busy());
        QCOMPARE(f.mounts->directories().size(), 1);
        f.mounts->enterDirectory(firstId(f.mounts->directories())); QTRY_VERIFY(!f.mounts->busy());
        {
            Dialog ui("ShareMountDialog.qml", f);
            QVERIFY(ui.click("Assign selected directory")); QTRY_VERIFY(!f.mounts->busy());
            QVERIFY(f.mounts->error().isEmpty()); QCOMPARE(f.mounts->assignments().size(), 1);
            QCoro::waitFor(f.permissions->inspect(f.contact, "/"));
            QVERIFY(f.permissions->property("preview").toList().isEmpty());
            f.mounts->setProperty("draftPath", "/../escape");
            QTRY_VERIFY(ui.find("Mount at path"));
            QVERIFY(ui.click("Mount at path")); QTRY_VERIFY(!f.mounts->busy());
            QVERIFY(f.mounts->error().contains("absolute virtual path")); QVERIFY(ui.find(f.mounts->error()));
            QVERIFY(f.mounts->mounts().isEmpty());
            f.mounts->setProperty("draftPath", "/share");
            QVERIFY(ui.click("Mount at path")); QTRY_VERIFY(!f.mounts->busy());
            QVERIFY(f.mounts->error().isEmpty()); QCOMPARE(f.mounts->mounts().size(), 1);
            QCOMPARE(ui.warnings.size(), 0);
        }
        const auto assignment = firstId(f.mounts->assignments()), mount = firstId(f.mounts->mounts());
        QCoro::waitFor(f.permissions->inspect(f.contact, "/"));
        QVERIFY(f.permissions->property("preview").toList().isEmpty()); // Mounting is not a grant.
        {
            Dialog ui("SharePermissionsDialog.qml", f);
            auto *view = ui.find("View as selected contact"); QVERIFY(view); QVERIFY(!view->isEnabled());
            f.permissions->contactPage("", 0); QTRY_VERIFY(!f.permissions->property("busy").toBool());
            const auto contact_label = "Contact (" + text(f.contact) + ")";
            QTRY_VERIFY(ui.find(contact_label)); QVERIFY(ui.click(contact_label));
            QVERIFY(view->isEnabled());
            QVERIFY(QMetaObject::invokeMethod(ui.root.get(), "editRule", Q_ARG(QVariant, QVariantMap({
                {"id", ""}, {"tier", 2}, {"principal", text(f.contact)}, {"path", "/"},
                {"permissions", QVariantList{1,1,1,1,1,1}}, {"denied", 0}}))));
            QVERIFY(ui.click("Save rule…")); QVERIFY(f.permissions->rules().isEmpty()); // Confirmation is required.
            QVERIFY(ui.confirm()); QTRY_VERIFY(!f.permissions->property("busy").toBool());
            QVERIFY(f.permissions->property("error").toString().isEmpty()); QCOMPARE(f.permissions->rules().size(), 1);
            QVERIFY(ui.click("View as selected contact")); QTRY_VERIFY(!f.permissions->property("busy").toBool());
            QCOMPARE(f.permissions->property("preview").toList().size(), 1);
            QCOMPARE(ui.warnings.size(), 0);
        }
        const auto rule_id = firstId(f.permissions->rules());
        // Recreate all models, VFS and backend; no process-local catalog or policy survives.
        f.close(); f.open();
        QCOMPARE(f.resources->rowCount(), 1);
        QCOMPARE(f.resources->data(f.resources->index(0), ReadOnlyResources::ResourceId).toString(), text(ns));
        f.mounts->selectIdentity(f.identity); f.permissions->selectIdentity(f.identity);
        QTRY_VERIFY(!f.mounts->busy() && !f.permissions->property("busy").toBool());
        QCOMPARE(firstId(f.mounts->assignments()), assignment); QCOMPARE(firstId(f.mounts->mounts()), mount);
        QCOMPARE(firstId(f.permissions->rules()), rule_id);
        auto request = f.permissions->requestFor(f.identity, f.contact); QVERIFY(request);
        auto composed = QCoro::waitFor(f.vfs->compose(f.identity, request.value())); QVERIFY(composed);
        auto share = QCoro::waitFor(f.vfs->lookup(f.identity, f.vfs->root(f.identity), "share", composed.value())); QVERIFY(share);
        auto page = QCoro::waitFor(f.vfs->list(f.identity, share.value().id, {}, composed.value())); QVERIFY(page);
        QCOMPARE(page.value().entries.size(), 1); QCOMPARE(page.value().entries.first().name, "file");
        QVERIFY(!QCoro::waitFor(f.vfs->lookup(f.identity, share.value().id, "hidden", composed.value())));
        auto file = page.value().entries.first();
        auto prepared = QCoro::waitFor(f.vfs->prepareRead(f.identity, file.id, {}, composed.value())); QVERIFY(prepared);
        QCOMPARE(prepared.value().info.sha256, QCryptographicHash::hash("test", QCryptographicHash::Sha256));
        auto bytes = QCoro::waitFor(prepared.value().reader->readAt(0, 2)); QVERIFY(bytes); QCOMPARE(bytes.value(), "te");
        {
            Dialog ui("SharePermissionsDialog.qml", f);
            QVERIFY(QMetaObject::invokeMethod(ui.root.get(), "editRule", Q_ARG(QVariant, QVariantMap({
                {"id", rule_id}, {"tier", 2}, {"principal", text(f.contact)}, {"path", "/"},
                {"permissions", QVariantList{2,2,2,2,2,2}}, {"denied", 0}}))));
            QVERIFY(ui.click("Save rule…")); QVERIFY(ui.confirm());
            QTRY_VERIFY(!f.permissions->property("busy").toBool());
            QVERIFY(f.permissions->property("error").toString().isEmpty());
            QVERIFY(!composed.value().current());
            QVERIFY(!QCoro::waitFor(prepared.value().reader->readAt(2, 2)));
            QCOMPARE(ui.warnings.size(), 0);
        }
        prepared.value().reader.reset();
        f.close(); f.open();
        QVERIFY(QCoro::waitFor(f.permissions->load(f.identity)));
        request = f.permissions->requestFor(f.identity, f.contact); QVERIFY(request);
        composed = QCoro::waitFor(f.vfs->compose(f.identity, request.value())); QVERIFY(composed);
        QVERIFY(!QCoro::waitFor(f.vfs->prepareRead(f.identity, file.id, {}, composed.value())));
        QCoro::waitFor(f.permissions->inspect(f.contact, "/"));
        QVERIFY(f.permissions->property("preview").toList().isEmpty());
        QVERIFY(QFile::exists(f.temp.filePath("source/allowed/file")));
        QVERIFY(QFile::exists(f.temp.filePath("source/hidden")));
    }
};
QTEST_MAIN(ShareAdministrationTest)
#include "share_administration_test.moc"
