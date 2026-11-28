#include <ds/fileshareservice.h>
#include <ds/sharepermissions.h>
#include <ds/sharemounts.h>
#include <ds/protocolcodec.h>
#include <ds/peer.h>
#include <QTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QCoroTimer>
#include <QTcpServer>
#include <QtEndian>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <dsfilemanager/storagepool.h>

using namespace dsfm;
using namespace dsvfs;
using namespace ds::core;
using namespace ds::prot;
using namespace ds::protocol;
namespace {
QString text(QUuid id) { return id.toString(QUuid::WithoutBraces); }
VirtualPath path(QString value, bool utf8 = true) {
    VirtualPath p; p.setPath(value); p.setEncoding(VirtualPathEncodingGadget::VirtualPathEncoding(utf8 ? 1 : 2)); return p;
}
Envelope list(QString value = "/share", QByteArray cursor = {}, bool utf8 = true, quint64 id = 1) {
    ListDirectoryRequest r; r.setDirectory(path(value, utf8)); r.setPageSize(1); r.setContinuation(cursor);
    Envelope e; e.setProtocolVersion(3); e.setRequestId(id); e.setListDirectoryRequest(r); return e;
}
Envelope stat(QString value, QByteArray known = {}, bool utf8 = true) {
    StatVirtualRequest r; r.setPath(path(value, utf8)); r.setKnownNodeId(known);
    Envelope e; e.setProtocolVersion(3); e.setRequestId(1); e.setStatVirtualRequest(r); return e;
}
struct RecursiveFixture {
    QTemporaryDir temp;
    QUuid identity = QUuid::createUuid(), contact = QUuid::createUuid();
    std::shared_ptr<ReadOnlyFileManager> backend;
    std::unique_ptr<Vfs> vfs;
    std::unique_ptr<SharePermissions> permissions;
    std::unique_ptr<FileShareService> service;
    bool enabled = true, connected = true, accepted = true, utf8 = true;
    int opens = 0, starts = 0, cancellations = 0;
    bool approve = false, accepts = true;
    std::shared_ptr<NativeFileManager> exports;
    std::shared_ptr<StoragePool> pool;
    std::shared_ptr<CatalogExport> staged;
    std::function<void()> suspend;
    std::function<void()> request_suspend;
    RecursiveFixture() {
        QDir().mkpath(temp.filePath("source/selected"));
        QDir().mkpath(temp.filePath(QString::fromUtf8("source/selected/unicodé%FF")));
        for (const auto &name : {QString("a"), QString("b"), QString::fromUtf8("résumé%FF")}) {
            QFile file(temp.filePath("source/selected/") + name);
            if (!file.open(QIODevice::WriteOnly) || file.write("data") != 4) qFatal("source setup");
        }
        QFile outside(temp.filePath("source/private"));
        if (!outside.open(QIODevice::WriteOnly) || outside.write("private") != 7) qFatal("private source setup");
        outside.close();
        backend = std::make_shared<ReadOnlyFileManager>(temp.filePath("index"));
        ReadOnlyNamespaceConfig config; config.common.id = QUuid::createUuid(); config.common.storage_root = temp.filePath("source");
        auto ns = QCoro::waitFor(backend->createSource(config)); if (!ns) qFatal("source create");
        auto selected = QCoro::waitFor(backend->lookup(config.common.id, "/selected"));
        if (!selected) qFatal("subtree lookup");
        Limits limits; limits.max_batch = 2; limits.max_page_size = 2;
        vfs = std::make_unique<Vfs>(temp.filePath("vfs"), limits);
        if (!QCoro::waitFor(vfs->open()) || !vfs->registerNamespace(config.common.id, backend)) qFatal("VFS setup");
        Request admin; admin.scope = identity; admin.generation = 1;
        admin.current_generation = std::make_shared<std::atomic<quint64>>(1);
        admin.authorize = [](Target, Operation) -> Result<bool> { return true; };
        admin.commit_permit = [](Target, Operation) -> Result<std::shared_ptr<void>> { return std::static_pointer_cast<void>(std::make_shared<int>(0)); };
        StorageResource resource; resource.namespace_id = config.common.id;
        auto added = QCoro::waitFor(vfs->addResource(resource, admin)); if (!added) qFatal("resource setup");
        StorageAssignment assignment; assignment.identity = identity;
        assignment.resource = added.value().id; assignment.directory = selected.value().id;
        auto assigned = QCoro::waitFor(vfs->assignStorage(assignment, admin)); if (!assigned) qFatal("assignment setup");
        MountDefinition mount; mount.identity = identity; mount.assignment = assigned.value().id;
        mount.path = "/share"; mount.directory = assignment.directory;
        if (!QCoro::waitFor(vfs->defineMount(mount, admin))) qFatal("mount setup");
        permissions = std::make_unique<SharePermissions>(temp.filePath("access"), SharePermissions::Host{{},
            [this](QUuid id) { return id == identity; },
            [this](QUuid id, QUuid c) { return id == identity && c == contact; },
            [](QUuid, QString, int) -> Result<QVariantList> { return QVariantList{}; },
            [this]() -> QCoro::Task<Result<Vfs*>> { co_return vfs.get(); }});
        permissions->selectIdentity(identity);
        while (permissions->property("busy").toBool()) QCoreApplication::processEvents();
        grant("/", {1,1,1,2,2,2}); // LIST/STAT independent of READ, RECURSE and EXPORT.
        exports = std::make_shared<NativeFileManager>();
        pool = std::make_shared<StoragePool>(temp.filePath("pool"));
        if (!QCoro::waitFor(pool->open())) qFatal("pool setup");
        NamespaceConfig export_config{QUuid::createUuid(), temp.filePath("exports")}; export_config.storage_pool = pool;
        if (!QCoro::waitFor(exports->createNamespace(export_config)) || !vfs->setExportStore(exports, export_config.id)) qFatal("export setup");
        FileShareService::Host host;
        host.approve_recursion = [this](QUuid id, QUuid c, const QString &) { return approve && id == identity && c == contact; };
        host.open = [this]() -> QCoro::Task<Result<Vfs*>> {
            ++opens;
            if (suspend) {
                QTimer timer; timer.setSingleShot(true); timer.start(5);
                auto callback = std::exchange(suspend, {}); callback();
                co_await qCoro(timer).waitForTimeout();
            }
            co_return vfs.get();
        };
        host.request = [this](QUuid id, QUuid c) -> QCoro::Task<Result<Request>> {
            auto request = permissions->requestFor(id, c);
            if (request_suspend) {
                QTimer timer; timer.setSingleShot(true); timer.start(5);
                auto callback = std::exchange(request_suspend, {}); callback();
                co_await qCoro(timer).waitForTimeout();
            }
            co_return request;
        };
        host.enabled = [this](QUuid id) { return id == identity && enabled; };
        host.enable = [this](QUuid id, bool value) { if (id == identity) enabled = value; };
        service = std::make_unique<FileShareService>(host);
    }
    ~RecursiveFixture() {
        service->stop(); service.reset(); staged.reset(); permissions->stop(); permissions.reset();
        QCoro::waitFor(vfs->shutdown()); QCoro::waitFor(backend->shutdown()); QCoro::waitFor(exports->shutdown());
    }
    void grant(QString location, QVariantList values) {
        QVariantMap draft{{"tier", 2}, {"principal", text(contact)}, {"path", location}, {"permissions", values}, {"denied", 0}};
        for (const auto &value : permissions->rules()) {
            const auto rule = value.toMap();
            if (rule["path"] == location && rule["principal"] == text(contact)) draft.insert("id", rule["id"]);
        }
        QCoro::waitFor(permissions->apply("rule-save", draft, permissions->revision()));
        if (!permissions->property("error").toString().isEmpty()) qFatal("permission setup: %s", qPrintable(permissions->property("error").toString()));
    }
    FileShareService::SessionPtr session(QUuid c = {}) {
        return service->bind({identity, c.isNull() ? contact : c, QUuid::createUuid(),
            [this] { return accepted; }, [this] { return connected; }, [this] { return utf8; },
            [this](std::shared_ptr<CatalogExport> catalog) -> Result<FileShareService::CatalogDelivery> {
                staged = std::move(catalog);
                return FileShareService::CatalogDelivery{QByteArray(16, 't'), [this]() -> Result<void> { ++starts; return {}; },
                    [this] { ++cancellations; staged.reset(); }};
            }, [this] { return accepts; }});
    }
    Envelope run(FileShareService::SessionPtr session, Envelope request) {
        auto response = QCoro::waitFor(service->handle(session, request));
        if (!response) qFatal("unexpected discarded request");
        if (!fileshare::validate(*response, utf8) || !fileshare::matches(request, *response)) qFatal("invalid response");
        return *response;
    }
};
}
Envelope recursive(QString location = "/share", quint64 id = 1) {
    auto e = list(location, {}, true, id); auto r = e.listDirectoryRequest(); r.setRecursive(true); r.setPageSize(0);
    e.setListDirectoryRequest(r); return e;
}
class FileShareRecursiveServiceTest : public QObject {
    Q_OBJECT
private slots:
    void explicitRefusalBeforeVfs() {
        RecursiveFixture f; auto s = f.session();
        auto r = f.run(s, recursive()).listDirectoryResult();
        QCOMPARE(int(r.status()), 3); QVERIFY(r.entries().isEmpty()); QVERIFY(!r.hasDirectory());
        QVERIFY(r.catalogTransferId().isEmpty()); QCOMPARE(r.catalogFormatVersion(), 0u);
        QCOMPARE(f.opens, 0); QCOMPARE(f.starts, 0); QVERIFY(!f.staged);
        // Refusing bulk export leaves ordinary, explicitly requested navigation usable.
        const auto direct = f.run(s, list("/share", {}, true, 2)).listDirectoryResult();
        QCOMPARE(int(direct.status()), 1); QCOMPARE(direct.entries().size(), 1);
        QVERIFY(!direct.complete()); QVERIFY(!direct.continuation().isEmpty());
        QVERIFY(!f.staged); QCOMPARE(f.starts, 0);
    }
    void peerCatalogPreferenceRefusesBeforeVfs() {
        RecursiveFixture f; f.approve = true; f.accepts = false; auto s = f.session();
        const auto r = f.run(s, recursive()).listDirectoryResult();
        QCOMPARE(int(r.status()), 3); QVERIFY(r.entries().isEmpty()); QVERIFY(!r.hasDirectory());
        QVERIFY(r.catalogTransferId().isEmpty()); QCOMPARE(f.opens, 0); QCOMPARE(f.starts, 0);
    }
    void authorizedImmutableCatalogPublishedAfterControl() {
        RecursiveFixture f; f.approve = true; f.grant("/", {1,1,1,1,1,1}); auto s = f.session();
        const auto r = f.run(s, recursive()).listDirectoryResult();
        QCOMPARE(int(r.status()), 1); QCOMPARE(r.catalogFormatVersion(), 2u);
        QCOMPARE(r.catalogTransferId(), QByteArray(16, 't')); QVERIFY(r.entries().isEmpty()); QVERIFY(!r.complete());
        QCOMPARE(f.starts, 0); QVERIFY(f.staged); QVERIFY(f.staged->isAuthorized());
        auto read = QCoro::waitFor(f.staged->prepareRead()); QVERIFY(read);
        std::shared_ptr<IFileReader> reader(std::move(read.value().reader));
        auto valid = QCoro::waitFor(f.vfs->validateCatalog(reader, f.staged->size(), f.staged->sha256(), {}, true, true)); QVERIFY(valid);
        QVERIFY(QCoro::waitFor(reader->close()));
        QVERIFY(f.service->publishCatalog(s, 1)); QCOMPARE(f.starts, 1); QCOMPARE(f.cancellations, 0);
        QVERIFY(f.service->publishCatalog(s, 1)); QCOMPARE(f.starts, 1);
    }
    void completeCatalogMembership_data() {
        QTest::addColumn<bool>("deny_child");
        QTest::newRow("all-visible") << false;
        QTest::newRow("denied-child") << true;
    }
    void completeCatalogMembership() {
        QFETCH(bool, deny_child);
        RecursiveFixture f; f.approve = true; f.grant("/", {1,1,1,1,1,1});
        if (deny_child) f.grant("/share/b", {2,2,2,2,2,2});
        auto s = f.session();
        QCOMPARE(int(f.run(s, recursive()).listDirectoryResult().status()), 1);
        QVERIFY(f.staged); QCOMPARE(f.starts, 0);
        auto prepared = QCoro::waitFor(f.staged->prepareRead()); QVERIFY(prepared);
        std::shared_ptr<IFileReader> reader(std::move(prepared.value().reader));
        QByteArray bytes;
        QVERIFY(f.staged->size() < 16384); // Bound this fixture's whole-artifact inspection.
        while (quint64(bytes.size()) < f.staged->size()) {
            auto chunk = QCoro::waitFor(reader->readAt(bytes.size(), 1024)); QVERIFY(chunk);
            QVERIFY(!chunk.value().isEmpty()); bytes += chunk.value();
        }
        QCOMPARE(quint64(bytes.size()), f.staged->size());
        QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), f.staged->sha256());
        QVERIFY(QCoro::waitFor(f.vfs->validateCatalog(reader, f.staged->size(), f.staged->sha256(), {}, true, true)));
        QVERIFY(QCoro::waitFor(reader->close()));
        QStringList names;
        int headers = 0, roots = 0;
        for (const auto &line : bytes.split('\n')) {
            if (line.isEmpty()) continue;
            QJsonParseError error;
            const auto record = QJsonDocument::fromJson(line, &error).object();
            QCOMPARE(error.error, QJsonParseError::NoError);
            if (record.contains("format")) { ++headers; continue; }
            if (record["parent"].toString().isEmpty()) { ++roots; continue; }
            names.append(record["name"].toString());
        }
        QStringList expected{"a", QString::fromUtf8("résumé%25FF"), QString::fromUtf8("unicodé%25FF")};
        if (!deny_child) expected.append("b");
        names.sort(); expected.sort(); QCOMPARE(names, expected);
        QCOMPARE(headers, 1); QCOMPARE(roots, 1);
        QVERIFY(!bytes.contains("private")); QVERIFY(!bytes.contains(f.temp.path().toUtf8()));
        QVERIFY(f.service->publishCatalog(s, 1)); QCOMPARE(f.starts, 1);
    }
    void permissionDeniedNeverStages() {
        RecursiveFixture f; f.approve = true; auto s = f.session();
        const auto r = f.run(s, recursive()).listDirectoryResult();
        QVERIFY(int(r.status()) != 1); QVERIFY(r.entries().isEmpty()); QVERIFY(!r.hasDirectory());
        QVERIFY(r.catalogTransferId().isEmpty()); QVERIFY(!f.staged); QCOMPARE(f.starts, 0);
        QCOMPARE(f.opens, 0);
    }
    void withdrawnBeforeOfferCancelsStagedTransfer() {
        RecursiveFixture f; f.approve = true; f.grant("/", {1,1,1,1,1,1}); auto s = f.session();
        QCOMPARE(int(f.run(s, recursive()).listDirectoryResult().status()), 1);
        f.grant("/", {1,1,1,1,1,2});
        QVERIFY(!f.service->publishCatalog(s, 1)); QCOMPARE(f.starts, 0); QCOMPARE(f.cancellations, 1); QVERIFY(!f.staged);
    }
    void cancelledAfterStageDoesNotOffer() {
        RecursiveFixture f; f.approve = true; f.grant("/", {1,1,1,1,1,1}); auto s = f.session();
        QCOMPARE(int(f.run(s, recursive()).listDirectoryResult().status()), 1);
        Envelope cancel; cancel.setProtocolVersion(3); cancel.setRequestId(2); CancelFileShareRequest c; c.setRequestToCancel(1);
        cancel.setCancelFileShareRequest(c); QCOMPARE(int(f.run(s, cancel).cancelFileShareResult().status()), 1);
        QCOMPARE(f.starts, 0); QCOMPARE(f.cancellations, 1); QVERIFY(!f.staged);
        QVERIFY(f.service->publishCatalog(s, 1)); QCOMPARE(f.starts, 0);
    }
    void disconnectedDuringAdmissionPublishesNothing() {
        RecursiveFixture f; f.approve = true; f.grant("/", {1,1,1,1,1,1}); auto s = f.session();
        f.suspend = [&] { f.connected = false; };
        QVERIFY(!QCoro::waitFor(f.service->handle(s, recursive()))); QVERIFY(!f.staged); QCOMPARE(f.starts, 0);
    }
    void retainedArtifactsHaveGlobalAdmissionBound() {
        RecursiveFixture f; f.approve = true; f.grant("/", {1,1,1,1,1,1});
        auto a = f.session(), b = f.session(), c = f.session();
        QCOMPARE(int(f.run(a, recursive()).listDirectoryResult().status()), 1);
        QCOMPARE(int(f.run(b, recursive()).listDirectoryResult().status()), 1);
        const auto busy = f.run(c, recursive()).listDirectoryResult();
        QCOMPARE(int(busy.status()), 6); QVERIFY(busy.entries().isEmpty()); QVERIFY(!busy.hasDirectory());
        QVERIFY(busy.catalogTransferId().isEmpty()); QCOMPARE(f.starts, 0);
    }
    void localPolicyWithdrawalBeforeOfferCancels() {
        RecursiveFixture f; f.approve = true; f.grant("/", {1,1,1,1,1,1}); auto s = f.session();
        QCOMPARE(int(f.run(s, recursive()).listDirectoryResult().status()), 1);
        f.approve = false;
        QVERIFY(!f.service->publishCatalog(s, 1)); QCOMPARE(f.starts, 0); QCOMPARE(f.cancellations, 1);
    }
    void escapedRootUsesLogicalPermissionPathOnce() {
        RecursiveFixture f; f.approve = true; f.utf8 = false;
        f.grant("/", {1,1,1,1,1,2});
        f.grant(QString::fromUtf8("/share/unicodé%25FF"), {1,1,1,1,1,1});
        auto s = f.session(); auto e = recursive("/share/unicod%C3%A9%2525FF");
        auto r = e.listDirectoryRequest(); r.setDirectory(path("/share/unicod%C3%A9%2525FF", false)); e.setListDirectoryRequest(r);
        QCOMPARE(int(f.run(s, e).listDirectoryResult().status()), 1);
        QVERIFY(f.staged); QVERIFY(!f.staged->requiresUtf8());
        QVERIFY(f.service->publishCatalog(s, 1)); QCOMPARE(f.starts, 1);
    }
    void revokedRetainedArtifactsDoNotExhaustAdmission() {
        RecursiveFixture f; f.approve = true; f.grant("/", {1,1,1,1,1,1});
        auto a = f.session(), b = f.session(), replacement = f.session();
        QCOMPARE(int(f.run(a, recursive()).listDirectoryResult().status()), 1);
        auto retained_a = f.staged; QVERIFY(f.service->publishCatalog(a, 1));
        QCOMPARE(int(f.run(b, recursive()).listDirectoryResult().status()), 1);
        auto retained_b = f.staged; QVERIFY(f.service->publishCatalog(b, 1));
        f.grant("/", {1,1,1,1,1,2});
        QVERIFY(!retained_a->isAuthorized()); QVERIFY(!retained_b->isAuthorized());
        QVERIFY(QCoro::waitFor(f.vfs->refresh()));
        f.grant("/", {1,1,1,1,1,1});
        QCOMPARE(int(f.run(replacement, recursive()).listDirectoryResult().status()), 1);
        QVERIFY(f.staged->isAuthorized());
    }
};
QTEST_GUILESS_MAIN(FileShareRecursiveServiceTest)
#include "fileshare_recursive_service_test.moc"
