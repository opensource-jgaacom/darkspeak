#pragma once
#include <ds/filebrowsermodel.h>
#include <ds/protocolcodec.h>
#include <dsfilemanager/readonlyfilemanager.h>
#include <QFile>
#include <QTemporaryDir>
#include <QDir>
#include <QTest>
using namespace ds::models;
using namespace dsfm;
using namespace dsvfs;
using namespace ds::prot;
using namespace ds::protocol;
namespace {
template<class T> T get(QCoro::Task<Result<T>> task) {
    auto result = QCoro::waitFor(std::move(task));
    if (!result) qFatal("fixture setup failed code=%d", int(result.error().code));
    return std::move(result.value());
}
struct Fixture {
    QTemporaryDir temp;
    QUuid identity = QUuid::createUuid(), contact = QUuid::createUuid(), connection = QUuid::createUuid();
    std::shared_ptr<ReadOnlyFileManager> backend;
    std::unique_ptr<Vfs> vfs;
    std::unique_ptr<FileShareService> service;
    FileShareService::SessionPtr session;
    Request admin;
    bool live = true, exists = true, allowed = true, readable = true, utf8 = true, corrupt = false, wrong_connection = false, offline_permit = false;
    int delay = 0, sends = 0, cancels = 0;
    quint64 next = 0;
    explicit Fixture(int extra_files = 0, quint32 reconcile_ms = 30000) {
        QDir().mkpath(temp.filePath("source/selected/sub/deep"));
        for (const auto &name : {QString("a"), QString::fromUtf8("résumé%FF")}) {
            QFile file(temp.filePath("source/selected/") + name);
            if (!file.open(QIODevice::WriteOnly) || file.write("data") != 4) qFatal("source setup");
        }
        QFile private_file(temp.filePath("source/private"));
        if (!private_file.open(QIODevice::WriteOnly) || private_file.write("private") != 7) qFatal("source setup");
        private_file.close();
        for (int i = 0; i < extra_files; ++i) {
            QFile file(temp.filePath(QString("source/selected/f%1").arg(i, 3, 10, QChar('0'))));
            if (!file.open(QIODevice::WriteOnly) || file.write("data") != 4) qFatal("extra source setup");
        }
        backend = std::make_shared<ReadOnlyFileManager>(temp.filePath("index"));
        ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), temp.filePath("source")};
        config.limits.reconcile_interval_ms = reconcile_ms;
        get(backend->createSource(config));
        auto selected = get(backend->lookup(config.common.id, "/selected"));
        vfs = std::make_unique<Vfs>(temp.filePath("vfs"));
        if (!QCoro::waitFor(vfs->open()) || !vfs->registerNamespace(config.common.id, backend)) qFatal("VFS setup");
        admin.scope = identity; admin.generation = 1;
        admin.current_generation = std::make_shared<std::atomic<quint64>>(1);
        admin.authorize = [](Target, Operation) -> Result<bool> { return true; };
        admin.commit_permit = [](Target, Operation) -> Result<std::shared_ptr<void>> { return std::static_pointer_cast<void>(std::make_shared<int>(0)); };
        auto resource = get(vfs->addResource({{}, config.common.id, {}}, admin));
        auto assignment = get(vfs->assignStorage({{}, identity, resource.id, selected.id, false}, admin));
        MountDefinition mount; mount.identity = identity; mount.assignment = assignment.id;
        mount.directory = selected.id; mount.path = "/share";
        get(vfs->defineMount(mount, admin));
        FileShareService::Host host;
        host.open = [this]() -> QCoro::Task<Result<Vfs*>> { co_return vfs.get(); };
        host.request = [this](QUuid id, QUuid c) -> QCoro::Task<Result<Request>> {
            if (id != identity || c != contact) co_return Error{ErrorCode::PermissionDenied};
            auto request = admin; request.scope = contact;
            request.authorize = [this](Target, Operation op) -> Result<bool> { return allowed && (op != Operation::Read || readable); };
            co_return request;
        };
        host.enabled = [this](QUuid id) { return exists && id == identity; };
        service = std::make_unique<FileShareService>(host);
        session = service->bind({identity, contact, connection, [this] { return allowed; }, [this] { return live; }, [this] { return utf8; }});
    }
    ~Fixture() {
        service->stop(); session.reset(); service.reset();
        QCoro::waitFor(vfs->shutdown()); QCoro::waitFor(backend->shutdown());
    }
    LocalBrowseProvider::Host localHost() {
        return {{}, [this](QUuid id) { return exists && id == identity; },
            [this]() -> QCoro::Task<Result<Vfs*>> { co_return vfs.get(); }};
    }
    RemoteBrowseProvider::Context remoteContext(BrowserScope scope = {}) {
        if (scope.identity.isNull()) scope = {identity, contact};
        return {scope, connection, utf8, [this] { return live && exists; },
            [this](Envelope request) {
                ++sends; const auto id = ++next; request.setRequestId(id);
                if (request.hasCancelFileShareRequest()) { ++cancels; return id; }
                // Test transport uses the real wire codec and server. Delivery is
                // queued so it has the same non-reentrant contract as the socket.
                Envelope decoded;
                if (!ds::prot::codec::deserialize(ds::prot::codec::serialize(request), decoded)) qFatal("request codec");
                auto response = QCoro::waitFor(service->handle(session, decoded));
                if (!response) return quint64(0);
                if (!ds::prot::codec::deserialize(ds::prot::codec::serialize(*response), decoded)) qFatal("response codec");
                if (corrupt && decoded.hasStatVirtualResult()) {
                    auto result = decoded.statVirtualResult(); auto entry = result.entry();
                    entry.setNodeId("invalid"); result.setEntry(entry); decoded.setStatVirtualResult(result);
                }
                if (corrupt && decoded.hasListDirectoryResult()) {
                    auto result = decoded.listDirectoryResult(); auto entries = result.entries();
                    if (!entries.isEmpty()) { entries.first().setNodeId("invalid"); result.setEntries(entries); }
                    decoded.setListDirectoryResult(result);
                }
                QPointer<FileShareService> hub(service.get());
                if (wrong_connection) QTimer::singleShot(0, hub, [hub, decoded] { if (hub) emit hub->responseReceived(QUuid::createUuid(), decoded); });
                QTimer::singleShot(delay, hub, [hub, connection = connection, decoded] { if (hub) emit hub->responseReceived(connection, decoded); });
                return id;
            }, service.get(), [this] { return !live && exists && offline_permit; }};
    }
    FileBrowserModel::Factory factory() {
        return [this](FileBrowserModel::Location location, BrowserScope scope) -> Result<std::shared_ptr<BrowseProvider>> {
            if (scope.identity != identity || (location == FileBrowserModel::Remote && scope.contact != contact)) return Error{ErrorCode::PermissionDenied};
            if (location == FileBrowserModel::Local)
                return std::static_pointer_cast<BrowseProvider>(std::make_shared<LocalBrowseProvider>(scope.identity, localHost()));
            return std::static_pointer_cast<BrowseProvider>(std::make_shared<RemoteBrowseProvider>(remoteContext(scope)));
        };
    }
};
}
