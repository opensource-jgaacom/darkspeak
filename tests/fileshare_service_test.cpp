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
class ServicePeer final : public ds::prot::Peer {
public:
    ServicePeer() : ds::prot::Peer(ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    using ds::prot::Peer::onReceivedEnvelope;
    using ds::prot::Peer::sendCapabilities;
    void initialize(crypto_secretstream_xchacha20poly1305_state &receiver) {
        unsigned char key[crypto_secretstream_xchacha20poly1305_KEYBYTES];
        unsigned char header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
        crypto_secretstream_xchacha20poly1305_keygen(key);
        crypto_secretstream_xchacha20poly1305_init_push(&stateOut, header, key);
        if (crypto_secretstream_xchacha20poly1305_init_pull(&receiver, header, key)) qFatal("stream setup");
        sodium_memzero(key, sizeof key);
    }
    void negotiate() {
        onReceivedEnvelope(codec::makeCapabilities({"fileshare-v1"}, 1048576)); peer_fileshare_utf8_ = true;
    }
};
struct Fixture {
    QTemporaryDir temp;
    QUuid identity = QUuid::createUuid(), contact = QUuid::createUuid();
    std::shared_ptr<ReadOnlyFileManager> backend;
    std::unique_ptr<Vfs> vfs;
    std::unique_ptr<SharePermissions> permissions;
    std::unique_ptr<FileShareService> service;
    bool enabled = true, connected = true, accepted = true, utf8 = true;
    int opens = 0;
    std::function<void()> suspend;
    std::function<void()> request_suspend;
    Fixture() {
        QDir().mkpath(temp.filePath("source/selected"));
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
        FileShareService::Host host;
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
    ~Fixture() {
        service->stop(); service.reset(); permissions->stop(); permissions.reset();
        QCoro::waitFor(vfs->shutdown()); QCoro::waitFor(backend->shutdown());
    }
    void grant(QString location, QVariantList values) {
        QCoro::waitFor(permissions->apply("rule-save", {{"tier", 2}, {"principal", text(contact)}, {"path", location},
            {"permissions", values}, {"denied", 0}}, permissions->revision()));
        if (!permissions->property("error").toString().isEmpty()) qFatal("permission setup: %s", qPrintable(permissions->property("error").toString()));
    }
    FileShareService::SessionPtr session(QUuid c = {}) {
        return service->bind({identity, c.isNull() ? contact : c, QUuid::createUuid(),
            [this] { return accepted; }, [this] { return connected; }, [this] { return utf8; }});
    }
    Envelope run(FileShareService::SessionPtr session, Envelope request) {
        auto response = QCoro::waitFor(service->handle(session, request));
        if (!response) qFatal("unexpected discarded request");
        if (!fileshare::validate(*response, utf8) || !fileshare::matches(request, *response)) qFatal("invalid response");
        return *response;
    }
};
}
class FileShareServiceTest : public QObject {
    Q_OBJECT
private slots:
    void authenticatedTransportDispatch() {
        Fixture f;
        QTcpServer listener; QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        auto peer = std::make_shared<ServicePeer>();
        auto socket = peer->getConnectionPtr(); socket->connectToHost(QHostAddress::LocalHost, listener.serverPort());
        QVERIFY(socket->waitForConnected(1000)); QVERIFY(listener.waitForNewConnection(1000));
        std::unique_ptr<QTcpSocket> remote(listener.nextPendingConnection()); QVERIFY(remote);
        crypto_secretstream_xchacha20poly1305_state receiver{}; peer->initialize(receiver);
        f.service->attach(peer, {f.identity, f.contact, QUuid::createUuid(), [&] { return f.accepted; },
            [&] { return f.connected; }, [&] { return f.utf8; }}, [&] { return f.utf8; });
        auto receive = [&]() {
            if (!QTest::qWaitFor([&] { return remote->bytesAvailable() >= 4; }, 2000)) qFatal("missing record");
            const auto header = remote->peek(4); const auto length = qFromBigEndian<quint32>(header.constData());
            if (!QTest::qWaitFor([&] { return remote->bytesAvailable() >= length + 4; }, 2000)) qFatal("incomplete record");
            const auto record = remote->read(length + 4);
            QByteArray plain(length - crypto_secretstream_xchacha20poly1305_ABYTES, Qt::Uninitialized);
            unsigned char tag = 0;
            if (crypto_secretstream_xchacha20poly1305_pull(&receiver, reinterpret_cast<unsigned char*>(plain.data()), nullptr,
                &tag, reinterpret_cast<const unsigned char*>(record.constData() + 4), length, nullptr, 0)) qFatal("authentication failed");
            TransportEnvelope transport; Envelope envelope;
            if (!codec::deserialize(plain, transport) || !codec::deserialize(transport.payload(), envelope)) qFatal("response decode");
            return envelope;
        };
        peer->sendCapabilities();
        QVERIFY(receive().capabilities().capabilities().contains("fileshare-v1"));
        peer->negotiate(); peer->onReceivedEnvelope(list());
        auto response = receive(); QVERIFY(fileshare::matches(list(), response));
        QCOMPARE(int(response.listDirectoryResult().status()), 1);
        QCOMPARE(response.listDirectoryResult().entries().size(), 1);
        f.enabled = false;
        QVERIFY_EXCEPTION_THROWN(peer->onReceivedEnvelope(list("/share", {}, true, 2)), std::exception);
    }
    void authorizedPagesAndStat() {
        Fixture f; auto s = f.session();
        auto first = f.run(s, list()).listDirectoryResult();
        QCOMPARE(int(first.status()), 1); QCOMPARE(first.entries().size(), 1); QVERIFY(!first.complete());
        QCOMPARE(first.entries().first().name().value(), "a"); QCOMPARE(first.continuation().size(), 32);
        auto second = f.run(s, list("/share", first.continuation())).listDirectoryResult();
        QCOMPARE(int(second.status()), 1); QCOMPARE(second.entries().first().name().value(), "b");
        const auto metadata = f.run(s, stat("/share/a", first.entries().first().nodeId())).statVirtualResult();
        QCOMPARE(int(metadata.status()), 1); QCOMPARE(metadata.entry().size(), quint64(4));
        QVERIFY(metadata.entry().sha256().isEmpty()); // Indexing alone does not verify content.
        QVERIFY(!f.run(s, stat("/")).statVirtualResult().entry().hasModifiedUtcDay());
        auto request = f.permissions->requestFor(f.identity, f.contact); QVERIFY(request);
        QCOMPARE(request.value().permissions->resolve(f.contact, {}, QStringLiteral("/share"), Operation::Read), Decision::Deny);
        QCOMPARE(request.value().permissions->resolve(f.contact, {}, QStringLiteral("/share"), Operation::Export), Decision::Deny);
        QCOMPARE(request.value().permissions->resolve(f.contact, {}, QStringLiteral("/share"), Operation::Recurse), Decision::Deny);
        auto composed = QCoro::waitFor(f.vfs->compose(f.identity, request.value())); QVERIFY(composed);
        auto share = QCoro::waitFor(f.vfs->lookup(f.identity, f.vfs->root(f.identity), "share", composed.value())); QVERIFY(share);
        auto file = QCoro::waitFor(f.vfs->lookup(f.identity, share.value().id, "a", composed.value())); QVERIFY(file);
        QVERIFY(!QCoro::waitFor(f.vfs->prepareRead(f.identity, file.value().id, {}, composed.value())));
        QVERIFY(!QCoro::waitFor(f.vfs->exportCatalog(f.identity, share.value().id, true, composed.value())));
    }
    void authorizationConfinementAndPrivateErrors() {
        Fixture f; auto s = f.session();
        QCOMPARE(int(f.run(s, stat("/private")).statVirtualResult().status()), 2);
        f.grant("/share/a", {2,2,2,2,2,2});
        auto denied = f.run(s, stat("/share/a")).statVirtualResult();
        auto absent = f.run(s, stat("/share/absent")).statVirtualResult();
        QCOMPARE(denied, absent); QVERIFY(!denied.hasEntry()); QVERIFY(!denied.hasPath());
        auto other = f.session(QUuid::createUuid());
        QCOMPARE(int(f.run(other, list()).listDirectoryResult().status()), 2);
        auto invalid = list("/share/../private");
        auto response = QCoro::waitFor(f.service->handle(s, invalid)); QVERIFY(response);
        QCOMPARE(int(response->listDirectoryResult().status()), 4);
    }
    void escapedNamesAndOpaqueNodeContext() {
        Fixture f; auto s = f.session(); f.utf8 = false;
        // The backend logical name already escapes the native literal percent.
        // ASCII presentation escapes that logical percent once more, never decodes it.
        auto metadata = f.run(s, stat("/share/r%C3%A9sum%C3%A9%2525FF", {}, false)).statVirtualResult();
        QCOMPARE(int(metadata.status()), 1); QCOMPARE(metadata.entry().name().value(), "r%C3%A9sum%C3%A9%2525FF");
        auto fresh = f.session();
        QCOMPARE(int(f.run(fresh, stat("/share/r%C3%A9sum%C3%A9%2525FF", metadata.entry().nodeId(), false)).statVirtualResult().status()), 8);
        QCOMPARE(int(f.run(s, stat("/share/r%C3%A9sum%C3%A9%2525FF", metadata.entry().nodeId(), false)).statVirtualResult().status()), 1);
    }
    void cursorIsolationAndRevocation() {
        Fixture f; auto s = f.session();
        auto first = f.run(s, list()).listDirectoryResult();
        QCOMPARE(int(f.run(f.session(), list("/share", first.continuation())).listDirectoryResult().status()), 5);
        QCOMPARE(int(f.run(s, list("/", first.continuation())).listDirectoryResult().status()), 5);
        f.grant("/share", {1,1,1,2,2,2});
        QCOMPARE(int(f.run(s, list("/share", first.continuation())).listDirectoryResult().status()), 5);
        f.suspend = [&] { f.permissions->invalidate(); };
        // Generation captured after open: subsequent fresh authorization still works.
        QCOMPARE(int(f.run(s, list()).listDirectoryResult().status()), 1);
        f.request_suspend = [&] { f.permissions->invalidate(); };
        auto revoked = QCoro::waitFor(f.service->handle(s, list()));
        QVERIFY(!revoked || int(revoked->listDirectoryResult().status()) != 1);
        f.suspend = [&] { f.service->setEnabled(f.identity, false); f.service->setEnabled(f.identity, true); };
        QVERIFY(!QCoro::waitFor(f.service->handle(s, list())));
        f.suspend = [&] { f.connected = false; };
        QVERIFY(!QCoro::waitFor(f.service->handle(s, list())));
    }
    void emptyAuthorizedPageWithContinuation() {
        Fixture f; auto s = f.session();
        f.grant("/share/a", {2,2,2,2,2,2}); f.grant("/share/b", {2,2,2,2,2,2});
        auto first = f.run(s, list()).listDirectoryResult();
        QCOMPARE(int(first.status()), 1); QVERIFY(first.entries().isEmpty());
        QVERIFY(!first.complete()); QVERIFY(!first.continuation().isEmpty());
        auto second = f.run(s, list("/share", first.continuation())).listDirectoryResult();
        QCOMPARE(int(second.status()), 1); QCOMPARE(second.entries().size(), 1);
        QCOMPARE(second.entries().first().name().value(), QString::fromUtf8("résumé%25FF"));
    }
    void recursionRefusalCancellationAndRateBounds() {
        Fixture f; auto s = f.session();
        auto request = list(); auto recursive = request.listDirectoryRequest(); recursive.setRecursive(true); recursive.setPageSize(0);
        request.setListDirectoryRequest(recursive);
        const auto refused = f.run(s, request).listDirectoryResult();
        QCOMPARE(int(refused.status()), 3); QVERIFY(refused.entries().isEmpty()); QVERIFY(!refused.hasDirectory()); QCOMPARE(f.opens, 0);
        auto cancel = [](quint64 target) { Envelope e; e.setProtocolVersion(3); e.setRequestId(target + 1);
            CancelFileShareRequest c; c.setRequestToCancel(target); e.setCancelFileShareRequest(c); return e; };
        f.suspend = [&] { QCOMPARE(int(f.run(s, cancel(1)).cancelFileShareResult().status()), 1); };
        QVERIFY(!QCoro::waitFor(f.service->handle(s, list())));
        bool busy = false;
        for (quint64 i = 3; i < 25; ++i) {
            auto r = f.run(s, list("/share", {}, true, i)).listDirectoryResult();
            if (int(r.status()) == 6) busy = true;
        }
        QVERIFY(busy);
        f.accepted = false; QVERIFY(!QCoro::waitFor(f.service->handle(s, list())));
        f.accepted = true; f.service->stop(); QVERIFY(!QCoro::waitFor(f.service->handle(s, list())));
    }
    void concurrentAdmissionBound() {
        Fixture f; auto s = f.session();
        std::vector<QCoro::Task<std::optional<Envelope>>> requests;
        for (quint64 id = 1; id <= 4; ++id) {
            f.suspend = [] {};
            requests.push_back(f.service->handle(s, list("/share", {}, true, id)));
        }
        QCOMPARE(int(f.run(s, list("/share", {}, true, 5)).listDirectoryResult().status()), 6);
        for (auto &task : requests) {
            auto response = QCoro::waitFor(std::move(task)); QVERIFY(response);
            QCOMPARE(int(response->listDirectoryResult().status()), 1);
        }
    }
};
QTEST_GUILESS_MAIN(FileShareServiceTest)
#include "fileshare_service_test.moc"
