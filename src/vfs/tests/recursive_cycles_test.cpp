#include <dsvfs/vfs.h>
#include <dsfilemanager/readonlyfilemanager.h>
#include <dsfilemanager/storagepool.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QTimer>
#include <QThread>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <source_location>
using namespace dsfm;
#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__))                                                                                            \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + " " #__VA_ARGS__);       \
    } while (false)
template <class T> T get(QCoro::Task<Result<T>> task, std::source_location at = std::source_location::current()) {
    auto result = QCoro::waitFor(std::move(task));
    if (!result)
        throw std::runtime_error(std::string(at.file_name()) + ":" + std::to_string(at.line()) +
                                 " error=" + std::to_string(int(result.error().code)));
    return std::move(result).value();
}
void get(QCoro::Task<Result<void>> task, std::source_location at = std::source_location::current()) {
    auto result = QCoro::waitFor(std::move(task));
    if (!result)
        throw std::runtime_error(std::string(at.file_name()) + ":" + std::to_string(at.line()) +
                                 " error=" + std::to_string(int(result.error().code)));
}
template <class T> void error(QCoro::Task<Result<T>> task, ErrorCode expected) {
    auto result = QCoro::waitFor(std::move(task));
    CHECK(!result);
    if (result.error().code != expected)
        throw std::runtime_error("expected=" + std::to_string(int(expected)) +
                                 " actual=" + std::to_string(int(result.error().code)));
}
dsvfs::StorageAssignment assignment(dsvfs::Vfs &vfs, QUuid identity, NamespaceId ns, DirectoryId root,
                                    dsvfs::Request request, bool external = false) {
    auto resources = vfs.resources();
    CHECK(resources);
    QUuid resource;
    for (auto r : resources.value())
        if (r.namespace_id == ns)
            resource = r.id;
    if (resource.isNull()) {
        // Writable test storage is always identity-owned.
        auto created = QCoro::waitFor(vfs.addResource({{}, ns, {}}, request));
        if (!created)
            created = QCoro::waitFor(vfs.addResource({{}, ns, identity}, request));
        CHECK(created);
        resource = created.value().id;
    }
    auto assigned = vfs.assignments(identity);
    CHECK(assigned);
    for (auto a : assigned.value())
        if (a.resource == resource && a.directory == root && a.allow_external_files == external)
            return a;
    return get(vfs.assignStorage({{}, identity, resource, root, external}, request));
}
struct Fixture {
    QTemporaryDir temporary;
    std::shared_ptr<NativeFileManager> backend = std::make_shared<NativeFileManager>();
    std::shared_ptr<StoragePool> pool;
    NamespaceInfo source, exports;
    QUuid identity = QUuid::createUuid();
    std::shared_ptr<std::atomic<quint64>> generation = std::make_shared<std::atomic<quint64>>(1);
    dsvfs::Request request;
    std::unique_ptr<dsvfs::Vfs> vfs;
    Fixture(dsvfs::Limits limits = {}) {
        source = get(backend->createNamespace({QUuid::createUuid(), temporary.path() + "/source"}));
        pool = std::make_shared<StoragePool>(temporary.path() + "/pool");
        get(pool->open());
        NamespaceConfig config{QUuid::createUuid(), temporary.path() + "/exports"};
        config.storage_pool = pool;
        exports = get(backend->createNamespace(config));
        request.scope = QUuid::createUuid();
        request.generation = 1;
        request.current_generation = generation;
        request.authorize = [](dsvfs::Target, dsvfs::Operation) -> Result<bool> { return true; };
        request.commit_permit = [](dsvfs::Target, dsvfs::Operation) -> Result<std::shared_ptr<void>> {
            return std::static_pointer_cast<void>(std::make_shared<int>(1));
        };
        reopen(limits);
    }
    void reopen(dsvfs::Limits limits = {}) {
        if (vfs) {
            get(vfs->shutdown());
            vfs.reset();
        }
        vfs = std::make_unique<dsvfs::Vfs>(temporary.path() + "/vfs", limits);
        get(vfs->open());
        CHECK(vfs->registerNamespace(source.config.id, backend, SymlinkPolicy::Never, identity));
        CHECK(vfs->setExportStore(backend, exports.config.id));
        assignment(*vfs, identity, source.config.id, source.root, request);
    }
    FileInfo file(QString name, QByteArray bytes, QUuid parent = {}) {
        auto pending = get(backend->createFile(
            source.config.id,
            {parent.isNull() ? source.root : parent, name, ConflictPolicy::Fail, {{quint64(bytes.size())}, {}}}));
        CHECK(get(pending.writer->writeAt(0, bytes)) == bytes.size());
        get(pending.writer->markVerified({0, quint64(bytes.size())}));
        get(pending.writer->commit());
        return get(backend->stat(source.config.id, pending.info.file_id));
    }
    dsvfs::Mount mount(QString name = "share", QUuid source_dir = {}, dsvfs::NodeId parent = {}) {
        return get(vfs->addMount(
            identity, {{}, parent, source.config.id, source_dir.isNull() ? source.root : source_dir, name, {}},
            request));
    }
    dsvfs::NodeInfo mounted(QString name = "share") {
        return get(vfs->lookup(identity, vfs->root(identity), name, request));
    }
    ~Fixture() {
        if (vfs) {
            auto ignored = QCoro::waitFor(vfs->shutdown());
        }
        QCoro::waitFor(backend->shutdown());
    }
};

