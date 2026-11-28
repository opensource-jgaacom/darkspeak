#include <dsvfs/vfs.h>
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
class MemoryReader final : public IFileReader {
  public:
    explicit MemoryReader(QByteArray bytes) : bytes_(std::move(bytes)) {}
    QCoro::Task<Result<QByteArray>> readAt(quint64 offset, qsizetype count, CancellationToken cancel) override {
        if (cancel.cancelled())
            co_return Error{ErrorCode::Cancelled};
        if (count < 0)
            co_return Error{ErrorCode::InvalidArgument};
        if (offset >= quint64(bytes_.size()))
            co_return QByteArray{};
        co_return bytes_.sliced(qsizetype(offset), std::min(count, bytes_.size() - qsizetype(offset)));
    }
    QCoro::Task<Result<void>> close() override { co_return Result<void>{}; }

  private:
    QByteArray bytes_;
};

QByteArray bytesOf(std::shared_ptr<dsvfs::CatalogExport> catalog) {
    auto prepared = get(catalog->prepareRead());
    auto bytes = get(prepared.reader->readAt(0, qsizetype(catalog->size())));
    CHECK(quint64(bytes.size()) == catalog->size());
    get(prepared.reader->close());
    return bytes;
}
void validate(Fixture &f, QByteArray bytes, std::optional<bool> encoding = {}) {
    get(f.vfs->validateCatalog(std::make_shared<MemoryReader>(bytes), bytes.size(),
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), {}, encoding));
}
void reject(Fixture &f, QByteArray bytes, std::optional<bool> encoding = {}) {
    CHECK(!QCoro::waitFor(f.vfs->validateCatalog(std::make_shared<MemoryReader>(bytes), bytes.size(),
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), {}, encoding)));
}
QByteArray modify(QByteArray bytes, int index, std::function<void(QJsonObject &)> action) {
    auto lines = bytes.split('\n');
    auto object = QJsonDocument::fromJson(lines[index]).object();
    action(object);
    lines[index] = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return lines.join('\n');
}
void portableSuccessPrivacyAndIntegrity() {
    Fixture f;
    f.file(QString::fromUtf8("é%file"), "hello");
    f.mount();
    auto first = get(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, f.request));
    auto bytes = bytesOf(first);
    validate(f, bytes, true);
    auto second = get(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, f.request));
    auto again = bytesOf(second);
    CHECK(bytes != again); // Per-export references cannot correlate private routes.
    CHECK(!bytes.contains(f.vfs->root(f.identity).toHex()));
    for (const auto &line : bytes.split('\n')) {
        if (line.isEmpty()) continue;
        auto object = QJsonDocument::fromJson(line).object();
        CHECK(!object.contains("capabilities") && !object.contains("symlinks") && !object.contains("revision"));
        CHECK(!object.contains("modified") && !object.contains("hashState"));
        if (object.contains("id")) CHECK(object["id"].toString().size() == 64);
        if (object["type"] == 1) CHECK(!object.contains("size") && !object.contains("modifiedUtcDay") && !object.contains("sha256"));
    }
    auto wrong = first->sha256(); wrong[0] ^= 1;
    error(f.vfs->validateCatalog(std::make_shared<MemoryReader>(bytes), bytes.size(), wrong), ErrorCode::IntegrityError);
    reject(f, bytes, false);
    reject(f, modify(bytes, 0, [](auto &o) { o["version"] = 3; }));
    reject(f, modify(bytes, 0, [](auto &o) { o.remove("encoding"); }));
    reject(f, modify(bytes, 1, [](auto &o) { o["capabilities"] = 0; }));
    reject(f, modify(bytes, 1, [](auto &o) { o["id"] = QString(34, '1'); }));
    reject(f, bytes + bytes.mid(bytes.indexOf('\n') + 1));
    reject(f, bytes.chopped(1));
    auto duplicate_name = modify(bytes, 3, [](auto &o) { o["id"] = QString(64, 'f'); }).split('\n')[3];
    reject(f, bytes + duplicate_name + '\n');
    reject(f, modify(bytes, 3, [](auto &o) { o["size"] = "18446744073709551615"; }));
    reject(f, modify(bytes, 3, [](auto &o) { o["modifiedUtcDay"] = "2932897"; }));
    reject(f, modify(bytes, 3, [](auto &o) { o["name"] = QString::fromUtf8("\xF3\xA0\x80\x81"); }));
    get(first->close()); get(second->close());
}
void asciiAndRevocation() {
    Fixture f;
    f.file(QString::fromUtf8("é%file"), "hello");
    f.mount(); f.request.allow_utf8 = false;
    auto catalog = get(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, f.request));
    auto bytes = bytesOf(catalog);
    CHECK(bytes.contains("%C3%A9%25file")); validate(f, bytes, false);
    reject(f, bytes, true);
    for (QString name : {QStringLiteral("%2F"), QStringLiteral("%41"), QStringLiteral("%c3%A9"),
                         QStringLiteral("%FF"), QStringLiteral("%00"), QStringLiteral("%"), QString::fromUtf8("é")})
        reject(f, modify(bytes, 3, [name](auto &o) { o["name"] = name; }));
    ++*f.generation;
    CHECK(!catalog->isAuthorized());
    error(catalog->prepareRead(), ErrorCode::MissingEntry);
    get(catalog->close());
}
void fixtureAndBounds() {
    Fixture f;
    QFile fixture(QStringLiteral(VFS_FIXTURES) + "/catalog-v2.jsonl"); CHECK(fixture.open(QIODevice::ReadOnly));
    auto bytes = fixture.readAll(); validate(f, bytes, true);
    CHECK(!QCoro::waitFor(f.vfs->validateCatalog(std::make_shared<MemoryReader>(bytes), bytes.size(),
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), {}, true, true)));
    get(f.vfs->validateCatalog(std::make_shared<MemoryReader>(bytes), bytes.size(),
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), {}, true, false));
    reject(f, modify(bytes, 0, [](auto &o) { o["encoding"] = "unknown"; }));
    reject(f, modify(bytes, 1, [](auto &o) { o["name"] = ".."; }));
    auto cancel = CancellationToken{}; cancel.cancel();
    error(f.vfs->validateCatalog(std::make_shared<MemoryReader>(bytes), bytes.size(),
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), cancel), ErrorCode::Cancelled);
}
void effectiveExportPolicy() {
    using namespace dsvfs;
    Fixture f;
    f.file("visible", "payload"); f.mount();
    auto mounted = f.mounted();
    auto policy = std::make_shared<PermissionPolicy>();
    for (int op = 0; op <= int(Operation::Recurse); ++op)
        policy->defaults[Operation(op)] = Decision::Allow;
    CHECK(policy->validate());
    f.request.permissions = policy;
    const auto reservations = get(f.pool->reservations(f.exports.config.id)).size();
    for (auto operation : {Operation::Export, Operation::List}) {
        policy->rules = {{"/share", Tier::Contact, f.request.scope, {{operation, Decision::Deny}}}};
        error(f.vfs->exportCatalog(f.identity, mounted.id, true, f.request), ErrorCode::MissingEntry);
        CHECK(get(f.pool->reservations(f.exports.config.id)).size() == reservations);
        policy->rules = {{"/", Tier::Contact, f.request.scope, {{operation, Decision::Deny}}}};
        error(f.vfs->exportCatalog(f.identity, f.vfs->root(f.identity), true, f.request), ErrorCode::MissingEntry);
        CHECK(get(f.pool->reservations(f.exports.config.id)).size() == reservations);
    }
    policy->rules.clear();
    auto catalog = get(f.vfs->exportCatalog(f.identity, mounted.id, true, f.request));
    CHECK(catalog->isAuthorized());
    policy->rules = {{"/share", Tier::Contact, f.request.scope, {{Operation::Export, Decision::Deny}}}};
    CHECK(!catalog->isAuthorized());
    error(catalog->prepareRead(), ErrorCode::MissingEntry);
    get(catalog->close());
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        portableSuccessPrivacyAndIntegrity();
        asciiAndRevocation();
        fixtureAndBounds();
        effectiveExportPolicy();
        std::cout << "portable catalog: 4 groups passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
