#include "ds/sharepermissions.h"
#include "../../vfs/src/work.h"
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopeGuard>
#include <logfault/logfault.h>
#include <limits>

namespace ds::core {
using namespace dsfm;
using namespace dsvfs;
namespace {
constexpr qint64 maximum_bytes = 4 << 20;
constexpr Operation operations[] = {Operation::Traverse, Operation::Inspect, Operation::List, Operation::Read,
                                    Operation::Recurse, Operation::Export};
QString text(QUuid id) { return id.toString(QUuid::WithoutBraces); }
Error invalid() { return Error{ErrorCode::InvalidArgument}; }
Result<QByteArray> readConfig(QString path) {
    QFile f(path);
    if (!f.exists()) return QByteArray{};
    if (!f.open(QIODevice::ReadOnly)) return Error{ErrorCode::IoError, int(f.error())};
    auto bytes = f.read(maximum_bytes + 1);
    if (bytes.size() > maximum_bytes) return Error{ErrorCode::QuotaExceeded};
    if (f.error() != QFile::NoError || bytes.isEmpty()) return Error{ErrorCode::IoError, int(f.error())};
    return bytes;
}
Result<void> saveConfig(QString path, QByteArray bytes) {
    const auto parent = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parent) || !QFile::setPermissions(parent, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        return Error{ErrorCode::IoError};
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || !f.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        f.write(bytes) != bytes.size() || !f.commit()) return Error{ErrorCode::IoError, int(f.error())};
    return {};
}
}
SharePermissions::SharePermissions(QString root, Host host, QObject *parent)
    : QObject(parent), root_(std::move(root)), host_(std::move(host)) {}
