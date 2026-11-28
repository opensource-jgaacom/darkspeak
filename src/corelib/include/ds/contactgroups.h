#pragma once
#include <dsfilemanager/filemanager.h>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace ds::core {
// General identity-scoped contact membership, independent of FileShare rules.
// The host validates contact ownership before adding members. Serialized together
// with dependent configuration by the host's atomic configuration transaction.
struct ContactGroups {
    struct Group { QUuid id; QString name; QSet<QUuid> members; };
    QVector<Group> entries;
    QSet<QUuid> groupsFor(QUuid contact) const {
        QSet<QUuid> result;
        for (const auto &g : entries) if (g.members.contains(contact)) result.insert(g.id);
        return result;
    }
    Group *find(QUuid id) {
        for (auto &g : entries) if (g.id == id) return &g;
        return nullptr;
    }
    dsfm::Result<void> add(QString name) {
        if (name.trimmed().isEmpty() || name.size() > 128) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        if (entries.size() >= 128) return dsfm::Error{dsfm::ErrorCode::QuotaExceeded};
        entries.append({QUuid::createUuid(), name.trimmed(), {}});
        return {};
    }
    dsfm::Result<void> rename(QUuid id, QString name) {
        auto group = find(id);
        if (!group || name.trimmed().isEmpty() || name.size() > 128) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        group->name = name.trimmed();
        return {};
    }
    dsfm::Result<void> setMember(QUuid id, QUuid contact, bool member) {
        auto group = find(id);
        if (!group || contact.isNull()) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        qsizetype count = 0;
        for (const auto &entry : entries) count += entry.members.size();
        if (member) {
            if (count >= 4096 && !group->members.contains(contact)) return dsfm::Error{dsfm::ErrorCode::QuotaExceeded};
            group->members.insert(contact);
        } else group->members.remove(contact);
        return {};
    }
    dsfm::Result<void> remove(QUuid id) {
        if (!find(id)) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        entries.removeIf([id](const auto &group) { return group.id == id; });
        return {};
    }
    QJsonArray serialize() const {
        QJsonArray result;
        for (const auto &g : entries) {
            QJsonArray members;
            for (const auto &id : g.members) members.append(id.toString(QUuid::WithoutBraces));
            result.append(QJsonObject{{"id", g.id.toString(QUuid::WithoutBraces)}, {"name", g.name}, {"members", members}});
        }
        return result;
    }
    static dsfm::Result<ContactGroups> deserialize(QJsonArray records) {
        using namespace dsfm;
        if (records.size() > 128) return dsfm::Error{ErrorCode::QuotaExceeded};
        ContactGroups result;
        QSet<QUuid> ids;
        qsizetype member_count = 0;
        for (const auto &record : records) {
            const auto obj = record.toObject();
            const QUuid id(obj["id"].toString());
            const auto name = obj["name"].toString();
            if (obj.size() != 3 || id.isNull() || ids.contains(id) || name.trimmed().isEmpty() || name.size() > 128 ||
                !obj["members"].isArray() || obj["members"].toArray().size() > 4096)
                return dsfm::Error{ErrorCode::InvalidArgument};
            member_count += obj["members"].toArray().size();
            if (member_count > 4096) return dsfm::Error{ErrorCode::QuotaExceeded};
            Group group{id, name, {}};
            for (const auto &member : obj["members"].toArray()) {
                const QUuid contact(member.toString());
                if (contact.isNull() || group.members.contains(contact)) return dsfm::Error{ErrorCode::InvalidArgument};
                group.members.insert(contact);
            }
            ids.insert(id); result.entries.append(group);
        }
        return result;
    }
};
}
