#include <QtTest>
#include <logfault/logfault.h>
#include <iostream>
#include <QTimer>
#include <QCoroTimer>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <ds/dsengine.h>
#include <ds/crypto.h>
#include <ds/managedstorage.h>
#include <ds/peer.h>
#include <ds/incomingtransfer.h>
#include <ds/outgoingtransfer.h>
#include <ds/fileshareservice.h>
#include <ds/protocolcodec.h>
#include <ds/remotecatalogs.h>
#include <dsfilemanager/readonlyfilemanager.h>
#include <algorithm>
#include <utility>
using namespace ds::core;
template<class T> T waitTask(QCoro::Task<T> task) {
    QEventLoop loop;
    std::optional<T> result;
    task.then([&](T value) { result.emplace(std::move(value)); loop.quit(); });
    if (!result) loop.exec();
    return std::move(*result);
}
#include "transfer_sender_fixture.h"
using SendingPeer = CheckpointSendingPeer;

class FileShareGetTest final : public QObject {
    Q_OBJECT
    struct Fixture {
        QTemporaryDir temp;
        std::shared_ptr<dsfm::ReadOnlyFileManager> backend;
        std::unique_ptr<dsvfs::Vfs> vfs;
        std::unique_ptr<ds::prot::FileShareService> service;
        ds::prot::FileShareService::SessionPtr session;
        ds::prot::FileShareService::Binding binding;
        File::ptr_t file;
        std::shared_ptr<SendingPeer> sender = std::make_shared<SendingPeer>();
        std::shared_ptr<std::atomic<quint64>> generation = std::make_shared<std::atomic<quint64>>(1);
        dsvfs::Request request;
        bool connected = true, enabled = true, read = true, utf8 = true;
        int staged = 0, discarded = 0;
        QByteArray bytes = QByteArray(3 * dsfm::minimum_checkpoint_interval + 37, 'x');
        QString name = QString::fromUtf8("CON:résumé%FF");
        std::function<void()> suspend;
        Fixture(FileShareGetTest &test) {
            test.contact_->setState(Contact::ACCEPTED);
            QDir().mkpath(temp.filePath("source"));
            QFile source(temp.filePath("source/") + name);
            if (!source.open(QIODevice::WriteOnly) || source.write(bytes) != bytes.size()) qFatal("source setup");
            source.close();
            backend = std::make_shared<dsfm::ReadOnlyFileManager>(temp.filePath("index"));
            dsfm::ReadOnlyNamespaceConfig config; config.common = {QUuid::createUuid(), temp.filePath("source")};
            auto ns = waitTask(backend->createSource(config)); if (!ns) qFatal("source create");
            vfs = std::make_unique<dsvfs::Vfs>(temp.filePath("vfs"));
            if (!waitTask(vfs->open()) || !vfs->registerNamespace(config.common.id, backend)) qFatal("vfs setup");
            request.scope = test.contact_->getUuid(); request.generation = 1; request.current_generation = generation;
            request.authorize = [this](dsvfs::Target, dsvfs::Operation op) -> dsfm::Result<bool> { return op != dsvfs::Operation::Read || read; };
            request.commit_permit = [](dsvfs::Target, dsvfs::Operation) -> dsfm::Result<std::shared_ptr<void>> {
                return std::static_pointer_cast<void>(std::make_shared<int>(0));
            };
            auto policy = std::make_shared<dsvfs::PermissionPolicy>();
            for (int op = 0; op <= int(dsvfs::Operation::Recurse); ++op) policy->defaults.insert(dsvfs::Operation(op), dsvfs::Decision::Allow);
            request.permissions = policy;
            const auto identity = test.identity_->getUuid();
            auto resource = waitTask(vfs->addResource({{}, config.common.id, {}}, request)); if (!resource) qFatal("resource");
            auto assignment = waitTask(vfs->assignStorage({{}, identity, resource.value().id, ns.value().root, false}, request)); if (!assignment) qFatal("assignment");
            auto mount = waitTask(vfs->addMount(identity, {{}, {}, config.common.id, ns.value().root, "share", {}}, request)); if (!mount) qFatal("mount");
            ds::prot::FileShareService::Host host;
            host.open = [this]() -> QCoro::Task<dsfm::Result<dsvfs::Vfs*>> {
                if (suspend) {
                    QTimer timer; timer.setSingleShot(true); timer.start(1);
                    std::exchange(suspend, {})(); co_await qCoro(timer).waitForTimeout();
                }
                co_return vfs.get();
            };
            host.request = [this](QUuid, QUuid) -> QCoro::Task<dsfm::Result<dsvfs::Request>> {
                // Keep the fixture's authoritative request for subsequent calls.
                co_return dsvfs::Request{request};
            };
            host.enabled = [this](QUuid) { return enabled; }; host.enable = [this](QUuid, bool value) { enabled = value; };
            service = std::make_unique<ds::prot::FileShareService>(host);
            binding = {identity, test.contact_->getUuid(), QUuid::createUuid(),
                [] { return true; }, [this] { return connected; }, [this] { return utf8; }};
            binding.stage_download = [this, &test](dsvfs::Vfs &vfs, dsvfs::NodeId node, dsvfs::Request request,
                    dsvfs::PreparedRead prepared) -> dsfm::Result<ds::prot::FileShareService::CatalogDelivery> {
                ++staged;
                auto result = test.engine_->getFileManager()->addPreparedVfsTransfer(*test.conversation_, vfs, node, request, std::move(prepared));
                if (!result) return result.error();
                file = result.value();
                return ds::prot::FileShareService::CatalogDelivery{file->getFileId(),
                    [sender = sender, file = file]() -> dsfm::Result<void> { sender->begin(file); return {}; },
                    [this, value = file] { ++discarded; value->setState(File::FS_CANCELLED); value->deleteFromDb(); },
                    [weak = std::weak_ptr<File>(file)] { auto f = weak.lock(); return f && f->getState() != File::FS_DONE &&
                        f->getState() != File::FS_CANCELLED && f->getState() != File::FS_REJECTED; }};
            };
            session = service->bind(binding);
        }
        ~Fixture() {
            service->stop(); session.reset(); service.reset();
            if (file && file->getState() != File::FS_DONE) file->setState(File::FS_CANCELLED);
            file.reset(); sender.reset();
            waitTask(vfs->shutdown()); waitTask(backend->shutdown());
        }
        ds::protocol::Envelope get(quint64 id = 1) {
            ds::protocol::VirtualPath p; const auto logical = dsfm::encodeLinuxName(name.toUtf8()).value();
            p.setPath("/share/" + (utf8 ? logical : QString::fromLatin1(logical.toUtf8().toPercentEncoding(":%"))));
            p.setEncoding(ds::protocol::VirtualPathEncodingGadget::VirtualPathEncoding(utf8 ? 1 : 2));
            ds::protocol::RequestDownload get; get.setPath(p);
            ds::protocol::Envelope e; e.setProtocolVersion(3); e.setRequestId(id); e.setRequestDownload(get); return e;
        }
        ds::protocol::Envelope run(ds::protocol::Envelope e) {
            auto response = waitTask(service->handle(session, e)); if (!response) qFatal("discarded response");
            if (!ds::prot::fileshare::validate(*response, utf8)) qFatal("invalid response"); return *response;
        }
    };
private slots:
    void initTestCase() {
        if (qEnvironmentVariableIsSet("DS_GET_TRACE")) logfault::LogManager::Instance().AddHandler(std::make_unique<logfault::StreamHandler>(std::clog, logfault::LogLevel::TRACE));
        QVERIFY(directory_.isValid());
        qputenv("DARKSPEAK_PROFILE_ROOT", directory_.path().toUtf8());
        qputenv("DARKSPEAK_TEST_SECRETSTORE_PASSPHRASE", "managed-storage-disposable-fixture");
        crypto_ = std::make_unique<ds::crypto::Crypto>();
        auto settings = std::make_unique<QSettings>(directory_.filePath("settings.ini"), QSettings::IniFormat);
        settings->setValue("dbpath", directory_.filePath("app.sqlite"));
        settings->setValue("downloadLocation", directory_.filePath("downloads"));
        QDir().mkdir(directory_.filePath("downloads"));
        engine_ = std::make_unique<DsEngine>(std::move(settings));
        auto initialized = waitTask(storage().ready());
        QVERIFY2(initialized, "Managed namespaces failed to initialize");
        IdentityData identity;
        identity.uuid = QUuid::createUuid();
        identity.name = "Storage fixture";
        identity.cert = ds::crypto::DsCert::create();
        identity.hash = identity.cert->getHash().toByteArray();
        identity.autoConnect = false;
        identity_ = engine_->getIdentityManager()->addIdentity(identity);
        auto contact = std::make_unique<ContactData>();
        contact->identity = identity_->getId();
        contact->state = Contact::ACCEPTED;
        contact->name = "Fixture peer";
        const auto cert = ds::crypto::DsCert::create();
        contact->cert = ds::crypto::DsCert::createFromPubkey(cert->getSigningPubKey().toByteArray());
        contact->address = "fixture-not-networked";
        contact->autoConnect = false;
        contact_ = engine_->getContactManager()->addContact(std::move(contact));
        conversation_ = engine_->getConversationManager()->getConversation(contact_);
    }


