#include <ds/remotecatalogs.h>
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <utility>
using namespace dsfm;
using namespace ds::prot;
using namespace ds::protocol;
namespace {
constexpr auto mime = "application/vnd.darkspeak.vfs-catalog+jsonl";
class Reader : public IFileReader {
public:
    QByteArray bytes;
    std::function<void()> during_read;
    explicit Reader(QByteArray data) : bytes(std::move(data)) {}
    QCoro::Task<Result<QByteArray>> readAt(quint64 offset, qsizetype maximum, CancellationToken = {}) override {
        if (during_read) std::exchange(during_read, {})();
        co_return bytes.mid(qsizetype(offset), maximum);
    }
    QCoro::Task<Result<void>> close() override { co_return Result<void>{}; }
};
struct Fixture {
    QTemporaryDir temp;
    dsvfs::Vfs vfs{temp.filePath("vfs")};
    QUuid identity = QUuid::createUuid(), contact = QUuid::createUuid(), connection = QUuid::createUuid();
    bool live = true;
    quint64 next = 10;
    int dropped = 0;
    RemoteCatalogs catalogs{[this](QUuid, QUuid, QByteArray) { ++dropped; }};
    QByteArray transfer = QUuid::createUuid().toRfc4122(), bytes, digest;
    Fixture() {
        if (!QCoro::waitFor(vfs.open())) qFatal("VFS open");
        QFile file(QStringLiteral(CATALOG_FIXTURE));
        if (!file.open(QIODevice::ReadOnly)) qFatal("fixture open");
        bytes = file.readAll(); bytes.replace("\"recursive\":false", "\"recursive\":true");
        digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    }
    RemoteCatalogs::Context context(bool utf8 = true) {
        return {identity, contact, connection, utf8, [this] { return live; },
            [this](Envelope request) { if (!request.listDirectoryRequest().recursive()) qFatal("wrong request"); return next++; }};
    }
    Envelope response(quint64 id, bool utf8 = true) {
        VirtualPath path; path.setPath("/share"); path.setEncoding(VirtualPathEncodingGadget::VirtualPathEncoding(utf8 ? 1 : 2));
        ListDirectoryResult list; list.setDirectory(path); list.setStatus(FileShareStatusGadget::FileShareStatus::FILESHARE_OK);
        list.setCatalogTransferId(transfer); list.setCatalogFormatVersion(2);
        Envelope e; e.setProtocolVersion(3); e.setResponseToRequestId(id); e.setListDirectoryResult(list); return e;
    }
    bool admit(QUuid conn = {}) {
        return catalogs.admit(identity, contact, conn.isNull() ? connection : conn, transfer, mime, "catalog.jsonl", bytes.size(), digest);
    }
    RemoteCatalogs::Cached artifact() { return {QUuid::createUuid(), QUuid::createUuid(), quint64(bytes.size()), digest}; }
    void begin(bool utf8 = true) {
        auto id = catalogs.request(context(utf8), "/share"); if (!id) qFatal("request");
        catalogs.response(connection, response(id.value(), utf8));
        if (!admit()) qFatal("admit");
    }
};
}
class RemoteCatalogsTest : public QObject {
    Q_OBJECT
private slots:
    void successPublishesOnlyAfterCompleteValidation() {
        Fixture f; QSignalSpy ready(&f.catalogs, &RemoteCatalogs::catalogReady);
        f.begin(); QVERIFY(!f.catalogs.cached(f.identity, f.contact, "/share"));
        auto artifact = f.artifact();
        QVERIFY(QCoro::waitFor(f.catalogs.complete(f.identity, f.contact, f.connection, f.transfer, artifact,
            std::make_shared<Reader>(f.bytes), f.vfs)));
        QCOMPARE(ready.count(), 1);
        auto cached = f.catalogs.cached(f.identity, f.contact, "/share");
        QVERIFY(cached); QCOMPARE(cached->file, artifact.file);
        QVERIFY(!f.catalogs.cached(QUuid::createUuid(), f.contact, "/share"));
        f.live = false; QVERIFY(!f.catalogs.cached(f.identity, f.contact, "/share")); QCOMPARE(f.dropped, 1);
    }
    void refreshReplacesOnlyAfterValidation() {
        Fixture f; f.begin(); auto first = f.artifact();
        QVERIFY(QCoro::waitFor(f.catalogs.complete(f.identity, f.contact, f.connection, f.transfer, first,
            std::make_shared<Reader>(f.bytes), f.vfs)));
        f.transfer = QUuid::createUuid().toRfc4122(); f.begin();
        QCOMPARE(f.catalogs.cached(f.identity, f.contact, "/share")->file, first.file);
        auto second = f.artifact();
        QVERIFY(QCoro::waitFor(f.catalogs.complete(f.identity, f.contact, f.connection, f.transfer, second,
            std::make_shared<Reader>(f.bytes), f.vfs)));
        QCOMPARE(f.catalogs.cached(f.identity, f.contact, "/share")->file, second.file);
        QCOMPARE(f.dropped, 1);
    }
    void connectionRequestAndOfferIsolation() {
        Fixture f; QVERIFY(!f.admit());
        auto id = f.catalogs.request(f.context(), "/share"); QVERIFY(id);
        f.catalogs.response(QUuid::createUuid(), f.response(id.value())); QVERIFY(!f.admit());
        f.catalogs.response(f.connection, f.response(id.value() + 1)); QVERIFY(!f.admit());
        f.catalogs.response(f.connection, f.response(id.value()));
        QVERIFY(!f.admit(QUuid::createUuid()));
        QVERIFY(!f.catalogs.admit(f.identity, QUuid::createUuid(), f.connection, f.transfer, mime, "catalog.jsonl", f.bytes.size(), f.digest));
        QVERIFY(!f.catalogs.admit(f.identity, f.contact, f.connection, f.transfer, mime, "catalog.jsonl", 256ULL*1024*1024+1, f.digest));
        QVERIFY(f.admit());
        QVERIFY(!f.catalogs.admit(f.identity, f.contact, f.connection, f.transfer, mime, "catalog.jsonl", f.bytes.size()+1, f.digest));
        QVERIFY(f.admit()); // exact retransmission stays within existing authority
    }
    void corruptionAndPresentationNeverPublish() {
        for (const auto ascii : {false, true}) {
            Fixture f; QSignalSpy ready(&f.catalogs, &RemoteCatalogs::catalogReady);
            f.begin(!ascii);
            auto bytes = f.bytes;
            if (!ascii) bytes[bytes.size()-2] = ' ';
            QVERIFY(!QCoro::waitFor(f.catalogs.complete(f.identity, f.contact, f.connection, f.transfer, f.artifact(),
                std::make_shared<Reader>(bytes), f.vfs)));
            QVERIFY(!f.catalogs.cached(f.identity, f.contact, "/share")); QCOMPARE(ready.count(), 0); QCOMPARE(f.dropped, 1);
        }
    }
    void revocationDuringValidationAndCancellation() {
        Fixture f; f.begin(); auto reader = std::make_shared<Reader>(f.bytes);
        reader->during_read = [&] { f.live = false; };
        QVERIFY(!QCoro::waitFor(f.catalogs.complete(f.identity, f.contact, f.connection, f.transfer, f.artifact(), reader, f.vfs)));
        QVERIFY(!f.catalogs.cached(f.identity, f.contact, "/share")); QCOMPARE(f.dropped, 1);
        Fixture cancelled; cancelled.begin(); cancelled.catalogs.failed(cancelled.identity, cancelled.contact, cancelled.transfer);
        QVERIFY(!QCoro::waitFor(cancelled.catalogs.complete(cancelled.identity, cancelled.contact, cancelled.connection,
            cancelled.transfer, cancelled.artifact(), std::make_shared<Reader>(cancelled.bytes), cancelled.vfs)));
        QCOMPARE(cancelled.dropped, 1);
    }
    void requestCancellationAndRecursiveModeMismatch() {
        Fixture f; bool cancelled = false;
        auto context = f.context();
        context.send = [&](Envelope e) {
            if (e.hasCancelFileShareRequest()) { cancelled = e.cancelFileShareRequest().requestToCancel() == 71; return quint64(72); }
            return quint64(71);
        };
        auto id = f.catalogs.request(context, "/share"); QVERIFY(id);
        f.catalogs.cancel(f.connection, id.value()); QVERIFY(cancelled);
        f.catalogs.response(f.connection, f.response(id.value())); QVERIFY(!f.admit());
        Fixture nonrecursive;
        nonrecursive.bytes.replace("\"recursive\":true", "\"recursive\":false");
        nonrecursive.digest = QCryptographicHash::hash(nonrecursive.bytes, QCryptographicHash::Sha256);
        nonrecursive.begin();
        QVERIFY(!QCoro::waitFor(nonrecursive.catalogs.complete(nonrecursive.identity, nonrecursive.contact, nonrecursive.connection,
            nonrecursive.transfer, nonrecursive.artifact(), std::make_shared<Reader>(nonrecursive.bytes), nonrecursive.vfs)));
        QVERIFY(!nonrecursive.catalogs.cached(nonrecursive.identity, nonrecursive.contact, "/share"));
    }
    void boundsRefusalAndStop() {
        Fixture f;
        auto one = f.catalogs.request(f.context(), "/share"), two = f.catalogs.request(f.context(), "/share");
        QVERIFY(one); QVERIFY(two); QVERIFY(!f.catalogs.request(f.context(), "/share"));
        ListDirectoryResult refused; refused.setStatus(FileShareStatusGadget::FileShareStatus::FILESHARE_RECURSION_REFUSED);
        Envelope e; e.setProtocolVersion(3); e.setResponseToRequestId(one.value()); e.setListDirectoryResult(refused);
        f.catalogs.response(f.connection, e); QVERIFY(!f.admit());
        QVERIFY(f.catalogs.request(f.context(), "/share"));
        f.catalogs.stop(); QVERIFY(!f.catalogs.request(f.context(), "/share"));
    }
};
QTEST_GUILESS_MAIN(RemoteCatalogsTest)
#include "remote_catalogs_test.moc"
