#include "ds/sharepermissions.h"
#include "ds/sharemounts.h"
#include "appearance.h"
#include <dsfilemanager/readonlyfilemanager.h>
#include <QTest>
#include <QCryptographicHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
using namespace dsfm;
using namespace dsvfs;
using ds::core::SharePermissions;
using ds::core::ShareMounts;
namespace {
QString text(QUuid id) { return id.toString(QUuid::WithoutBraces); }
QVariantMap rule(int tier = 0, QUuid principal = {}, QString path = "/", QVariantList values = {1,1,1,1,1,1}, int denied = 0) {
    return {{"tier", tier}, {"principal", text(principal)}, {"path", path}, {"permissions", values}, {"denied", denied}};
}
void apply(SharePermissions &m, QString command, QVariantMap values) {
    QCoro::waitFor(m.apply(command, values, m.revision()));
    if (!m.property("error").toString().isEmpty()) qFatal("apply failed: %s", qPrintable(m.property("error").toString()));
}
struct Fixture {
    QTemporaryDir temp;
    QUuid identity = QUuid("11111111-1111-1111-1111-111111111111"), other_identity = QUuid::createUuid();
    QUuid contact = QUuid("22222222-2222-2222-2222-222222222222"), other_contact = QUuid::createUuid(), ns = QUuid::createUuid();
    std::shared_ptr<ReadOnlyFileManager> backend = std::make_shared<ReadOnlyFileManager>(temp.filePath("index"));
    std::unique_ptr<Vfs> vfs;
    Fixture() {
        QDir().mkpath(temp.filePath("source/sub/deep"));
        for (const auto &path : {QString("source/sub/hidden"),QString("source/sub/deep/inside")}) {
            QFile child(temp.filePath(path));
            if (!child.open(QIODevice::WriteOnly) || child.write("test") != 4) qFatal("child setup");
        }
        QFile f(temp.filePath("source/file")); if (!f.open(QIODevice::WriteOnly) || f.write("test") != 4) qFatal("source file setup"); f.close();
        ReadOnlyNamespaceConfig config; config.common.id = ns; config.common.storage_root = temp.filePath("source");
        if (!QCoro::waitFor(backend->createSource(config)) || !QCoro::waitFor(backend->reconcile(ns,{}))) qFatal("source setup");
        vfs = std::make_unique<Vfs>(temp.filePath("vfs"));
        if (!QCoro::waitFor(vfs->open()) || !vfs->registerNamespace(ns,backend)) qFatal("VFS setup");
        ShareMounts mounts({{}, [this](QUuid id) { return id == identity; },
            [this]() -> QCoro::Task<Result<Vfs*>> { co_return vfs.get(); }, [this]() -> IFileManager* { return backend.get(); }});
        mounts.selectIdentity(identity); while (mounts.busy()) QCoreApplication::processEvents();
        QCoro::waitFor(mounts.run(ShareMounts::SelectSource,text(ns)));
        QCoro::waitFor(mounts.run(ShareMounts::Assign));
        QCoro::waitFor(mounts.run(ShareMounts::AddMount,mounts.assignments().first().toMap()["id"].toString()));
        if (!mounts.error().isEmpty()) qFatal("mount setup");
    }
    ~Fixture() { QCoro::waitFor(vfs->shutdown()); QCoro::waitFor(backend->shutdown()); }
    QString root() { return temp.filePath("access"); }
    SharePermissions::Host host() {
        return {{}, [this](QUuid id) { return id == identity || id == other_identity; },
            [this](QUuid id, QUuid c) { return (id == identity && c == contact) || (id == other_identity && c == other_contact); },
            [this](QUuid id, QString filter, int page) -> Result<QVariantList> {
                if (page || !QString("Contact").contains(filter,Qt::CaseInsensitive)) return QVariantList{};
                return QVariantList{QVariantMap{{"id", text(id == identity ? contact : other_contact)}, {"name", "Contact"}}};
            }, [this]() -> QCoro::Task<Result<Vfs*>> { co_return vfs.get(); }};
    }
    void select(SharePermissions &m) {
        m.selectIdentity(identity);
        while (m.property("busy").toBool()) QCoreApplication::processEvents();
        if (!m.property("error").toString().isEmpty()) qFatal("selection failed");
    }
    Request request(SharePermissions &m) {
        auto r = m.requestFor(identity,contact); if (!r) qFatal("request failed");
        auto c = QCoro::waitFor(vfs->compose(identity,r.value())); if (!c) qFatal("compose failed");
        return c.value();
    }
};
}
class SharePermissionsTest : public QObject {
    Q_OBJECT
private slots:
    void versionFixtureAndNavigation() {
        Fixture f;
        QDir().mkpath(f.root());
        const auto config = QDir(f.root()).filePath(text(f.identity)+".json");
        QVERIFY(QFile::copy(QStringLiteral(DS_APP_QML "/../../tests/fixtures/identity-access-v1.json"),config));
        SharePermissions model(f.root(),f.host()); f.select(model);
        QCOMPARE(model.revision(),text(f.identity)+":7");
        QCOMPARE(model.groups().size(),1); QCOMPARE(model.rules().size(),1);
        auto request = model.requestFor(f.identity,f.contact); QVERIFY(request);
        QCOMPARE(request.value().groups.size(),1);
        QVERIFY(request.value().permissions->explain(f.contact,request.value().groups,u"/share",Operation::Read).allowed());
        // Group barrier, reopened at contact tier: ancestors allow virtual
        // navigation but must never list the denied source's direct files.
        apply(model,"rule-save",rule(1,QUuid("33333333-3333-3333-3333-333333333333"),"/share",{0,0,0,0,0,0},2));
        apply(model,"rule-save",rule(2,f.contact,"/share/sub/deep"));
        QCoro::waitFor(model.inspect(f.contact,"/share"));
        const auto rows = model.property("preview").toList();
        QCOMPARE(rows.size(),1); QCOMPARE(rows[0].toMap()["name"].toString(),"sub");
        auto composed = f.request(model);
        auto mount = QCoro::waitFor(f.vfs->lookup(f.identity,f.vfs->root(f.identity),"share",composed)); QVERIFY(mount);
        auto sub = QCoro::waitFor(f.vfs->lookup(f.identity,mount.value().id,"sub",composed)); QVERIFY(sub);
        QVERIFY(!sub.value().modified.isValid()); QCOMPARE(sub.value().size,0);
        QVERIFY(sub.value().sha256.isEmpty()); QVERIFY(sub.value().capabilities == Capabilities{});
        auto snapshot = QCoro::waitFor(f.vfs->stat(f.identity,sub.value().id,composed)); QVERIFY(snapshot);
        QVERIFY(!snapshot.value().modified.isValid());
        QCoro::waitFor(model.inspect(f.contact,"/share/sub"));
        QVERIFY(model.property("error").toString().isEmpty());
        QCOMPARE(model.property("preview").toList().size(),1);
        QCOMPARE(model.property("preview").toList()[0].toMap()["name"].toString(),"deep");
        auto deep = QCoro::waitFor(f.vfs->lookup(f.identity,sub.value().id,"deep",composed)); QVERIFY(deep);
        auto inside = QCoro::waitFor(f.vfs->lookup(f.identity,deep.value().id,"inside",composed)); QVERIFY(inside);
        auto reader = QCoro::waitFor(f.vfs->prepareRead(f.identity,inside.value().id,{},composed)); QVERIFY(reader);
        QCOMPARE(reader.value().info.sha256,QCryptographicHash::hash("test",QCryptographicHash::Sha256));
        auto bytes = QCoro::waitFor(reader.value().reader->readAt(0,4)); QVERIFY(bytes); QCOMPARE(bytes.value(),"test");
        // An ordinary explicit recursion denial still forbids implicit source
        // descent, even while other descendant permissions allow the file.
        apply(model,"rule-save",rule(2,f.contact,"/share",{0,0,0,0,2,0}));
        auto blocked = f.request(model);
        QVERIFY(!QCoro::waitFor(f.vfs->prepareRead(f.identity,inside.value().id,{},blocked)));
        QVERIFY(!QCoro::waitFor(reader.value().reader->readAt(0,4)));
    }
    void viewPersistAndRevoke() {
        Fixture f; SharePermissions model(f.root(),f.host()); f.select(model);
        QVERIFY(!model.requestFor(f.identity,f.other_contact));
        QCoro::waitFor(model.inspect(f.contact,"/"));
        QVERIFY(model.property("preview").toList().isEmpty());
        apply(model,"rule-save",rule());
        auto request = f.request(model);
        auto mount = QCoro::waitFor(f.vfs->lookup(f.identity,f.vfs->root(f.identity),"share",request)); QVERIFY(mount);
        auto file = QCoro::waitFor(f.vfs->lookup(f.identity,mount.value().id,"file",request)); QVERIFY(file);
        auto prepared = QCoro::waitFor(f.vfs->prepareRead(f.identity,file.value().id,{},request)); QVERIFY(prepared);
        auto bytes = QCoro::waitFor(prepared.value().reader->readAt(0,4)); QVERIFY(bytes); QCOMPARE(bytes.value(),"test");
        QCoro::waitFor(model.inspect(f.contact,"/share"));
        QCOMPARE(model.property("preview").toList().size(),2);
        QCOMPARE(model.property("decisions").toList().size(),6);
        auto deniedRule = rule(2,f.contact,"/share",{0,0,0,2,0,2});
        apply(model,"rule-save",deniedRule);
        QVERIFY(!request.current());
        QVERIFY(!QCoro::waitFor(prepared.value().reader->readAt(0,4)));
        request = f.request(model);
        QVERIFY(!QCoro::waitFor(f.vfs->prepareRead(f.identity,file.value().id,{},request)));
        QCoro::waitFor(model.inspect(f.contact,"/share"));
        QCOMPARE(model.property("preview").toList().size(),2); // LIST independent of READ/EXPORT
        SharePermissions restarted(f.root(),f.host()); f.select(restarted);
        auto r = restarted.requestFor(f.identity,f.contact); QVERIFY(r);
        QCOMPARE(r.value().permissions->resolve(f.contact,{},u"/share",Operation::Read),Decision::Deny);
        QCOMPARE(restarted.rules().size(),2);
        apply(restarted,"rule-save",rule(2,f.contact,"/share",{0,0,0,0,0,0},2));
        QCoro::waitFor(restarted.inspect(f.contact,"/"));
        QVERIFY(restarted.property("preview").toList().isEmpty());
    }
    void groupsPrecedenceMembershipAndIsolation() {
        Fixture f; SharePermissions model(f.root(),f.host()); f.select(model);
        apply(model,"group-add",{{"name","Readers"}});
        apply(model,"group-add",{{"name","Restricted"}});
        const QUuid readers(model.groups()[0].toMap()["id"].toString());
        const QUuid restricted(model.groups()[1].toMap()["id"].toString());
        apply(model,"member-add",{{"id",text(readers)},{"contact",text(f.contact)}});
        apply(model,"member-add",{{"id",text(restricted)},{"contact",text(f.contact)}});
        apply(model,"rule-save",rule(1,readers));
        apply(model,"rule-save",rule(1,restricted,"/share",{0,0,0,2,0,0}));
        auto r = model.requestFor(f.identity,f.contact); QVERIFY(r);
        QCOMPARE(r.value().groups.size(),2);
        auto decision = r.value().permissions->explain(f.contact,r.value().groups,u"/share",Operation::Read);
        QVERIFY(!decision.allowed());
        bool losingAllow = false;
        for (const auto &c : decision.contributions) losingAllow |= c.principal == readers && c.permission == Decision::Allow;
        QVERIFY(losingAllow);
        apply(model,"rule-save",rule(2,f.contact,"/share",{0,0,0,1,0,0}));
        r = model.requestFor(f.identity,f.contact); QVERIFY(r);
        QVERIFY(r.value().permissions->explain(f.contact,r.value().groups,u"/share",Operation::Read).allowed());
        auto old = r.value();
        apply(model,"member-remove",{{"id",text(readers)},{"contact",text(f.contact)}});
        QVERIFY(!old.current());
        auto fresh = f.request(model);
        auto page = QCoro::waitFor(f.vfs->list(f.identity,f.vfs->root(f.identity),{},fresh));
        QVERIFY(!page || page.value().entries.isEmpty());
        QCoro::waitFor(model.apply("member-add",{{"id",text(readers)},{"contact",text(f.other_contact)}},model.revision()));
        QVERIFY(!model.property("error").toString().isEmpty());
        QCoro::waitFor(model.apply("group-remove",{{"id",text(readers)}},model.revision()));
        QVERIFY(!model.property("error").toString().isEmpty()); // refuse rule dependencies
        apply(model,"group-rename",{{"id",text(readers)},{"name","Renamed"}});
        QCOMPARE(model.groups()[0].toMap()["id"].toString(),text(readers));
        SharePermissions restarted(f.root(),f.host()); f.select(restarted);
        QCOMPARE(restarted.requestFor(f.identity,f.contact).value().groups,QSet<QUuid>{restricted});
        QVERIFY(!restarted.requestFor(f.other_identity,f.contact));
        auto loaded = QCoro::waitFor(restarted.load(f.other_identity)); QVERIFY(loaded);
        auto other = restarted.requestFor(f.other_identity,f.other_contact); QVERIFY(other);
        QVERIFY(other.value().groups.isEmpty()); QVERIFY(other.value().permissions->rules.isEmpty());
        const auto pending = restarted.revision();
        restarted.selectIdentity(f.other_identity);
        QTRY_VERIFY(!restarted.property("busy").toBool());
        QCoro::waitFor(restarted.apply("rule-save",rule(),pending));
        QVERIFY(!restarted.property("error").toString().isEmpty()); QVERIFY(restarted.rules().isEmpty());
    }
    void staleInvalidAndFailedPublication() {
        Fixture f; SharePermissions model(f.root(),f.host()); f.select(model);
        const auto revision = model.revision();
        apply(model,"rule-save",rule());
        QCoro::waitFor(model.apply("rule-save",rule(2,f.contact),revision));
        QVERIFY(!model.property("error").toString().isEmpty()); QCOMPARE(model.rules().size(),1);
        for (auto bad : {rule(2,f.other_contact),rule(0,{},"/../escape"),rule(1,QUuid::createUuid())}) {
            QCoro::waitFor(model.apply("rule-save",bad,model.revision()));
            QVERIFY(!model.property("error").toString().isEmpty()); QCOMPARE(model.rules().size(),1);
        }
        const auto old = model.requestFor(f.identity,f.contact).value();
        const auto config = QDir(f.root()).filePath(text(f.identity)+".json");
        QVERIFY(QFile::rename(config,config+".saved")); QVERIFY(QDir().mkdir(config));
        QCoro::waitFor(model.apply("rule-save",rule(2,f.contact),model.revision()));
        QVERIFY(!model.property("error").toString().isEmpty()); QCOMPARE(model.rules().size(),1); QVERIFY(old.current());
        QVERIFY(QDir().rmdir(config)); QVERIFY(QFile::rename(config+".saved",config));
        QFile file(config); QVERIFY(file.open(QIODevice::ReadWrite));
        auto obj = QJsonDocument::fromJson(file.readAll()).object(); QCOMPARE(obj["version"].toInt(),1);
        obj["version"] = 999; file.resize(0); file.write(QJsonDocument(obj).toJson()); file.close();
        SharePermissions corrupt(f.root(),f.host());
        QVERIFY(!QCoro::waitFor(corrupt.load(f.identity))); QVERIFY(!corrupt.requestFor(f.identity,f.contact));
    }
    void qmlDialog() {
        Fixture f; SharePermissions model(f.root(),f.host()); f.select(model);
        apply(model,"group-add",{{"name","Readers"}});
        apply(model,"rule-save",rule());
        QCoro::waitFor(model.inspect(f.contact,"/share"));
        Appearance appearance; QQuickWindow window; window.resize(1100,1000);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,f.temp.filePath("settings"));
        QQmlEngine engine; QSignalSpy warnings(&engine,&QQmlEngine::warnings);
        engine.rootContext()->setContextProperty("appearance",&appearance);
        engine.rootContext()->setContextProperty("mainWindow",&window);
        engine.rootContext()->setContextProperty("sharePermissions",&model);
        QQmlComponent component(&engine,QUrl::fromLocalFile(QStringLiteral(DS_APP_QML "/SharePermissionsDialog.qml")));
        std::unique_ptr<QObject> dialog(component.create()); QVERIFY2(dialog,qPrintable(component.errorString()));
        dialog->setProperty("parent",QVariant::fromValue(window.contentItem()));
        QVERIFY(QMetaObject::invokeMethod(dialog.get(),"open")); QTest::qWait(30);
        QCOMPARE(warnings.size(),0);
    }
};
QTEST_MAIN(SharePermissionsTest)
#include "share_permissions_test.moc"