    void listedNodeDownload_data() {
        QTest::addColumn<bool>("utf8");
        QTest::newRow("utf8") << true;
        QTest::newRow("ascii-escaped") << false;
    }
    void listedNodeDownload() {
        QFETCH(bool, utf8);
        Fixture f(*this); f.utf8 = utf8;
        ds::protocol::VirtualPath directory;
        directory.setPath("/share");
        directory.setEncoding(ds::protocol::VirtualPathEncodingGadget::VirtualPathEncoding(utf8 ? 1 : 2));
        ds::protocol::ListDirectoryRequest list; list.setDirectory(directory); list.setPageSize(1);
        ds::protocol::Envelope e; e.setProtocolVersion(3); e.setRequestId(1); e.setListDirectoryRequest(list);
        auto authorize = f.request.authorize;
        f.request.authorize = [](dsvfs::Target, dsvfs::Operation) -> dsfm::Result<bool> { return false; };
        const auto denied = f.run(e).listDirectoryResult();
        QCOMPARE(int(denied.status()), 2); QVERIFY(denied.entries().isEmpty()); QVERIFY(!denied.hasDirectory());
        f.request.authorize = std::move(authorize);
        const auto page = f.run(e).listDirectoryResult();
        QCOMPARE(int(page.status()), 1); QVERIFY(page.complete()); QCOMPARE(page.entries().size(), 1);
        const auto entry = page.entries().first();
        const auto logical = dsfm::encodeLinuxName(f.name.toUtf8()).value();
        QCOMPARE(entry.name().value(), utf8 ? logical : QString::fromLatin1(logical.toUtf8().toPercentEncoding(":%")));
        QCOMPARE(entry.name().encoding(), directory.encoding());
        QCOMPARE(entry.nodeId().size(), 32);
        auto selected = page.directory(); selected.setPath(selected.path() + "/" + entry.name().value());
        ds::protocol::StatVirtualRequest stat; stat.setPath(selected); stat.setKnownNodeId(entry.nodeId());
        e = {}; e.setProtocolVersion(3); e.setRequestId(2); e.setStatVirtualRequest(stat);
        const auto metadata = f.run(e).statVirtualResult();
        QCOMPARE(int(metadata.status()), 1); QCOMPARE(metadata.entry().nodeId(), entry.nodeId());
        QCOMPARE(metadata.entry().size(), quint64(f.bytes.size()));
        auto download = f.get(3); auto get = download.requestDownload();
        get.setPath(metadata.path()); get.setKnownNodeId(metadata.entry().nodeId());
        get.setExpectedSize(metadata.entry().size());
        get.setExpectedSha256(QCryptographicHash::hash(f.bytes, QCryptographicHash::Sha256));
        download.setRequestDownload(get);
        // A visible LIST/STAT node is no authority to read its contents.
        f.read = false;
        QCOMPARE(int(f.run(download).requestDownloadResult().status()), 2); QCOMPARE(f.staged, 0);
        f.read = true; download.setRequestId(4);
        QCOMPARE(int(f.run(download).requestDownloadResult().status()), 1);
        QVERIFY(f.sender->sent.isEmpty()); QVERIFY(f.service->publishCatalog(f.session, 4));
        QTRY_COMPARE_WITH_TIMEOUT(f.file->getState(), File::FS_DONE, 10000);
        QCOMPARE(f.sender->sent, f.bytes);
        QCOMPARE(QCryptographicHash::hash(f.sender->sent, QCryptographicHash::Sha256), f.file->getHash());
        auto progress = waitTask(storage().transferProgress().load(f.file->checkpointKey().value())); QVERIFY(progress);
        QVERIFY(progress.value().complete); QCOMPARE(progress.value().verified_prefix, quint64(f.bytes.size()));
        QFile source(f.temp.filePath("source/") + f.name); QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write("changed"), qint64(7)); source.close();
        download.setRequestId(5);
        const auto changed = f.run(download).requestDownloadResult();
        QVERIFY(int(changed.status()) != 1); QVERIFY(changed.transferId().isEmpty()); QCOMPARE(f.staged, 1);
    }
    void hostileRequestsBeforeStorage() {
        Fixture f(*this); f.utf8 = false;
        bool opened = false; f.suspend = [&] { opened = true; };
        for (const auto &value : {QString("/share/../private"), QString("/share/%2F"),
                                 QString("/share/%FF"), QString("/share/%c3%a9"),
                                 QString("/") + QString(4096, 'x')}) {
            auto e = f.get(); auto get = e.requestDownload(); auto p = get.path(); p.setPath(value);
            get.setPath(p); e.setRequestDownload(get);
            const auto result = f.run(e).requestDownloadResult();
            QCOMPARE(int(result.status()), 4); QVERIFY(result.transferId().isEmpty());
            ds::protocol::ListDirectoryRequest list; list.setDirectory(p); list.setPageSize(1);
            e = {}; e.setProtocolVersion(3); e.setRequestId(1); e.setListDirectoryRequest(list);
            const auto page = f.run(e).listDirectoryResult();
            QCOMPARE(int(page.status()), 4); QVERIFY(page.entries().isEmpty()); QVERIFY(!page.hasDirectory());
        }
        QVERIFY(!opened); QCOMPARE(f.staged, 0);
        QVERIFY(!ds::prot::fileshare::validateWire(QByteArray(ds::prot::fileshare::max_control_bytes + 1, 'x')));
        // Malformed UTF-8 must fail in the raw codec before QString replacement or dispatch.
        auto wire = ds::prot::codec::serialize(f.get());
        wire[wire.size() - 1] = char(0xc3);
        ds::protocol::Envelope decoded;
        QVERIFY(!ds::prot::codec::deserialize(wire, decoded));
    }
    void verifiedDownload() {
        contact_->setState(Contact::ACCEPTED); Fixture f(*this);
        auto e = f.get(); auto get = e.requestDownload(); get.setExpectedSize(f.bytes.size());
        get.setExpectedSha256(QCryptographicHash::hash(f.bytes, QCryptographicHash::Sha256)); e.setRequestDownload(get);
        auto response = f.run(e).requestDownloadResult(); QCOMPARE(int(response.status()), 1);
        QCOMPARE(response.transferId(), f.file->getFileId()); QVERIFY(f.sender->sent.isEmpty());
        QCOMPARE(f.file->getName(), FileManager::transferName(dsfm::encodeLinuxName(f.name.toUtf8()).value())); QVERIFY(f.file->hasPreparedSource());
        QVERIFY(f.service->publishCatalog(f.session, 1));
        QTRY_COMPARE_WITH_TIMEOUT(f.file->getState(), File::FS_DONE, 10000);
        QCOMPARE(f.sender->sent, f.bytes); QCOMPARE(QCryptographicHash::hash(f.sender->sent, QCryptographicHash::Sha256), f.file->getHash());
        auto progress = waitTask(storage().transferProgress().load(f.file->checkpointKey().value())); QVERIFY(progress);
        QVERIFY(progress.value().complete); QCOMPARE(progress.value().verified_prefix, quint64(f.bytes.size()));
    }
    void checkpointResumeOnReauthenticatedConnection() {
        Fixture f(*this); QCOMPARE(int(f.run(f.get()).requestDownloadResult().status()), 1);
        f.sender->interrupt_after_checkpoint = true;
        QVERIFY(f.service->publishCatalog(f.session, 1));
        QTRY_COMPARE_WITH_TIMEOUT(f.file->getState(), File::FS_PAUSED, 10000);
        auto progress = waitTask(storage().transferProgress().load(f.file->checkpointKey().value())); QVERIFY(progress);
        QVERIFY(progress.value().verified_prefix > 0); QVERIFY(!progress.value().complete);
        f.connected = false;
        auto binding = f.binding; binding.connection = QUuid::createUuid(); binding.current = [] { return true; };
        auto reconnected = f.service->bind(binding); QVERIFY(reconnected);
        auto sender = std::make_shared<SendingPeer>(); sender->resume(f.file, f.sender->sent, progress.value());
        QTRY_COMPARE_WITH_TIMEOUT(f.file->getState(), File::FS_DONE, 10000);
        QVERIFY(sender->resumed_offset > 0); QCOMPARE(sender->sent, f.bytes);
        QCOMPARE(QCryptographicHash::hash(sender->sent, QCryptographicHash::Sha256), f.file->getHash());
    }
    void admissionFailures_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<int>("status");
        QTest::newRow("read-denied") << QString("deny") << 2;
        QTest::newRow("changed-size") << QString("size") << 8;
        QTest::newRow("changed-hash") << QString("hash") << 8;
        QTest::newRow("stale-node") << QString("node") << 8;
        QTest::newRow("directory") << QString("directory") << 2;
        QTest::newRow("outside") << QString("outside") << 2;
    }
    void admissionFailures() {
        QFETCH(QString, mode); QFETCH(int, status); Fixture f(*this); auto e = f.get(); auto get = e.requestDownload();
        if (mode == "deny") f.read = false;
        if (mode == "size") get.setExpectedSize(0);
        if (mode == "hash") get.setExpectedSha256(QByteArray(32, 'z'));
        if (mode == "node") get.setKnownNodeId(QByteArray(32, 'z'));
        if (mode == "directory" || mode == "outside") { auto p = get.path(); p.setPath(mode == "directory" ? "/share" : "/private"); get.setPath(p); }
        e.setRequestDownload(get); auto response = f.run(e).requestDownloadResult();
        QCOMPARE(int(response.status()), status); QVERIFY(response.transferId().isEmpty()); QCOMPARE(f.staged, 0);
    }
    void revokeDuringReadAndResume() {
        Fixture f(*this); QCOMPARE(int(f.run(f.get()).requestDownloadResult().status()), 1);
        QVERIFY(f.service->publishCatalog(f.session, 1));
        // A real sender must stop before its next chunk when READ is withdrawn.
        // Use another staged source so the first small asynchronous sender drains.
        QTRY_COMPARE_WITH_TIMEOUT(f.file->getState(), File::FS_DONE, 10000);
        f.sender = std::make_shared<SendingPeer>();
        QCOMPARE(int(f.run(f.get(2)).requestDownloadResult().status()), 1);
        f.sender->mutate = [&] { f.read = false; };
        QVERIFY(f.service->publishCatalog(f.session, 2));
        QTRY_COMPARE_WITH_TIMEOUT(f.file->getState(), File::FS_PAUSED, 10000);
        QVERIFY(!f.sender->final_sent); QVERIFY(f.sender->sent.size() < f.bytes.size());
        QVERIFY(!waitTask(f.file->prepareOutgoingRead()));
    }
    void preparedResumeAndSourceChange() {
        Fixture f(*this); QCOMPARE(int(f.run(f.get()).requestDownloadResult().status()), 1);
        auto first = waitTask(f.file->prepareOutgoingRead()); QVERIFY(first); auto lease = first.value().resume_lease;
        QVERIFY(waitTask(first.value().reader->close()));
        dsfm::ReadPreparationOptions options; options.expected_sha256 = f.file->getHash(); options.resume_lease = lease;
        f.connected = false;
        QVERIFY(!waitTask(f.file->prepareOutgoingRead(options)));
        auto binding = f.binding; binding.connection = QUuid::createUuid(); binding.current = [] { return true; };
        binding.contact = QUuid::createUuid(); auto unrelated = f.service->bind(binding); QVERIFY(unrelated);
        QVERIFY(!waitTask(f.file->prepareOutgoingRead(options))); // another contact cannot authorize resume
        binding.contact = contact_->getUuid();
        auto reconnected = f.service->bind(binding); QVERIFY(reconnected);
        auto resumed = waitTask(f.file->prepareOutgoingRead(options)); QVERIFY(resumed);
        auto bytes = waitTask(resumed.value().reader->readAt(100, 123)); QVERIFY(bytes); QCOMPARE(bytes.value(), f.bytes.mid(100,123));
        QVERIFY(waitTask(resumed.value().reader->close()));
        QFile source(f.temp.filePath("source/") + f.name); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("changed"); source.close();
        QVERIFY(!waitTask(f.file->prepareOutgoingRead(options)));
    }
    void cancellationAndPublicationFences() {
        Fixture f(*this); QCOMPARE(int(f.run(f.get()).requestDownloadResult().status()), 1);
        ds::protocol::Envelope cancel; cancel.setProtocolVersion(3); cancel.setRequestId(2);
        ds::protocol::CancelFileShareRequest c; c.setRequestToCancel(1); cancel.setCancelFileShareRequest(c);
        QCOMPARE(int(f.run(cancel).cancelFileShareResult().status()), 1); QCOMPARE(f.discarded, 1);
        QVERIFY(f.service->publishCatalog(f.session, 1)); QVERIFY(f.sender->sent.isEmpty());
        QCOMPARE(int(f.run(f.get(3)).requestDownloadResult().status()), 1);
        f.read = false;
        QVERIFY(!f.service->publishCatalog(f.session, 3)); QCOMPARE(f.discarded, 2); QVERIFY(f.sender->sent.isEmpty());
        f.read = true;
        QCOMPARE(int(f.run(f.get(4)).requestDownloadResult().status()), 1);
        f.service->setEnabled(identity_->getUuid(), false); QCOMPARE(f.discarded, 3); QVERIFY(f.sender->sent.isEmpty());
    }
    void disconnectDuringAdmission() {
        Fixture f(*this); f.suspend = [&] { f.connected = false; };
        QVERIFY(!waitTask(f.service->handle(f.session, f.get()))); QCOMPARE(f.staged, 0);
    }
    void boundedAndEscapedAdmission() {
        Fixture f(*this); f.utf8 = false;
        QCOMPARE(int(f.run(f.get()).requestDownloadResult().status()), 1);
        QVERIFY(f.service->publishCatalog(f.session, 1));
        QTRY_COMPARE_WITH_TIMEOUT(f.file->getState(), File::FS_DONE, 10000); QCOMPARE(f.sender->sent, f.bytes);
        f.sender = std::make_shared<SendingPeer>();
        QCOMPARE(int(f.run(f.get(2)).requestDownloadResult().status()), 1);
        QCOMPARE(int(f.run(f.get(3)).requestDownloadResult().status()), 6); // one unoffered result per session
        auto binding = f.binding; binding.connection = QUuid::createUuid();
        auto second = f.service->bind(binding); QVERIFY(second);
        auto response = waitTask(f.service->handle(second, f.get(4))); QVERIFY(response);
        QCOMPARE(int(response->requestDownloadResult().status()), 1);
        binding.connection = QUuid::createUuid(); auto third = f.service->bind(binding); QVERIFY(third);
        response = waitTask(f.service->handle(third, f.get(5))); QVERIFY(response);
        QCOMPARE(int(response->requestDownloadResult().status()), 6); // two GETs per contact across connections
    }
    void cleanupTestCase() {
        auto joined = engine_->transfers().shutdown(); QTRY_VERIFY_WITH_TIMEOUT(joined.isReady(), 10000);
        QVERIFY(waitTask(storage().shutdown()));
        conversation_.reset(); engine_.reset();
    }
private:
    ManagedStorage& storage() { return engine_->getFileManager()->storage(); }
    QTemporaryDir directory_;
    std::unique_ptr<ds::crypto::Crypto> crypto_;
    std::unique_ptr<DsEngine> engine_;
    Identity* identity_ = nullptr;
    Contact* contact_ = nullptr;
    Conversation::ptr_t conversation_;
};
QTEST_GUILESS_MAIN(FileShareGetTest)
#include "fileshare_get_test.moc"