QByteArray contents(const std::shared_ptr<dsvfs::CatalogExport>& catalog) {
    auto prepared = get(catalog->prepareRead());
    QByteArray bytes;
    while (quint64(bytes.size()) < catalog->size())
        bytes += get(prepared.reader->readAt(bytes.size(), 4096));
    get(prepared.reader->close());
    return bytes;
}
int named(const QByteArray& bytes, QString name) {
    int count = 0;
    for (const auto& line : bytes.split('\n'))
        if (QJsonDocument::fromJson(line).object()["name"] == name) ++count;
    return count;
}
void repeatedMountsAndAuthorization() {
    Fixture f;
    auto hello = f.file("hello", "hello");
    auto secret = f.file("secret", "secret");
    auto first = f.mount("a");
    auto first_node = f.mounted("a");
    f.mount("back", {}, first_node.id); // same backend root under its own logical descendant
    f.mount("b");                       // noncyclic repeated route
    auto scoped = f.request;
    scoped.authorize = [first, secret](dsvfs::Target target, dsvfs::Operation) -> Result<bool> {
        return target.mount != first.id || target.source != secret.id;
    };
    auto catalog = get(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, scoped));
    const auto bytes = contents(catalog);
    CHECK(named(bytes, "a") == 1 && named(bytes, "b") == 1 && named(bytes, "back") == 1);
    CHECK(named(bytes, "hello") == 1);
    CHECK(named(bytes, "secret") == 0); // second route never inherits/copies first route's descendants
    auto prepared = get(catalog->prepareRead());
    get(f.vfs->validateCatalog(std::shared_ptr<IFileReader>(std::move(prepared.reader)), catalog->size(), catalog->sha256()));
    get(catalog->close());
    auto denied = f.request;
    denied.authorize = [](dsvfs::Target, dsvfs::Operation) -> Result<bool> { return false; };
    error(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, denied), ErrorCode::MissingEntry);
    (void)hello;
}
void boundedVisitedAndRevocation() {
    dsvfs::Limits limits;
    limits.max_export_entries = 2;
    Fixture f(limits);
    auto d = get(f.backend->mkdir(f.source.config.id, f.source.root, "dir"));
    get(f.backend->mkdir(f.source.config.id, d, "nested"));
    f.mount();
    auto failed = QCoro::waitFor(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, f.request));
    CHECK(!failed && failed.error().code == ErrorCode::QuotaExceeded);
    CHECK(get(f.backend->list(f.exports.config.id, f.exports.root)).entries.isEmpty());
    {
        dsvfs::Limits depth_limits; depth_limits.max_depth = 2;
        Fixture shallow(depth_limits);
        shallow.mount("share");
        shallow.mount("back", {}, shallow.mounted().id);
        // A repeated target is terminal, but its visible record still has depth.
        error(shallow.vfs->exportCatalog(shallow.identity, shallow.vfs->root(shallow.identity), true, shallow.request),
              ErrorCode::QuotaExceeded);
    }
    AccessContext access;
    access.authorization_scope = f.request.scope;
    access.current_generation = f.generation;
    access.generation = 1;
    access.check = [](NamespaceId, QUuid, AccessOperation) -> Result<bool> { return true; };
    auto key = get(f.backend->directoryIdentity(f.source.config.id, f.source.root, access));
    CHECK(!key.isEmpty());
    ++*f.generation;
    error(f.backend->directoryIdentity(f.source.config.id, f.source.root, access), ErrorCode::MissingEntry);
}
void readOnlyLoopAndFreshIdentity() {
    QTemporaryDir temporary;
    CHECK(QDir().mkpath(temporary.path() + "/source/dir"));
    QFile file(temporary.path() + "/source/hello");
    CHECK(file.open(QIODevice::WriteOnly)); CHECK(file.write("hello") == 5); file.close();
    CHECK(::symlink("..", QFile::encodeName(temporary.path() + "/source/dir/back").constData()) == 0);
    auto backend = std::make_shared<ReadOnlyFileManager>(temporary.path() + "/index");
    ReadOnlyNamespaceConfig config;
    config.common = {QUuid::createUuid(), temporary.path() + "/source"};
    config.symlinks = SymlinkPolicy::WithinVfs;
    auto source = get(backend->createSource(config));
    bool ready = false;
    for (int turn = 0; turn < 1000; ++turn)
        if (get(backend->namespaceInfo(config.common.id)).status == NamespaceStatus::Ready) { ready = true; break; }
    CHECK(ready); // indexer itself deduplicates resolved dev/inode targets
    auto dir = get(backend->lookup(config.common.id, "dir"));
    auto native = std::make_shared<NativeFileManager>();
    auto pool = std::make_shared<StoragePool>(temporary.path() + "/pool"); get(pool->open());
    NamespaceConfig exports_config{QUuid::createUuid(), temporary.path() + "/exports"};
    exports_config.storage_pool = pool;
    auto exports = get(native->createNamespace(exports_config));
    dsvfs::Vfs vfs(temporary.path() + "/vfs"); get(vfs.open());
    CHECK(vfs.registerNamespace(config.common.id, backend, config.symlinks));
    CHECK(vfs.setExportStore(native, exports.config.id));
    auto identity = QUuid::createUuid();
    dsvfs::Request request;
    request.scope = QUuid::createUuid(); request.generation = 1;
    auto generation = std::make_shared<std::atomic<quint64>>(1); request.current_generation = generation;
    request.authorize = [](dsvfs::Target, dsvfs::Operation) -> Result<bool> { return true; };
    request.commit_permit = [](dsvfs::Target, dsvfs::Operation) -> Result<std::shared_ptr<void>> {
        return std::static_pointer_cast<void>(std::make_shared<int>(1));
    };
    assignment(vfs, identity, config.common.id, source.root, request);
    auto mount = get(vfs.addMount(identity, {{}, {}, config.common.id, source.root, "share", {}}, request));
    struct stat physical{};
    CHECK(::stat(QFile::encodeName(config.common.storage_root).constData(), &physical) == 0);
    request.resolve_target = [=](FilesystemIdentity object) -> Result<dsvfs::Target> {
        if (object.device == quint64(physical.st_dev) && object.inode == quint64(physical.st_ino))
            return dsvfs::Target{identity, mount.id, config.common.id, source.root};
        return Error{ErrorCode::MissingEntry};
    };
    request.within_subtree = [=](dsvfs::Target target, QUuid root) {
        return target.mount == mount.id && root == source.root;
    };
    auto catalog = get(vfs.exportCatalog(identity, vfs.root(identity), true, request));
    auto bytes = contents(catalog);
    CHECK(named(bytes, "hello") == 1 && named(bytes, "back") == 1 && named(bytes, "dir") == 1);
    get(catalog->close());
    AccessContext access;
    access.authorization_scope = request.scope; access.generation = 1; access.current_generation = generation;
    access.check = [](NamespaceId, QUuid, AccessOperation) -> Result<bool> { return true; };
    CHECK(!get(backend->directoryIdentity(config.common.id, dir.id, access)).isEmpty());
    CHECK(QDir().rename(config.common.storage_root + "/dir", config.common.storage_root + "/old"));
    CHECK(QDir().mkdir(config.common.storage_root + "/dir"));
    auto changed = QCoro::waitFor(backend->directoryIdentity(config.common.id, dir.id, access));
    CHECK(!changed); // stale catalog ID cannot certify a newly substituted directory
    ++*generation;
    error(backend->directoryIdentity(config.common.id, source.root, access), ErrorCode::MissingEntry);
    get(vfs.shutdown()); get(backend->shutdown()); get(native->shutdown());
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        repeatedMountsAndAuthorization(); std::cout << "PASS repeatedMountsAndAuthorization\n";
        boundedVisitedAndRevocation(); std::cout << "PASS boundedVisitedAndRevocation\n";
        readOnlyLoopAndFreshIdentity(); std::cout << "PASS readOnlyLoopAndFreshIdentity\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
