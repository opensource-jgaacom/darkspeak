#include "ds/sharemounts.h"
#include <QScopeGuard>
#include <logfault/logfault.h>

namespace ds::core {
using namespace dsfm;
using namespace dsvfs;
namespace {
QString text(QUuid id) { return id.toString(QUuid::WithoutBraces); }
Error invalid() { return Error{ErrorCode::InvalidArgument}; }
}
ShareMounts::ShareMounts(Host host, QObject *parent) : QObject(parent), host_(std::move(host)) {}
Request ShareMounts::request(bool inspect) const {
    Request r;
    r.scope = identity_; r.generation = 1;
    r.current_generation = std::make_shared<std::atomic<quint64>>(1);
    r.authorize = [identity = identity_, inspect](Target t, Operation op) -> Result<bool> {
        return (t.identity.isNull() || t.identity == identity) &&
            (op == Operation::ConfigureMounts || (inspect && (op == Operation::List || op == Operation::Recurse || op == Operation::Inspect || op == Operation::Traverse)));
    };
    r.commit_permit = [identity = identity_](Target t, Operation op) -> Result<std::shared_ptr<void>> {
        if ((!t.identity.isNull() && t.identity != identity) || op != Operation::ConfigureMounts)
            return Error{ErrorCode::PermissionDenied};
        return std::static_pointer_cast<void>(std::make_shared<int>(0));
    };
    return r;
}
void ShareMounts::selectIdentity(QUuid identity) {
    if (busy_ || stopping_) return;
    identity_ = identity; namespace_ = {}; directory_ = {}; ancestors_.clear();
    sources_.clear(); assignments_.clear(); mounts_.clear(); directories_.clear(); preview_.clear();
    directory_cursor_.clear(); preview_cursor_.clear(); preview_node_.clear(); directory_name_.clear();
    draft_path_ = QStringLiteral("/share"); emit draftChanged(); refresh();
}
void ShareMounts::refresh() { run(Refresh); }
void ShareMounts::selectSource(QString id) { run(SelectSource, id); }
void ShareMounts::enterDirectory(QString id) { run(EnterDirectory, id); }
void ShareMounts::up() { run(Up); }
void ShareMounts::nextDirectories() { run(NextDirectories); }
void ShareMounts::assign() { run(Assign); }
void ShareMounts::unassign(QString id) { run(Unassign, id); }
void ShareMounts::addMount(QString id) { run(AddMount, id); }
void ShareMounts::moveMount(QString id) { run(MoveMount, id); }
void ShareMounts::removeMount(QString id) { run(RemoveMount, id); }
void ShareMounts::browse(QString id) { run(Browse, id); }
void ShareMounts::nextPreview() { run(NextPreview); }
QCoro::Task<Result<void>> ShareMounts::directoryPage(QByteArray cursor) {
    if (!host_.backend() || namespace_.isNull() || directory_.isNull()) co_return invalid();
    auto page = co_await host_.backend()->list(namespace_, directory_, PageRequest{100, cursor});
    if (!page) co_return page.error();
    directories_.clear();
    for (const auto &e : page.value().entries)
        if (e.type == EntryType::Directory && e.status == EntryStatus::Available)
            directories_.append(QVariantMap{{"id", text(e.id)}, {"name", e.name}});
    directory_cursor_ = page.value().next_cursor;
    co_return Result<void>{};
}
QCoro::Task<Result<void>> ShareMounts::previewPage(Vfs &vfs, QByteArray cursor) {
    auto composed = co_await vfs.compose(identity_, request(true));
    if (!composed) co_return composed.error();
    auto page = co_await vfs.list(identity_, preview_node_, PageRequest{100, cursor}, composed.value());
    if (!page) co_return page.error();
    preview_.clear();
    for (const auto &e : page.value().entries)
        preview_.append(QVariantMap{{"id", QString::fromLatin1(e.id.toHex())}, {"name", e.name},
            {"directory", e.type == EntryType::Directory}});
    preview_cursor_ = page.value().next_cursor;
    co_return Result<void>{};
}
QCoro::Task<Result<void>> ShareMounts::validatePlacement(Vfs &vfs, QString path, QUuid moving) {
    if (path.size() > 4096 || !validVirtualPath(path) || path == "/") co_return invalid();
    // Reject existing source children and synthetic navigation nodes as well as
    // configured mounts. Backend changes after this check still fail closed in
    // the production resolver, which detects ambiguous composed names.
    if (!moving.isNull()) {
        auto definitions = vfs.definitions(identity_); if (!definitions) co_return definitions.error();
        for (const auto &m : definitions.value())
            if (m.id == moving && m.path == path) co_return Result<void>{};
    }
    auto composed = co_await vfs.compose(identity_, request(true));
    if (!composed) co_return composed.error();
    auto node = vfs.root(identity_);
    for (const auto &part : path.sliced(1).split('/')) {
        auto child = co_await vfs.lookup(identity_, node, part, composed.value());
        if (!child) {
            if (child.error().code == ErrorCode::MissingEntry) co_return Result<void>{};
            co_return child.error();
        }
        node = child.value().id;
    }
    co_return Error{ErrorCode::Conflict};
}
QCoro::Task<Result<void>> ShareMounts::reload(Vfs &vfs) {
    auto backend = host_.backend();
    if (!backend) co_return Error{ErrorCode::StorageUnavailable};
    auto namespaces = co_await backend->namespaces();
    if (!namespaces) co_return namespaces.error();
    if (namespaces.value().size() > 128) co_return Error{ErrorCode::QuotaExceeded};
    sources_.clear();
    for (const auto &ns : namespaces.value())
        sources_.append(QVariantMap{{"id", text(ns.config.id)}, {"name", ns.config.storage_root}});
    auto assigned = vfs.assignments(identity_); auto definitions = vfs.definitions(identity_);
    if (!assigned) co_return assigned.error();
    if (!definitions) co_return definitions.error();
    assignments_.clear(); mounts_.clear();
    auto resources = vfs.resources(); if (!resources) co_return resources.error();
    for (const auto &a : assigned.value()) {
        QString source = text(a.resource);
        for (const auto &r : resources.value()) if (r.id == a.resource)
            for (const auto &ns : namespaces.value()) if (ns.config.id == r.namespace_id) source = ns.config.storage_root;
        assignments_.append(QVariantMap{{"id", text(a.id)}, {"name", source + " — directory " + text(a.directory)}});
    }
    for (const auto &m : definitions.value())
        if (m.tier == Tier::Identity)
            mounts_.append(QVariantMap{{"id", text(m.id)}, {"path", m.path}, {"assignment", text(m.assignment)}});
    preview_node_ = vfs.root(identity_);
    co_return co_await previewPage(vfs);
}
QCoro::Task<void> ShareMounts::run(Command operation, QString id) {
    if (busy_ || stopping_) co_return;
    busy_ = true; error_.clear(); emit changed();
    auto guard = qScopeGuard([this] { busy_ = false; emit changed(); });
    auto hold = host_.hold ? host_.hold() : std::shared_ptr<void>{};
    bool committed = false;
    auto execute = [&]() -> QCoro::Task<Result<void>> {
        if (identity_.isNull() || !host_.identityExists(identity_)) co_return Error{ErrorCode::PermissionDenied};
        auto opened = co_await host_.open(); if (!opened) co_return opened.error();
        auto &vfs = *opened.value();
        if (stopping_ || !host_.identityExists(identity_)) co_return Error{ErrorCode::PermissionDenied};
        if (operation == Refresh) co_return co_await reload(vfs);
        if (operation == SelectSource) {
            const QUuid ns(id);
            bool known = false;
            for (const auto &row : sources_) known |= QUuid(row.toMap()["id"].toString()) == ns;
            if (!known || !host_.backend()) co_return invalid();
            auto info = co_await host_.backend()->namespaceInfo(ns); if (!info) co_return info.error();
            namespace_ = ns; directory_ = info.value().root; directory_name_ = QStringLiteral("/"); ancestors_.clear();
            co_return co_await directoryPage();
        }
        if (operation == EnterDirectory) {
            for (const auto &row : directories_) {
                auto entry = row.toMap();
                if (entry["id"].toString() != id) continue;
                if (ancestors_.size() >= 64) co_return Error{ErrorCode::QuotaExceeded};
                ancestors_.append({directory_, directory_name_}); directory_ = QUuid(id); directory_name_ = (directory_name_ == "/" ? directory_name_ : directory_name_ + u'/') + entry["name"].toString();
                co_return co_await directoryPage();
            }
            co_return invalid();
        }
        if (operation == Up) {
            if (ancestors_.isEmpty()) co_return invalid();
            auto parent = ancestors_.takeLast(); directory_ = parent.first; directory_name_ = parent.second;
            co_return co_await directoryPage();
        }
        if (operation == NextDirectories) {
            if (directory_cursor_.isEmpty()) co_return invalid();
            co_return co_await directoryPage(directory_cursor_);
        }
        if (operation == Assign) {
            if (namespace_.isNull() || directory_.isNull()) co_return invalid();
            auto resources = vfs.resources(); if (!resources) co_return resources.error();
            QUuid resource;
            for (const auto &r : resources.value()) if (r.namespace_id == namespace_ && r.owner_identity.isNull()) resource = r.id;
            if (resource.isNull()) {
                auto added = co_await vfs.addResource({{}, namespace_, {}}, request());
                if (!added) co_return added.error(); resource = added.value().id;
            }
            auto assigned = co_await vfs.assignStorage({{}, identity_, resource, directory_, false}, request());
            if (!assigned) co_return assigned.error();
        } else if (operation >= Unassign && operation <= RemoveMount) {
            const auto &rows = operation <= AddMount ? assignments_ : mounts_;
            bool known = false;
            for (const auto &row : rows) known |= row.toMap()["id"].toString() == id;
            if (!known || QUuid(id).isNull()) co_return invalid();
            if (operation == Unassign) {
                auto definitions = vfs.definitions(identity_); if (!definitions) co_return definitions.error();
                for (const auto &m : definitions.value())
                    if (m.assignment == QUuid(id)) co_return Error{ErrorCode::NamespaceBusy};
                auto r = co_await vfs.withdrawAssignment(QUuid(id), request()); if (!r) co_return r.error(); }
            if (operation == AddMount) {
                auto valid = co_await validatePlacement(vfs, draft_path_); if (!valid) co_return valid.error();
                auto r = co_await vfs.defineMount({{}, identity_, QUuid(id), {}, draft_path_, Tier::Identity, {}}, request());
                if (!r) co_return r.error();
            }
            if (operation == MoveMount) {
                auto valid = co_await validatePlacement(vfs, draft_path_, QUuid(id)); if (!valid) co_return valid.error();
                auto r = co_await vfs.relocateDefinition(QUuid(id), draft_path_, request()); if (!r) co_return r.error();
            }
            if (operation == RemoveMount) { auto r = co_await vfs.removeDefinition(QUuid(id), request()); if (!r) co_return r.error(); }
        } else if (operation == Browse) {
            if (id.isEmpty()) preview_node_ = vfs.root(identity_);
            else {
                bool known = false;
                for (const auto &row : preview_) known |= row.toMap()["id"].toString() == id && row.toMap()["directory"].toBool();
                if (!known) co_return invalid();
                preview_node_ = QByteArray::fromHex(id.toLatin1());
            }
            co_return co_await previewPage(vfs);
        } else if (operation == NextPreview) {
            if (preview_cursor_.isEmpty()) co_return invalid();
            co_return co_await previewPage(vfs, preview_cursor_);
        } else co_return invalid();
        committed = true;
        co_return co_await reload(vfs);
    };
    auto result = co_await execute();
    if (!result) {
        LFLOG_DEBUG << "Identity share administration operation=" << operation << " failed, code=" << int(result.error().code) << " native=" << result.error().native_code;
        if (committed) {
            error_ = tr("Configuration saved, but the view could not be refreshed. Refresh to inspect the committed settings.");
            co_return;
        }
        switch (result.error().code) {
        case ErrorCode::Conflict:
            error_ = tr("The virtual path already exists or conflicts with another entry. Choose another path."); break;
        case ErrorCode::NamespaceBusy:
            error_ = tr("This assignment has dependent mounts. Remove those mounts explicitly before unassigning."); break;
        case ErrorCode::PermissionDenied:
            error_ = tr("The identity or source is unavailable, or the directory is outside the assignment. Refresh and select an allowed source."); break;
        case ErrorCode::InvalidArgument:
            error_ = tr("Select a source/assignment and enter an absolute virtual path without empty, dot or parent components."); break;
        case ErrorCode::QuotaExceeded:
            error_ = tr("The configured resource, mount or directory-depth limit was reached. Remove unused configuration or choose a shallower directory."); break;
        default:
            error_ = tr("Storage or the view is unavailable (%1). Check source availability and disk permissions, then refresh and retry.").arg(int(result.error().code)); break;
        }
    }
}
}