SharePermissions::~SharePermissions() { stop(); }
QString SharePermissions::file(QUuid identity) const { return QDir(root_).filePath(text(identity) + ".json"); }
void SharePermissions::invalidate() {
    for (auto &s : states_) ++*s.clock;
    cursor_.clear(); preview_request_.reset(); preview_.clear(); decisions_.clear(); viewed_contact_ = {}; viewed_path_.clear(); emit changed();
}
void SharePermissions::stop() { stopping_ = true; invalidate(); }
void SharePermissions::fail(Error e, QString stage) {
    LFLOG_TRACE << "Share permission validation/storage stage=" << stage << " code=" << int(e.code) << " native=" << e.native_code;
    LFLOG_DEBUG << "Share permission operation stage=" << stage << " code=" << int(e.code) << " native=" << e.native_code;
    if (e.code == ErrorCode::NamespaceBusy) {
        error_ = tr("Remove the group's dependent permission rules and mounts before deleting the group.");
        return;
    }
    if (e.code == ErrorCode::Conflict) {
        error_ = tr("Configuration changed while this edit was open. Review the current values and try again.");
        return;
    }
    error_ = tr("%1 failed (%2). Check identity/contact ownership, rule values and configuration access; refresh after concurrent changes.")
                 .arg(stage).arg(int(e.code));
}
QString SharePermissions::revision() const {
    const auto it = states_.constFind(identity_);
    return it == states_.cend() ? QString{} : identityId() + u':' + QString::number(it->clock->load());
}
QCoro::Task<Result<void>> SharePermissions::load(QUuid identity) {
    if (stopping_ || identity.isNull() || !host_.identityExists(identity)) co_return Error{ErrorCode::PermissionDenied};
    if (states_.contains(identity)) co_return Result<void>{};
    if (states_.size() >= 32) co_return Error{ErrorCode::QuotaExceeded};
    auto bytes = co_await dsvfs::detail::work<QByteArray>([path = file(identity)] { return readConfig(path); });
    if (!bytes) co_return bytes.error();
    if (stopping_ || !host_.identityExists(identity)) co_return Error{ErrorCode::PermissionDenied};
    State state;
    for (int op = 0; op <= int(Operation::Recurse); ++op) state.policy.defaults[Operation(op)] = Decision::Deny;
    if (!bytes.value().isEmpty()) {
        const auto obj = QJsonDocument::fromJson(bytes.value()).object();
        if (obj.size() != 4 || obj["version"] != 1 || QUuid(obj["identity"].toString()) != identity ||
            !obj["groups"].isArray() || !obj["policy"].isObject()) {
            LFLOG_TRACE << "Identity access configuration validation failed; expected format version=1 observed=" << obj["version"].toInt(-1);
            co_return invalid();
        }
        auto policy = PermissionPolicy::deserialize(QJsonDocument(obj["policy"].toObject()).toJson(QJsonDocument::Compact));
        auto groups = ContactGroups::deserialize(obj["groups"].toArray());
        if (!policy) co_return policy.error();
        if (!groups) co_return groups.error();
        state.policy = std::move(policy).value(); state.groups = std::move(groups).value();
        for (const auto &rule : state.policy.rules)
            if (rule.tier == Tier::Group && !state.groups.find(rule.principal)) co_return invalid();
    }
    state.clock->store(state.policy.generation);
    if (!states_.contains(identity)) {
        if (states_.size() >= 32) co_return Error{ErrorCode::QuotaExceeded};
        states_.insert(identity, std::move(state));
    }
    co_return Result<void>{};
}
void SharePermissions::selectIdentity(QUuid identity) {
    if (busy_ || stopping_) return;
    identity_ = identity; viewed_contact_ = {}; viewed_path_.clear(); contacts_.clear(); preview_.clear(); decisions_.clear(); cursor_.clear(); preview_request_.reset();
    apply("load", {}, {});
}
void SharePermissions::contactPage(QString filter, int page) {
    if (busy_ || stopping_ || filter.size() > 128 || page < 0 || page > 100000 || !host_.identityExists(identity_)) return;
    auto rows = host_.contacts(identity_, filter, page);
    if (!rows) fail(rows.error(), tr("Contact listing"));
    else {
        contacts_ = rows.value(); more_contacts_ = contacts_.size() > 100;
        if (more_contacts_) contacts_.removeLast();
    }
    emit changed();
}
QVariantList SharePermissions::groups() const {
    QVariantList result;
    auto it = states_.constFind(identity_); if (it == states_.cend()) return result;
    for (const auto &g : it->groups.entries) {
        QVariantList members;
        for (const auto &id : g.members) members.append(text(id));
        result.append(QVariantMap{{"id", text(g.id)}, {"name", g.name}, {"members", members}, {"count", g.members.size()}});
    }
    return result;
}
QVariantList SharePermissions::rules() const {
    QVariantList result;
    auto it = states_.constFind(identity_); if (it == states_.cend()) return result;
    for (const auto &r : it->policy.rules) {
        QVariantList values;
        for (auto op : operations) values.append(int(r.permissions.value(op, Decision::Unspecified)));
        result.append(QVariantMap{{"id", text(r.id)}, {"tier", int(r.tier)}, {"principal", text(r.principal)},
            {"path", r.path}, {"permissions", values}, {"denied", int(r.denied)}});
    }
    return result;
}
Result<Request> SharePermissions::requestFor(QUuid identity, QUuid contact) const {
    auto it = states_.constFind(identity);
    if (stopping_ || contact.isNull() || !host_.identityExists(identity) || !host_.contactExists(identity, contact) || it == states_.cend())
        return Error{ErrorCode::PermissionDenied};
    Request r;
    r.scope = contact; r.generation = it->clock->load(); r.current_generation = it->clock;
    r.permissions = std::make_shared<const PermissionPolicy>(it->policy);
    r.groups = it->groups.groupsFor(contact);
    // VFS applies path/mount/assignment policy in addition to this hard host gate.
    // Read-only contact operations only; ConfigureMounts and mutations never delegated.
    r.authorize = [identity](Target target, Operation op) -> Result<bool> {
        return target.identity == identity && (op == Operation::Traverse || op == Operation::Inspect ||
            op == Operation::List || op == Operation::Read || op == Operation::Recurse || op == Operation::Export);
    };
    return r;
}
void SharePermissions::edit(QString command, QVariantMap values, QString expected) { apply(command, values, expected); }
QCoro::Task<void> SharePermissions::apply(QString command, QVariantMap values, QString expected) {
    if (busy_ || stopping_) co_return;
    busy_ = true; error_.clear(); emit changed();
    const auto selected = identity_;
    auto hold = host_.hold ? host_.hold() : std::shared_ptr<void>{};
    auto guard = qScopeGuard([this] { busy_ = false; emit changed(); });
    auto execute = [&]() -> QCoro::Task<Result<void>> {
        auto loaded = co_await load(selected); if (!loaded) co_return loaded.error();
        if (command == "load") co_return Result<void>{};
        if (expected != revision()) co_return Error{ErrorCode::Conflict};
        auto candidate = states_.value(selected);
        if (candidate.clock->load() == std::numeric_limits<quint64>::max()) co_return Error{ErrorCode::QuotaExceeded};
        candidate.policy.generation = candidate.clock->load() + 1;
        const QUuid id(values["id"].toString());
        const QUuid contact(values["contact"].toString());
        if (command == "group-add" || command == "group-rename") {
            auto result = command == "group-add" ? candidate.groups.add(values["name"].toString())
                : candidate.groups.rename(id, values["name"].toString());
            if (!result) co_return result.error();
        } else if (command == "member-add" || command == "member-remove") {
            if (!host_.contactExists(selected, contact)) co_return Error{ErrorCode::PermissionDenied};
            auto result = candidate.groups.setMember(id, contact, command == "member-add");
            if (!result) co_return result.error();
        } else if (command == "group-remove") {
            if (!candidate.groups.find(id)) co_return invalid();
            for (const auto &r : candidate.policy.rules)
                if (r.tier == Tier::Group && r.principal == id) co_return Error{ErrorCode::NamespaceBusy};
            auto opened = co_await host_.open(); if (!opened) co_return opened.error();
            auto definitions = opened.value()->definitions(selected); if (!definitions) co_return definitions.error();
            for (const auto &m : definitions.value())
                if (m.tier == Tier::Group && m.principal == id) co_return Error{ErrorCode::NamespaceBusy};
            auto removed = candidate.groups.remove(id); if (!removed) co_return removed.error();
        } else if (command == "rule-save" || command == "rule-remove") {
            auto &records = candidate.policy.rules;
            auto found = std::find_if(records.begin(), records.end(), [id](const auto &r) { return r.id == id; });
            if (!id.isNull() && found == records.end()) co_return invalid();
            if (command == "rule-remove") {
                if (found == records.end()) co_return invalid(); records.erase(found);
            } else {
                PermissionRule rule;
                rule.id = id.isNull() ? QUuid::createUuid() : id;
                rule.path = values["path"].toString();
                const auto tier = values["tier"].toInt();
                if (tier < 0 || tier > 2) co_return invalid();
                rule.tier = Tier(tier); rule.principal = QUuid(values["principal"].toString());
                if ((rule.tier == Tier::Contact && !host_.contactExists(selected, rule.principal)) ||
                    (rule.tier == Tier::Group && !candidate.groups.find(rule.principal))) co_return Error{ErrorCode::PermissionDenied};
                const auto permissions = values["permissions"].toList();
                if (permissions.size() != 6) co_return invalid();
                for (int i = 0; i < 6; ++i) {
                    bool ok = false; const auto value = permissions[i].toInt(&ok);
                    if (!ok || value < 0 || value > 2) co_return invalid();
                    rule.permissions[operations[i]] = Decision(value);
                }
                bool ok = false; const auto barrier = values["denied"].toInt(&ok);
                if (!ok || (barrier != 0 && barrier != 2)) co_return invalid();
                rule.denied = Decision(barrier);
                if (found == records.end()) {
                    if (records.size() >= 4096) co_return Error{ErrorCode::QuotaExceeded};
                    records.append(rule);
                } else *found = rule;
            }
        } else co_return invalid();
        auto policy = candidate.policy.serialize(); if (!policy) co_return policy.error();
        auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"identity", text(selected)},
            {"groups", candidate.groups.serialize()}, {"policy", QJsonDocument::fromJson(policy.value()).object()}}).toJson(QJsonDocument::Compact);
        if (bytes.size() > maximum_bytes) co_return Error{ErrorCode::QuotaExceeded};
        auto saved = co_await dsvfs::detail::work<void>([path = file(selected), bytes] { return saveConfig(path, bytes); });
        if (!saved) co_return saved.error();
        // Invalidate existing requests before publishing a new immutable snapshot.
        // No event-loop yield between these operations.
        candidate.policy.generation = candidate.clock->fetch_add(1) + 1;
        states_[selected] = std::move(candidate);
        preview_request_.reset(); cursor_.clear(); preview_.clear(); decisions_.clear(); viewed_contact_ = {}; viewed_path_.clear();
        LFLOG_DEBUG << "Share permissions committed generation=" << states_[selected].policy.generation;
        auto opened = co_await host_.open();
        if (!opened) { error_ = tr("Saved; live view refresh failed. Refresh to inspect current permissions."); co_return Result<void>{}; }
        auto refreshed = co_await opened.value()->refresh();
        if (!refreshed) error_ = tr("Saved; live view refresh failed. Existing handles have been revoked.");
        co_return Result<void>{};
    };
    auto result = co_await execute();
    if (!result) fail(result.error(), tr("Permission configuration"));
    busy_ = false;
    if (command == "load" && result) contactPage({}, 0);
}
void SharePermissions::viewAs(QUuid contact, QString path) { inspect(contact, path); }
void SharePermissions::nextPreview() { inspect(viewed_contact_, viewed_path_, true); }
QCoro::Task<void> SharePermissions::inspect(QUuid contact, QString path, bool next) {
    if (busy_ || stopping_) co_return;
    busy_ = true; error_.clear(); emit changed();
    auto hold = host_.hold ? host_.hold() : std::shared_ptr<void>{};
    auto guard = qScopeGuard([this] { busy_ = false; emit changed(); });
    auto execute = [&]() -> QCoro::Task<Result<void>> {
        if (!validVirtualPath(path)) co_return invalid();
        auto request = requestFor(identity_, contact); if (!request) co_return request.error();
        auto opened = co_await host_.open(); if (!opened) co_return opened.error();
        auto &vfs = *opened.value();
        if (!next) {
            viewed_contact_ = contact; viewed_path_ = path;
            preview_.clear(); decisions_.clear(); cursor_.clear(); preview_request_.reset();
            for (auto op : operations) {
                const auto decision = request.value().permissions->explain(contact, request.value().groups, path, op);
                QVariantList trace;
                for (const auto &c : decision.contributions) {
                    QString principal = c.tier == Tier::Identity ? tr("Identity") : text(c.principal);
                    if (c.tier == Tier::Group)
                        for (const auto &g : states_[identity_].groups.entries) if (g.id == c.principal) principal = g.name + " (" + text(g.id) + ")";
                    trace.append(QVariantMap{{"rule", text(c.rule)}, {"tier", int(c.tier)}, {"principal", principal}, {"path", c.path},
                        {"permission", int(c.permission)}, {"barrier", int(c.barrier)}, {"decisive", c.decisive},
                        {"hue", int(quint8(c.principal.toRfc4122().front())) * 360 / 256},
                        {"mostSpecific", c.most_specific}, {"identityDefault", c.identity_default}});
                }
                QString reason;
                switch (decision.reason) {
                case AuthorizationReason::Granted: reason = tr("Explicit or inherited grant"); break;
                case AuthorizationReason::DefaultDenied: reason = tr("Identity default denies access"); break;
                case AuthorizationReason::PermissionDenied: reason = tr("Permission denied"); break;
                case AuthorizationReason::PathDenied: reason = tr("Subtree DENIED barrier"); break;
                case AuthorizationReason::InvalidContext: reason = tr("Invalid identity/contact context"); break;
                case AuthorizationReason::Unavailable: reason = tr("Temporarily unavailable"); break;
                }
                decisions_.append(QVariantMap{{"operation", int(op)}, {"outcome", int(decision.outcome)},
                    {"reason", reason}, {"generation", QString::number(decision.policy_generation)}, {"trace", trace}});
            }
            auto composed = co_await vfs.compose(identity_, request.value()); if (!composed) co_return composed.error();
            preview_request_ = composed.value(); preview_node_ = vfs.root(identity_);
            if (path != "/") for (const auto &part : path.sliced(1).split('/')) {
                auto child = co_await vfs.lookup(identity_, preview_node_, part, *preview_request_);
                if (!child) co_return child.error(); preview_node_ = child.value().id;
            }
            viewed_contact_ = contact; viewed_path_ = path;
        } else if (!preview_request_ || cursor_.isEmpty() || !preview_request_->current()) co_return Error{ErrorCode::PermissionDenied};
        auto page = co_await vfs.list(identity_, preview_node_, PageRequest{100, next ? cursor_ : QByteArray{}}, *preview_request_);
        if (!page) co_return page.error();
        preview_.clear();
        for (const auto &entry : page.value().entries)
            preview_.append(QVariantMap{{"name", entry.name}, {"directory", entry.type == EntryType::Directory}});
        cursor_ = page.value().next_cursor;
        co_return Result<void>{};
    };
    auto result = co_await execute();
    if (!result) { preview_.clear(); cursor_.clear(); preview_request_.reset(); fail(result.error(), tr("View as Contact")); }
}
}
