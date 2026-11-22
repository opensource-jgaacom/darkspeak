#include "ds/sharemounts.h"
#include "appearance.h"
#include <dsfilemanager/readonlyfilemanager.h>
#include <QCoroTask>
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
using namespace dsfm;
using namespace dsvfs;
using ds::core::ShareMounts;
namespace {
struct Fixture {
    QTemporaryDir temp;
    std::shared_ptr<ReadOnlyFileManager> backend = std::make_shared<ReadOnlyFileManager>(temp.filePath("index"));
    std::unique_ptr<Vfs> vfs;
    NamespaceId ns = QUuid::createUuid();
    QUuid identity = QUuid::createUuid();
    Fixture() {
        QDir().mkpath(temp.filePath("source/sub/child"));
        QFile file(temp.filePath("source/sub/file")); file.open(QIODevice::WriteOnly); file.write("test"); file.close();
        ReadOnlyNamespaceConfig config; config.common.id = ns; config.common.storage_root = temp.filePath("source");
        if (!QCoro::waitFor(backend->createSource(config))) qFatal("source setup");
        if (!QCoro::waitFor(backend->reconcile(ns, {}))) qFatal("index setup");
        reopen();
    }
    void reopen() {
        if (vfs) { QCoro::waitFor(vfs->shutdown()); vfs.reset(); }
        vfs = std::make_unique<Vfs>(temp.filePath("vfs"));
        if (!QCoro::waitFor(vfs->open()) || !vfs->registerNamespace(ns, backend)) qFatal("VFS setup");
    }
    ~Fixture() { QCoro::waitFor(vfs->shutdown()); QCoro::waitFor(backend->shutdown()); }
    ShareMounts::Host host() {
        return {{}, [this](QUuid id) { return id == identity; },
            [this]() -> QCoro::Task<Result<Vfs*>> { co_return vfs.get(); }, [this]() -> IFileManager* { return backend.get(); }};
    }
    void context(ShareMounts &model) { model.selectIdentity(identity); QTest::qWait(1); }
};
QVariantList rows(ShareMounts &m, const char *key) { return m.property(key).toList(); }
QString id(QVariantList rows) { return rows.first().toMap()["id"].toString(); }
}
class ShareMountsTest : public QObject {
    Q_OBJECT
private slots:
    void assignmentMountRestartConfinement() {
        Fixture f; ShareMounts model(f.host()); f.context(model);
        QTRY_VERIFY(!model.property("busy").toBool());
        QVERIFY(model.property("error").toString().isEmpty());
        QCOMPARE(rows(model,"sources").size(),1);
        QCoro::waitFor(model.run(ShareMounts::SelectSource, f.ns.toString()));
        QCOMPARE(rows(model,"directories").size(),1);
        QCoro::waitFor(model.run(ShareMounts::EnterDirectory, id(rows(model,"directories"))));
        QCoro::waitFor(model.run(ShareMounts::Assign));
        QCOMPARE(rows(model,"assignments").size(),1);
        QVERIFY(f.vfs->definitions(f.identity).value().isEmpty());
        QVERIFY(rows(model,"preview").isEmpty());
        const auto assignment = id(rows(model,"assignments"));
        model.setProperty("draftPath", "/library/books");
        QCoro::waitFor(model.run(ShareMounts::AddMount, assignment));
        QVERIFY2(model.property("error").toString().isEmpty(), qPrintable(model.property("error").toString()));
        QCOMPARE(rows(model,"mounts").size(),1);
        QCOMPARE(rows(model,"preview").first().toMap()["name"].toString(),"library");
        const auto mount = id(rows(model,"mounts"));
        QCoro::waitFor(model.run(ShareMounts::Unassign,assignment)); // No silent cascade.
        QVERIFY(!model.property("error").toString().isEmpty());
        QCOMPARE(f.vfs->assignments(f.identity).value().size(),1);
        model.setProperty("draftPath", "/books"); QCoro::waitFor(model.run(ShareMounts::MoveMount,mount));
        QVERIFY(model.property("error").toString().isEmpty());
        model.setProperty("draftPath", "/books/child");
        QCoro::waitFor(model.run(ShareMounts::AddMount,assignment));
        QVERIFY(!model.property("error").toString().isEmpty()); // source directory collision
        QCOMPARE(rows(model,"mounts").size(),1);
        f.reopen(); QCoro::waitFor(model.run(ShareMounts::Refresh));
        QCOMPARE(id(rows(model,"mounts")),mount);
        QCOMPARE(rows(model,"mounts").first().toMap()["path"].toString(),"/books");
        QCoro::waitFor(model.run(ShareMounts::Browse,id(rows(model,"preview")))) ;
        QCOMPARE(rows(model,"preview").size(),2); // subtree contents, never source siblings/ancestors
        auto assigned = f.vfs->assignments(f.identity).value().first();
        QVERIFY(!assigned.allow_external_files);
        Request admin;
        admin.scope = f.identity; admin.generation = 1;
        admin.current_generation = std::make_shared<std::atomic<quint64>>(1);
        admin.authorize = [](Target, Operation op) -> Result<bool> { return op == Operation::ConfigureMounts; };
        admin.commit_permit = [](Target, Operation) -> Result<std::shared_ptr<void>> { return std::static_pointer_cast<void>(std::make_shared<int>(0)); };
        auto source = QCoro::waitFor(f.backend->namespaceInfo(f.ns)); QVERIFY(source);
        auto escape = QCoro::waitFor(f.vfs->defineMount({{},f.identity,assigned.id,source.value().root,"/escape",Tier::Identity,{}},admin));
        QVERIFY(!escape); QCOMPARE(escape.error().code, ErrorCode::PermissionDenied);

        Request remote; remote.scope = QUuid::createUuid();
        auto denied = QCoro::waitFor(f.vfs->list(f.identity,f.vfs->root(f.identity),{},remote));
        QVERIFY(!denied || denied.value().entries.isEmpty());
        QCoro::waitFor(model.run(ShareMounts::RemoveMount,mount)); QCoro::waitFor(model.run(ShareMounts::Unassign,assignment));
        QVERIFY(rows(model,"mounts").isEmpty()); QVERIFY(rows(model,"assignments").isEmpty());
        QVERIFY(QFile::exists(f.temp.filePath("source/sub/file")));
    }
    void rejectsInvalidIdentityIdsAndPaths() {
        Fixture f; ShareMounts model(f.host()); f.context(model); QTRY_VERIFY(!model.property("busy").toBool());
        QCoro::waitFor(model.run(ShareMounts::AddMount,QUuid::createUuid().toString()));
        QVERIFY(!model.property("error").toString().isEmpty());
        QCoro::waitFor(model.run(ShareMounts::SelectSource,f.ns.toString())); QCoro::waitFor(model.run(ShareMounts::Assign));
        const auto assignment = id(rows(model,"assignments"));
        for (const auto &path : {QString("../escape"),QString("/../escape"),QString("/")}) {
            model.setProperty("draftPath",path); QCoro::waitFor(model.run(ShareMounts::AddMount,assignment));
            QVERIFY(!model.property("error").toString().isEmpty()); QVERIFY(rows(model,"mounts").isEmpty());
        }
        model.selectIdentity(QUuid::createUuid()); QTRY_VERIFY(!model.property("busy").toBool());
        QVERIFY(!model.property("error").toString().isEmpty()); QVERIFY(rows(model,"assignments").isEmpty());
    }
    void qmlDialog() {
        Fixture f; ShareMounts model(f.host()); f.context(model); QTRY_VERIFY(!model.property("busy").toBool());
        QCoro::waitFor(model.run(ShareMounts::SelectSource,f.ns.toString()));
        QCoro::waitFor(model.run(ShareMounts::Assign));
        QCoro::waitFor(model.run(ShareMounts::AddMount,id(rows(model,"assignments"))));
        QVERIFY(model.property("error").toString().isEmpty());
        Appearance appearance; QQuickWindow window; window.resize(1000,900);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,f.temp.filePath("settings"));
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.rootContext()->setContextProperty("appearance", &appearance);
        engine.rootContext()->setContextProperty("mainWindow", &window);
        engine.rootContext()->setContextProperty("shareMounts", &model);
        QQmlComponent component(&engine,QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/ShareMountDialog.qml")));
        std::unique_ptr<QObject> dialog(component.create());
        QVERIFY2(dialog,qPrintable(component.errorString()));
        dialog->setProperty("parent",QVariant::fromValue(window.contentItem()));
        QVERIFY(QMetaObject::invokeMethod(dialog.get(),"open")); QTest::qWait(20);
        QCOMPARE(warnings.size(),0);
    }
};
QTEST_MAIN(ShareMountsTest)
#include "share_mounts_test.moc"
