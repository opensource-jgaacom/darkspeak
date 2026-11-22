#include "ds/readonlyresources.h"
#include "../../vfs/src/work.h"
#include <logfault/logfault.h>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <sys/stat.h>
#include <algorithm>

namespace ds::core {
namespace {
using namespace dsfm;
using dsvfs::detail::work;
constexpr int maximum_sources = 32;
constexpr qint64 maximum_registry = 256 * 1024;
QString idText(QUuid id) { return id.toString(QUuid::WithoutBraces); }
bool below(QStringView path, QStringView root) {
    return path == root || root == u"/" || (path.startsWith(root) && path.size() > root.size() && path[root.size()] == u'/');
}
struct Location { QString canonical, device, physical; quint64 dev = 0, ino = 0; };
// mountinfo's root identifies bind-mounted subtrees on the same filesystem.
// Canonical path comparison alone misses these aliases. An unreadable or
// ambiguous mount table fails closed instead of claiming disjointness.
Result<QVector<Location>> locations(QStringList paths) {
    QFile mounts(QStringLiteral("/proc/self/mountinfo"));
    if (!mounts.open(QIODevice::ReadOnly)) return Error{ErrorCode::StorageUnavailable};
    const auto bytes = mounts.read(4 * 1024 * 1024 + 1);
    if (bytes.size() > 4 * 1024 * 1024) return Error{ErrorCode::QuotaExceeded};
    const auto lines = bytes.split('\n');
    if (lines.size() > 8192) return Error{ErrorCode::QuotaExceeded};
    auto decode = [](QByteArray value) {
        value.replace("\\040", " "); value.replace("\\011", "\t");
        value.replace("\\012", "\n"); value.replace("\\134", "\\");
        return QFile::decodeName(value);
    };
    QVector<Location> result;
    for (const auto &path : paths) {
        const auto canonical = QFileInfo(path).canonicalFilePath();
        struct stat st{};
        if (canonical.isEmpty() || ::stat(QFile::encodeName(canonical).constData(), &st) || !S_ISDIR(st.st_mode))
            return Error{ErrorCode::StorageUnavailable};
        Location location{canonical, {}, {}, quint64(st.st_dev), quint64(st.st_ino)};
        qsizetype longest = -1;
        for (const auto &line : lines) {
            const auto fields = line.split(' ');
            if (fields.size() < 10) continue;
            const auto mount = decode(fields[4]);
            if (!below(canonical, mount) || mount.size() < longest) continue;
            if (mount.size() == longest) return Error{ErrorCode::Conflict};
            longest = mount.size();
            location.device = QString::fromLatin1(fields[2]);
            location.physical = QDir::cleanPath(decode(fields[3]) + u'/' + canonical.mid(mount == "/" ? 1 : mount.size()));
        }
        if (longest < 0) return Error{ErrorCode::StorageUnavailable};
        result.append(location);
    }
    return result;
}
Result<QSet<int>> conflicts(QStringList paths) {
    auto resolved = locations(paths); if (!resolved) return resolved.error();
    QSet<int> result;
    for (int i = 0; i < resolved.value().size(); ++i) {
        const auto &a = resolved.value()[i];
        for (int j = 0; j < i; ++j) {
            const auto &b = resolved.value()[j];
            if ((a.dev == b.dev && a.ino == b.ino) || below(a.canonical, b.canonical) || below(b.canonical, a.canonical)
                || (a.device == b.device && (below(a.physical, b.physical) || below(b.physical, a.physical)))) {
                result.insert(i); result.insert(j);
            }
        }
    }
    return result;
}
bool validPath(const QString &path) {
    return !path.isEmpty() && path.size() <= 4096 && path.isValidUtf16()
        && !path.contains(QChar::Null) && QDir::isAbsolutePath(path);
}
QString problem(ErrorCode code) {
    switch (code) {
    case ErrorCode::Conflict: return ReadOnlyResources::tr("Source overlaps another registered directory. Resolve the conflicting roots.");
    case ErrorCode::PermissionDenied: return ReadOnlyResources::tr("Source is excluded/private or inaccessible. Choose an accessible non-private directory.");
    case ErrorCode::Busy: case ErrorCode::NamespaceBusy: return ReadOnlyResources::tr("Storage is in use. Resolve assignments, mounts or active operations, then retry.");
    case ErrorCode::InvalidArgument: case ErrorCode::InvalidName: return ReadOnlyResources::tr("Check the name, absolute directory paths and supported policy settings.");
    case ErrorCode::QuotaExceeded: return ReadOnlyResources::tr("The bounded storage configuration or indexing limit was reached.");
    default: return ReadOnlyResources::tr("Storage is unavailable or its configuration cannot be saved. Check the directory, disk and permissions, then retry.");
    }
}
}
ReadOnlyResources::ReadOnlyResources(QString registry, Host host, QObject *parent)
    : QAbstractListModel(parent), registry_(std::move(registry)), host_(std::move(host)) {
    timer_.setInterval(2000);
    connect(&timer_, &QTimer::timeout, this, [this] { if (loaded_ && !busy_ && !failed_load_) inspect(); });
}
ReadOnlyResources::~ReadOnlyResources() = default;
int ReadOnlyResources::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : sources_.size(); }
QHash<int, QByteArray> ReadOnlyResources::roleNames() const {
    return {{ResourceId, "resourceId"}, {Name, "sourceName"}, {Path, "sourcePath"}, {Status, "sourceStatus"},
        {Problem, "sourceError"}, {Symlinks, "symlinkPolicy"}, {Verification, "verificationPolicy"}, {Exclusions, "exclusions"}};
}
QVariant ReadOnlyResources::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= sources_.size()) return {};
    const auto &source = sources_[index.row()];
    switch (role) {
    case ResourceId: return idText(source.config.common.id);
    case Name: return source.name;
    case Path: return source.config.common.storage_root;
    case Status: return source.status;
    case Problem: return source.problem;
    case Symlinks: return int(source.config.symlinks);
    case Verification: return int(source.config.verification);
    case Exclusions: return source.effective_exclusions;
    default: return {};
    }
}
bool ReadOnlyResources::begin() {
    if (busy_ || stopping_) return false;
    busy_ = true; error_.clear(); error_code_ = -1; emit stateChanged(); return true;
}
void ReadOnlyResources::end() { busy_ = false; emit stateChanged(); }
void ReadOnlyResources::fail(Error error, QString message) {
    LFLOG_DEBUG << "Read-only storage administration failed, code=" << int(error.code);
    error_ = std::move(message); error_code_ = int(error.code); emit stateChanged();
}
void ReadOnlyResources::stop() { stopping_ = true; timer_.stop(); }
void ReadOnlyResources::refresh() { if (!loaded_ || failed_load_) load(); else inspect(); }
void ReadOnlyResources::add() { create(); }
void ReadOnlyResources::remove(QString id) { erase(QUuid(id)); }
void ReadOnlyResources::discardDraft() {
    draft_name_.clear(); draft_path_.clear(); draft_exclusions_.clear(); draft_symlinks_ = 0; draft_verification_ = 0; emit draftChanged();
}
QCoro::Task<Result<void>> ReadOnlyResources::save(const QVector<Source> &sources) {
    QJsonArray rows;
    for (const auto &source : sources) rows.append(QJsonObject{{"id", idText(source.config.common.id)}, {"name", source.name},
        {"path", source.config.common.storage_root}, {"symlinks", int(source.config.symlinks)},
        {"verification", int(source.config.verification)}, {"exclusions", QJsonArray::fromStringList(source.config.exclusions.directory_roots)}});
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"sources", rows}}).toJson(QJsonDocument::Compact);
    co_return co_await work<void>([path = registry_, bytes]() -> Result<void> {
        if (bytes.size() > maximum_registry) return Error{ErrorCode::QuotaExceeded};
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
            || file.write(bytes) != bytes.size() || !file.commit()) return Error{ErrorCode::IoError};
        return {};
    });
}
QCoro::Task<void> ReadOnlyResources::load() {
    if (!begin()) co_return;
    auto lifetime = host_.hold ? host_.hold() : nullptr;
    auto finish = qScopeGuard([this] { end(); });
    auto loaded = co_await work<QVector<Source>>([path = registry_]() -> Result<QVector<Source>> {
        QFile file(path);
        if (!file.exists()) return QVector<Source>{};
        if (!file.open(QIODevice::ReadOnly) || file.size() > maximum_registry) return Error{ErrorCode::Corruption};
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.read(maximum_registry + 1), &error);
        const auto object = document.object();
        if (error.error != QJsonParseError::NoError || object["version"].toInt(-1) != 1 || !object["sources"].isArray()
            || object["sources"].toArray().size() > maximum_sources) return Error{ErrorCode::Corruption};
        QVector<Source> sources;
        QSet<QUuid> ids;
        for (const auto &value : object["sources"].toArray()) {
            const auto row = value.toObject(); Source source;
            source.name = row["name"].toString(); source.config.common.id = QUuid(row["id"].toString());
            source.config.common.storage_root = row["path"].toString();
            const auto symlinks = row["symlinks"].toInt(-1), verification = row["verification"].toInt(-1);
            if (source.name.trimmed().isEmpty() || source.name.size() > 128 || source.config.common.id.isNull()
                || ids.contains(source.config.common.id) || !validPath(source.config.common.storage_root)
                || symlinks < 0 || symlinks > 2 || verification < 0 || verification > 2 || !row["exclusions"].isArray()
                || row["exclusions"].toArray().size() > 64) return Error{ErrorCode::Corruption};
            ids.insert(source.config.common.id);
            source.config.symlinks = SymlinkPolicy(symlinks); source.config.verification = ReadVerification(verification);
            for (const auto &excluded : row["exclusions"].toArray()) {
                if (!validPath(excluded.toString())) return Error{ErrorCode::Corruption};
                source.config.exclusions.directory_roots.append(excluded.toString());
            }
            sources.append(source);
        }
        return sources;
    });
    if (!loaded) { failed_load_ = true; fail(loaded.error(), tr("Cannot load the versioned storage registry. Restore or repair it before adding sources.")); co_return; }
    beginResetModel(); sources_ = std::move(loaded).value(); endResetModel();
    loaded_ = true; failed_load_ = false;
    finish.dismiss(); end();
    if (!stopping_) timer_.start();
    co_await inspect();
}
QCoro::Task<void> ReadOnlyResources::inspect() {
    if (!begin()) co_return;
    auto lifetime = host_.hold ? host_.hold() : nullptr;
    auto finish = qScopeGuard([this] { end(); });
    QStringList paths; for (const auto &source : sources_) paths.append(source.config.common.storage_root);
    // An unverifiable relationship quarantines all roots until it is resolved.
    auto checked = co_await work<QSet<int>>([paths] { return conflicts(paths); });
    for (int i = 0; i < sources_.size(); ++i) {
        auto &source = sources_[i];
        if (!checked || checked.value().contains(i)) {
            if (source.opened) {
                auto closed = co_await host_.close(source.config.common.id);
                if (!closed) { fail(closed.error(), problem(closed.error().code)); }
                else source.opened = false;
            }
            source.status = tr("Conflict / unavailable");
            source.problem = problem(checked ? ErrorCode::Conflict : checked.error().code);
        } else {
            auto info = source.opened ? co_await host_.info(source.config.common.id) : co_await host_.open(source.config, false);
            if (!info) { source.status = tr("Unavailable"); source.problem = problem(info.error().code); }
            else {
                source.opened = true;
                source.status = info.value().status == NamespaceStatus::Ready ? tr("Ready") : info.value().status == NamespaceStatus::Updating ? tr("Indexing") : tr("Failed");
                source.problem = info.value().readiness_error ? problem(info.value().readiness_error->code) : QString{};
                auto config = co_await host_.config(source.config.common.id);
                if (config) source.effective_exclusions = config.value().exclusions.directory_roots.join('\n');
            }
        }
        emit dataChanged(index(i), index(i));
    }
}
QCoro::Task<void> ReadOnlyResources::create() {
    if (!begin()) co_return;
    auto lifetime = host_.hold ? host_.hold() : nullptr;
    auto finish = qScopeGuard([this] { end(); });
    if (!loaded_ || failed_load_) { fail(Error{ErrorCode::Busy}, tr("Refresh the storage registry before adding a source.")); co_return; }
    Source source; source.name = draft_name_.trimmed();
    source.config.common.id = QUuid::createUuid();
    source.config.common.storage_root = QDir::cleanPath(draft_path_);
    source.config.exclusions.directory_roots = draft_exclusions_.split('\n', Qt::SkipEmptyParts);
    if (sources_.size() >= maximum_sources || source.name.isEmpty() || source.name.size() > 128 || source.name.contains(QChar::Null)
        || !validPath(draft_path_) || draft_symlinks_ < 0 || draft_symlinks_ > 2 || draft_verification_ < 0 || draft_verification_ > 2
        || source.config.exclusions.directory_roots.size() > 64) { fail(Error{ErrorCode::InvalidArgument}, problem(ErrorCode::InvalidArgument)); co_return; }
    for (auto &path : source.config.exclusions.directory_roots) {
        path = path.trimmed(); if (!validPath(path)) { fail(Error{ErrorCode::InvalidArgument}, problem(ErrorCode::InvalidArgument)); co_return; }
    }
    source.config.symlinks = SymlinkPolicy(draft_symlinks_); source.config.verification = ReadVerification(draft_verification_);
    QStringList paths; for (const auto &existing : sources_) paths.append(existing.config.common.storage_root);
    paths.append(source.config.common.storage_root);
    auto checked = co_await work<QSet<int>>([paths] { return conflicts(paths); });
    if (!checked || !checked.value().isEmpty()) { const auto error = checked ? Error{ErrorCode::Conflict} : checked.error(); fail(error, problem(error.code)); co_return; }
    auto canonical = co_await work<QString>([path = source.config.common.storage_root]() -> Result<QString> {
        const auto canonical = QFileInfo(path).canonicalFilePath();
        if (canonical.isEmpty()) return Error{ErrorCode::StorageUnavailable};
        return canonical;
    });
    if (!canonical) { fail(canonical.error(), problem(canonical.error().code)); co_return; }
    source.config.common.storage_root = canonical.value();
    auto opened = co_await host_.open(source.config, true);
    if (!opened) {
        // A rejected create can have allocated a partial private catalog. The
        // fresh local UUID cannot refer to another registered resource.
        auto cleaned = co_await host_.remove(source.config.common.id);
        if (!cleaned) LFLOG_DEBUG << "Partial read-only registration cleanup failed, code=" << int(cleaned.error().code);
        fail(opened.error(), problem(opened.error().code)); co_return;
    }
    source.opened = true; source.status = tr("Indexing");
    auto effective = co_await host_.config(source.config.common.id);
    if (effective) source.effective_exclusions = effective.value().exclusions.directory_roots.join('\n');
    auto next = sources_; next.append(source);
    auto saved = co_await save(next);
    if (!saved) {
        auto removed = co_await host_.remove(source.config.common.id);
        Q_UNUSED(removed);
        fail(saved.error(), problem(saved.error().code)); co_return;
    }
    beginInsertRows({}, sources_.size(), sources_.size()); sources_.append(source); endInsertRows();
    LFLOG_DEBUG << "Read-only source registration committed";
    discardDraft();
}
QCoro::Task<void> ReadOnlyResources::erase(QUuid id) {
    if (!begin()) co_return;
    auto lifetime = host_.hold ? host_.hold() : nullptr;
    auto finish = qScopeGuard([this] { end(); });
    auto found = std::find_if(sources_.begin(), sources_.end(), [id](const auto &source) { return source.config.common.id == id; });
    if (id.isNull() || found == sources_.end()) { fail(Error{ErrorCode::MissingEntry}, tr("Refresh and select an existing source.")); co_return; }
    const auto row = int(std::distance(sources_.begin(), found));
    auto removed = co_await host_.remove(id);
    if (!removed) {
        fail(removed.error(), removed.error().code == ErrorCode::Conflict
            ? tr("The private index contains retained recovery data. Resolve it before retrying removal.") : problem(removed.error().code));
        co_return;
    }
    sources_[row].opened = false;
    auto next = sources_; next.removeAt(row);
    auto saved = co_await save(next);
    if (!saved) { sources_[row].status = tr("Unavailable"); fail(saved.error(), tr("Source was closed, but unregistering could not be saved. Retry removal.")); co_return; }
    beginRemoveRows({}, row, row); sources_.removeAt(row); endRemoveRows();
    LFLOG_DEBUG << "Read-only source unregistered; source data retained";
}
} // namespace ds::core
