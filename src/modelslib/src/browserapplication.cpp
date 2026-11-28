#include <ds/filebrowsermodel.h>
#include <ds/dsengine.h>
#include <ds/managedstorage.h>
#include <ds/contactmanager.h>
#include <ds/peer.h>
#include <logfault/logfault.h>

namespace ds::models {
FileBrowserModel::Factory applicationBrowserFactory(core::ManagedStorage &storage, prot::FileShareService &responses) {
    return [storage = QPointer(&storage), responses = QPointer(&responses)](FileBrowserModel::Location location,
            BrowserScope scope) -> dsfm::Result<std::shared_ptr<BrowseProvider>> {
        if (!storage || !responses) return dsfm::Error{dsfm::ErrorCode::ShuttingDown};
        auto identity_exists = [](QUuid id) {
            return core::DsEngine::instance().getIdentityManager()->identityFromUuid(id) != nullptr;
        };
        if (!identity_exists(scope.identity)) return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
        if (location == FileBrowserModel::Local) {
            if (!scope.contact.isNull()) return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
            LocalBrowseProvider::Host host{
                [storage]() -> std::shared_ptr<void> {
                    return storage ? std::make_shared<core::ManagedStorage::Operation>(storage->holdOperation()) : nullptr;
                }, [storage, identity_exists](QUuid id) { return storage && identity_exists(id); },
                [storage]() -> QCoro::Task<dsfm::Result<dsvfs::Vfs*>> {
                    if (!storage) co_return dsfm::Error{dsfm::ErrorCode::ShuttingDown};
                    co_return co_await storage->openVfs();
                }};
            return std::static_pointer_cast<BrowseProvider>(std::make_shared<LocalBrowseProvider>(scope.identity, std::move(host)));
        }
        try {
            auto contact = core::DsEngine::instance().getContactManager()->getContact(scope.contact);
            if (!contact || !contact->getIdentity() || contact->getIdentity()->getUuid() != scope.identity)
                return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
            return RemoteBrowseProvider::forContact(std::dynamic_pointer_cast<prot::Peer>(contact->currentConnection()), contact, *responses);
        } catch (const std::exception &) {
            // Unknown local IDs are an ordinary failed binding, never a log of contact data.
            LFLOG_TRACE << "FileShare browser binding failed stage=contact-lookup";
            return dsfm::Error{dsfm::ErrorCode::PermissionDenied};
        }
    };
}
}
