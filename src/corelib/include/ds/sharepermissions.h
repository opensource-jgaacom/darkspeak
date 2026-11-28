#pragma once
#include <ds/contactgroups.h>
#include <dsvfs/vfs.h>
#include <QVariantList>

namespace ds::core {
// Privileged local editor; requestFor() is the sole contact policy adapter.
class SharePermissions final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy MEMBER busy_ NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error_ NOTIFY changed)
    Q_PROPERTY(QString identityId READ identityId NOTIFY changed)
    Q_PROPERTY(QString revision READ revision NOTIFY changed)
    Q_PROPERTY(QVariantList contacts MEMBER contacts_ NOTIFY changed)
    Q_PROPERTY(bool moreContacts MEMBER more_contacts_ NOTIFY changed)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)
    Q_PROPERTY(QVariantList rules READ rules NOTIFY changed)
    Q_PROPERTY(QVariantList decisions MEMBER decisions_ NOTIFY changed)
    Q_PROPERTY(QString viewedContact READ viewedContact NOTIFY changed)
    Q_PROPERTY(QString viewedPath READ viewedPath NOTIFY changed)
    Q_PROPERTY(QVariantList preview MEMBER preview_ NOTIFY changed)
    Q_PROPERTY(bool morePreview READ morePreview NOTIFY changed)
public:
    struct Host {
        std::function<std::shared_ptr<void>()> hold;
        std::function<bool(QUuid)> identityExists;
        std::function<bool(QUuid, QUuid)> contactExists;
        std::function<dsfm::Result<QVariantList>(QUuid, QString, int)> contacts;
        std::function<QCoro::Task<dsfm::Result<dsvfs::Vfs*>>()> open;
    };
    SharePermissions(QString root, Host host, QObject *parent = nullptr);
    ~SharePermissions() override;
    Q_INVOKABLE void selectIdentity(QUuid identity);
    Q_INVOKABLE void contactPage(QString filter, int page);
    Q_INVOKABLE void edit(QString command, QVariantMap values, QString expected);
    Q_INVOKABLE void viewAs(QUuid contact, QString path);
    Q_INVOKABLE void nextPreview();
    // Also usable by future server callers, after load(identity). No UI state is authority.
    QCoro::Task<dsfm::Result<void>> load(QUuid identity);
    dsfm::Result<dsvfs::Request> requestFor(QUuid identity, QUuid contact) const;
    QCoro::Task<void> apply(QString command, QVariantMap values, QString expected);
    QCoro::Task<void> inspect(QUuid contact, QString path, bool next = false);
    void stop();
    Q_INVOKABLE void invalidate(); // Contact/identity lifecycle; fail closed for existing handles.
    QString identityId() const { return identity_.toString(QUuid::WithoutBraces); }
    QString revision() const;
    QVariantList groups() const;
    QVariantList rules() const;
    QString viewedContact() const { return viewed_contact_.isNull() ? QString{} : viewed_contact_.toString(QUuid::WithoutBraces); }
    QString viewedPath() const { return viewed_path_; }
    bool morePreview() const { return !cursor_.isEmpty(); }
    enum { Traverse = int(dsvfs::Operation::Traverse), Inspect = int(dsvfs::Operation::Inspect),
           List = int(dsvfs::Operation::List), Read = int(dsvfs::Operation::Read),
           Export = int(dsvfs::Operation::Export), Recurse = int(dsvfs::Operation::Recurse) };
signals:
    void changed();
private:
    struct State {
        dsvfs::PermissionPolicy policy;
        ContactGroups groups;
        std::shared_ptr<std::atomic<quint64>> clock = std::make_shared<std::atomic<quint64>>(1);
    };
    QString file(QUuid identity) const;
    void fail(dsfm::Error error, QString stage);
    QString root_, error_;
    Host host_;
    QUuid identity_, viewed_contact_;
    QString viewed_path_;
    QHash<QUuid, State> states_;
    QVariantList contacts_, decisions_, preview_;
    QByteArray cursor_, preview_node_;
    std::optional<dsvfs::Request> preview_request_;
    bool busy_ = false, stopping_ = false, more_contacts_ = false;
};
}
