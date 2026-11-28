#include <QtTest>
#include <QFile>
#include <QTcpServer>
#include <QtEndian>
#include "ds/peer.h"
#include "ds/filesharecodec.h"

using namespace ds::protocol;
namespace fs = ds::prot::fileshare;
using ds::prot::codec::serialize;
using ds::prot::codec::deserialize;

namespace {
VirtualPath path(QString value = "/", int encoding = 1) {
    VirtualPath p; p.setPath(value); p.setEncoding(VirtualPathEncodingGadget::VirtualPathEncoding(encoding)); return p;
}
Envelope list(quint64 id = 1, bool recursive = false) {
    ListDirectoryRequest r; r.setDirectory(path()); r.setRecursive(recursive);
    Envelope e; e.setProtocolVersion(3); e.setRequestId(id); e.setListDirectoryRequest(r); return e;
}
Envelope result(quint64 id = 1, int status = 1) {
    ListDirectoryResult r; r.setStatus(FileShareStatusGadget::FileShareStatus(status));
    if (status == 1) { r.setDirectory(path()); r.setComplete(true); }
    Envelope e; e.setProtocolVersion(3); e.setResponseToRequestId(id); e.setListDirectoryResult(r); return e;
}
DirectoryEntry file() {
    DirectoryEntry f; f.setNodeId(QByteArray(32,'n'));
    VirtualName name; name.setEncoding(VirtualPathEncodingGadget::VirtualPathEncoding(1)); name.setValue("CON"); f.setName(name);
    f.setType(VirtualEntryTypeGadget::VirtualEntryType(1)); f.setState(VirtualEntryStateGadget::VirtualEntryState(1));
    f.setSize(0); f.setSha256(QByteArray(32,'h')); return f;
}
class Peer final : public ds::prot::Peer {
public:
    Peer() : ds::prot::Peer(ds::prot::ConnectionSocket::create(),{}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    using ds::prot::Peer::onReceivedEnvelope;
    using ds::prot::Peer::sendCapabilities;
    void initializeSender(crypto_secretstream_xchacha20poly1305_state &receiver) {
        unsigned char key[crypto_secretstream_xchacha20poly1305_KEYBYTES];
        unsigned char header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
        crypto_secretstream_xchacha20poly1305_keygen(key);
        crypto_secretstream_xchacha20poly1305_init_push(&stateOut,header,key);
        if (crypto_secretstream_xchacha20poly1305_init_pull(&receiver,header,key)) qFatal("test stream setup");
        sodium_memzero(key,sizeof key);
    }
    void install() {
        configureFileShare([this] { return ready; },[this] { return unicode; },[this](const Envelope &e) { delivered.append(e); });
    }
    bool ready = true, unicode = true;
    QList<Envelope> delivered;
    void enable(bool remote = true) {
        install();
        advertised_fileshare_ = true; capabilities_sent_ = true;
        auto cap = ds::prot::codec::makeCapabilities(remote ? QStringList{"fileshare-v1"} : QStringList{},1048576);
        onReceivedEnvelope(cap); peer_fileshare_utf8_ = true;
    }
    quint64 pending(Envelope request = list()) {
        const auto id = nextRequestId(RequestType::FileShare); request.setRequestId(id);
        auto &p = outstandingRequests_.at(id); p.fileshare_request = request;
        p.expires = std::chrono::steady_clock::now()+std::chrono::seconds(30); return id;
    }
    void expire(quint64 id) { outstandingRequests_.at(id).expires = std::chrono::steady_clock::now()-std::chrono::seconds(1); }
    void cancel(quint64 id) { outstandingRequests_.at(id).cancelled = true; }
    void expireIncoming() { for (auto &[id,p] : incoming_fileshare_) p.expires = std::chrono::steady_clock::now()-std::chrono::seconds(1); }
    size_t incoming() { return incoming_fileshare_.size(); }
    bool advertised() { return advertised_fileshare_; }
    void peerUnicode(bool value) { peer_fileshare_utf8_ = value; }
};
}
class FileShareProtocolTest : public QObject {
    Q_OBJECT
private slots:
    void wireFixtureAndRoundTrips() {
        QFile f(QStringLiteral(DS_FILESHARE_FIXTURES "/fileshare-v1-list.hex")); QVERIFY(f.open(QIODevice::ReadOnly));
        const auto bytes = QByteArray::fromHex(f.readAll().trimmed());
        QCOMPARE(serialize(list()),bytes);
        Envelope decoded; QVERIFY(deserialize(bytes,decoded)); QVERIFY(fs::validate(decoded,true));
        QCOMPARE(serialize(decoded),bytes);
        QList<Envelope> messages{list(),result(),result(1,3)};
        StatVirtualRequest s; s.setPath(path("/COM1")); Envelope e; e.setProtocolVersion(3); e.setRequestId(2); e.setStatVirtualRequest(s); messages.append(e);
        StatVirtualResult sr; sr.setStatus(FileShareStatusGadget::FileShareStatus(1)); sr.setPath(s.path()); sr.setEntry(file()); e.setResponseToRequestId(2); e.setStatVirtualResult(sr); messages.append(e);
        RequestDownload d; d.setPath(path("/AUX")); d.setExpectedSize(0); d.setExpectedSha256(QByteArray(32,'h')); e.setRequestId(3); e.setRequestDownload(d); messages.append(e);
        RequestDownloadResult dr; dr.setStatus(FileShareStatusGadget::FileShareStatus(1)); dr.setTransferId(QByteArray(16,'t')); e.setResponseToRequestId(3); e.setRequestDownloadResult(dr); messages.append(e);
        CancelFileShareRequest c; c.setRequestToCancel(1); e.setRequestId(4); e.setCancelFileShareRequest(c); messages.append(e);
        CancelFileShareResult cr; cr.setStatus(FileShareStatusGadget::FileShareStatus(1)); e.setResponseToRequestId(4); e.setCancelFileShareResult(cr); messages.append(e);
        auto recursive = result(); auto r = recursive.listDirectoryResult(); r.setComplete(false); r.setCatalogTransferId(QByteArray(16,'t')); r.setCatalogFormatVersion(2); recursive.setListDirectoryResult(r); messages.append(recursive);
        for (const auto &message : messages) { QVERIFY(fs::validate(message,true)); QVERIFY(deserialize(serialize(message),decoded)); QVERIFY(fs::validate(decoded,true)); QCOMPARE(serialize(message),serialize(decoded)); }
    }
    void portablePaths_data() {
        QTest::addColumn<QString>("value"); QTest::addColumn<int>("encoding"); QTest::addColumn<bool>("valid");
        QTest::newRow("unicode") << QString::fromUtf8("/résumé%FF") << 1 << true;
        QTest::newRow("escaped") << QString("/r%C3%A9sum%C3%A9%25FF") << 2 << true;
        QTest::newRow("single-layer") << QString("/%25FF") << 2 << true;
        QTest::newRow("percent") << QString("/%") << 1 << true;
        QTest::newRow("backslash") << QString("/a\\b") << 2 << true;
        for (const auto &s : {"/../a","//a","/a/","/%2F","/%00","/%FF","/%C3","/%EF%BB%BFx","/%c3%A9","/%41","/%C0%AF","/%E2%80%AE"}) QTest::newRow(s) << QString(s) << 2 << false;
        QTest::newRow("missing-encoding") << QString("/") << 0 << false;
        QTest::newRow("unknown-encoding") << QString("/") << 9 << false;
        QTest::newRow("oversize-component") << "/"+QString(766,'a') << 1 << false;
        QTest::newRow("max-component") << "/"+QString(765,'a') << 1 << true;
        QTest::newRow("depth") << "/"+QString("a/").repeated(64)+"a" << 1 << false;
        QTest::newRow("path-length") << "/"+QString("abc/").repeated(1100)+"a" << 1 << false;
    }
    void portablePaths() { QFETCH(QString,value); QFETCH(int,encoding); QFETCH(bool,valid); QCOMPARE(fs::validPath(path(value,encoding),true),valid); }
    void asciiPreference() {
        QVERIFY(!fs::validPath(path("/ASCII",1),false));
        QVERIFY(fs::validPath(path("/r%C3%A9sum%C3%A9%25FF",2),false));
        auto p = std::make_shared<Peer>(); p->enable(); p->unicode = false;
        QVERIFY_EXCEPTION_THROWN(p->onReceivedEnvelope(list()),std::exception);
        auto q = std::make_shared<Peer>(); q->enable(); auto e = list(); auto r = e.listDirectoryRequest(); r.setDirectory(path("/r%C3%A9",2)); e.setListDirectoryRequest(r); q->unicode = false; q->onReceivedEnvelope(e); QCOMPARE(q->delivered.size(),1);
        auto remote_ascii = std::make_shared<Peer>(); remote_ascii->enable(); remote_ascii->peerUnicode(false);
        QVERIFY_EXCEPTION_THROWN(remote_ascii->onReceivedEnvelope(list()),std::exception);
    }
    void malformedWire() {
        auto bytes = serialize(list()); Envelope e;
        // Fixture's final byte is the root path string; Qt must never replace it.
        bytes[bytes.size()-1] = char(0xff); QVERIFY(!deserialize(bytes,e));
        bytes[bytes.size()-1] = char(0xc3); QVERIFY(!deserialize(bytes,e)); // truncated sequence must not buffer silently
        bytes = serialize(list()); bytes += bytes.sliced(4); QVERIFY(!deserialize(bytes,e)); // duplicate feature body
        // Path declares unknown field, scalar and control-size bounds.
        QVERIFY(!fs::validateWire(QByteArray(fs::max_control_bytes+1,'x')));
        bytes = serialize(list()); bytes += QByteArray::fromHex("1001"); QVERIFY(!deserialize(bytes,e)); // duplicate request_id
        QVERIFY(!deserialize(QByteArray::fromHex("080310018202090a05080112012f1002"),e)); // bool cannot coerce 2 to true
        QVERIFY(!deserialize(QByteArray::fromHex("080310018202090a05080112012f2801"),e)); // unknown request field
    }
    void resultShapesAndLimits() {
        auto e = result(1,3); auto r = e.listDirectoryResult(); r.setDirectory(path()); e.setListDirectoryResult(r); QVERIFY(!fs::validate(e,true));
        e = result(); r = e.listDirectoryResult(); r.setComplete(false); e.setListDirectoryResult(r); QVERIFY(!fs::validate(e,true));
        r.setContinuation("cursor"); e.setListDirectoryResult(r); QVERIFY(fs::validate(e,true)); // empty continuing page
        r.setContinuation(QByteArray(2049,'x')); e.setListDirectoryResult(r); QVERIFY(!fs::validate(e,true));
        r.setContinuation({}); r.setComplete(true); r.setEntries(QList<DirectoryEntry>(101,file())); e.setListDirectoryResult(r); QVERIFY(!fs::validate(e,true)); QVERIFY(!fs::validateWire(serialize(e)));
        r.setEntries({file()}); e.setListDirectoryResult(r); QVERIFY(fs::validate(e,true));
        auto f = file(); f.setType(VirtualEntryTypeGadget::VirtualEntryType(2)); r.setEntries({f}); e.setListDirectoryResult(r); QVERIFY(!fs::validate(e,true));
        auto request = list(); auto a = request.listDirectoryRequest(); a.setRecursive(true); a.setPageSize(1); request.setListDirectoryRequest(a); QVERIFY(!fs::validate(request,true));
        a.setPageSize(0); a.setContinuation("x"); request.setListDirectoryRequest(a); QVERIFY(!fs::validate(request,true));
        e = result(); r = e.listDirectoryResult(); r.setStatus(FileShareStatusGadget::FileShareStatus(99)); e.setListDirectoryResult(r); QVERIFY(!fs::validate(e,true));
        QVERIFY(!fs::matches(list(),result(1,3))); QVERIFY(fs::matches(list(1,true),result(1,3)));
    }
    void negotiationAndRevocation() {
        auto p = std::make_shared<Peer>(); p->enable(false);
        Envelope keepalive; keepalive.setProtocolVersion(3); keepalive.setKeepAlive(KeepAlive{}); p->onReceivedEnvelope(keepalive);
        QVERIFY_EXCEPTION_THROWN(p->onReceivedEnvelope(list()),std::exception); QVERIFY(p->delivered.isEmpty());
        auto q = std::make_shared<Peer>(); q->enable(); q->ready = false;
        QVERIFY_EXCEPTION_THROWN(q->onReceivedEnvelope(list()),std::exception); QVERIFY(q->delivered.isEmpty());
        QVERIFY_EXCEPTION_THROWN(q->sendFileShare(list()),std::exception);
        QVERIFY_EXCEPTION_THROWN(q->configureFileShare({}, {}, {}),std::exception);
        auto off = std::make_shared<Peer>(); off->onReceivedEnvelope(ds::prot::codec::makeCapabilities({"fileshare-v1"},1048576));
        QVERIFY_EXCEPTION_THROWN(off->onReceivedEnvelope(list()),std::exception);
        auto closed = std::make_shared<Peer>(); closed->enable(); closed->close();
        QVERIFY_EXCEPTION_THROWN(closed->onReceivedEnvelope(list()),std::exception); QVERIFY(closed->delivered.isEmpty());
    }
    void authenticatedOutgoingLifecycle() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost,0));
        auto p = std::make_shared<Peer>(); auto socket = p->getConnectionPtr();
        socket->connectToHost(QHostAddress::LocalHost,server.serverPort()); QVERIFY(socket->waitForConnected(1000));
        QVERIFY(server.waitForNewConnection(1000)); auto remote = std::unique_ptr<QTcpSocket>(server.nextPendingConnection()); QVERIFY(remote);
        crypto_secretstream_xchacha20poly1305_state receiver{}; p->initializeSender(receiver); p->install(); p->sendCapabilities(); QVERIFY(p->advertised());
        QVERIFY(socket->waitForBytesWritten(1000)); QVERIFY(remote->waitForReadyRead(1000));
        const auto record = remote->readAll(); QVERIFY(record.size() > 4);
        const auto length = qFromBigEndian<quint32>(record.constData()); QCOMPARE(record.size(),qsizetype(length+4));
        QByteArray plain(length-crypto_secretstream_xchacha20poly1305_ABYTES,Qt::Uninitialized);
        unsigned char tag = 0; QCOMPARE(crypto_secretstream_xchacha20poly1305_pull(&receiver,reinterpret_cast<unsigned char*>(plain.data()),nullptr,&tag,reinterpret_cast<const unsigned char*>(record.constData()+4),length,nullptr,0),0);
        TransportEnvelope transport; QVERIFY(deserialize(plain,transport)); Envelope capability; QVERIFY(deserialize(transport.payload(),capability)); QVERIFY(capability.capabilities().capabilities().contains("fileshare-v1"));
        p->onReceivedEnvelope(ds::prot::codec::makeCapabilities({"fileshare-v1"},1048576)); p->peerUnicode(true);
        const auto id = p->sendFileShare(list()); QVERIFY(id); p->onReceivedEnvelope(result(id)); QCOMPARE(p->delivered.size(),1);
        const auto cancelled = p->sendFileShare(list());
        CancelFileShareRequest cancel; cancel.setRequestToCancel(cancelled); auto e = list(); e.setCancelFileShareRequest(cancel); const auto cid = p->sendFileShare(e);
        p->onReceivedEnvelope(result(cancelled)); QCOMPARE(p->delivered.size(),1);
        CancelFileShareResult cr; cr.setStatus(FileShareStatusGadget::FileShareStatus(1)); e.setResponseToRequestId(cid); e.setCancelFileShareResult(cr); p->onReceivedEnvelope(e); QCOMPARE(p->delivered.size(),2);
        p->onReceivedEnvelope(list(10)); QCOMPARE(p->incoming(),size_t(1)); p->sendFileShare(result(10)); QCOMPARE(p->incoming(),size_t(0));
        QVERIFY_EXCEPTION_THROWN(p->sendFileShare(result(10)),std::exception);
        for (int i = 0; i < 16; ++i) p->sendFileShare(list());
        QVERIFY_EXCEPTION_THROWN(p->sendFileShare(list()),std::exception);
        p->ready = false; QVERIFY_EXCEPTION_THROWN(p->sendFileShare(list()),std::exception);
    }
    void correlationAndCancellation() {
        auto p = std::make_shared<Peer>(); p->enable(); auto id = p->pending();
        p->onReceivedEnvelope(result(id)); QCOMPARE(p->delivered.size(),1);
        QVERIFY_EXCEPTION_THROWN(p->onReceivedEnvelope(result(id)),std::exception);
        auto q = std::make_shared<Peer>(); q->enable(); QVERIFY_EXCEPTION_THROWN(q->onReceivedEnvelope(result(id)),std::exception);
        auto x = std::make_shared<Peer>(); x->enable(); auto expired = x->pending(); x->expire(expired); QVERIFY_EXCEPTION_THROWN(x->onReceivedEnvelope(result(expired)),std::exception);
        auto c = std::make_shared<Peer>(); c->enable(); auto cancelled = c->pending(); c->cancel(cancelled); c->onReceivedEnvelope(result(cancelled)); QVERIFY(c->delivered.isEmpty());
        auto mismatch = std::make_shared<Peer>(); mismatch->enable(); auto req = list(); auto r = req.listDirectoryRequest(); r.setDirectory(path("/other")); req.setListDirectoryRequest(r); auto mid = mismatch->pending(req); QVERIFY_EXCEPTION_THROWN(mismatch->onReceivedEnvelope(result(mid)),std::exception);
    }
    void incomingBoundsReplayAndExpiry() {
        auto p = std::make_shared<Peer>(); p->enable();
        for (quint64 i = 1; i <= 16; ++i) p->onReceivedEnvelope(list(i));
        QCOMPARE(p->incoming(),size_t(16));
        QVERIFY_EXCEPTION_THROWN(p->onReceivedEnvelope(list(17)),std::exception);
        auto q = std::make_shared<Peer>(); q->enable(); q->onReceivedEnvelope(list()); QVERIFY_EXCEPTION_THROWN(q->onReceivedEnvelope(list()),std::exception);
        auto x = std::make_shared<Peer>(); x->enable(); x->onReceivedEnvelope(list()); x->expireIncoming(); x->onReceivedEnvelope(list(2)); QCOMPARE(x->incoming(),size_t(1));
        CancelFileShareRequest cancel; cancel.setRequestToCancel(1); auto e = list(3); e.setCancelFileShareRequest(cancel); x->onReceivedEnvelope(e); QCOMPARE(x->delivered.size(),3);
        cancel.setRequestToCancel(4); e.setRequestId(4); e.setCancelFileShareRequest(cancel); QVERIFY(!fs::validate(e,true));
    }
};
QTEST_GUILESS_MAIN(FileShareProtocolTest)
#include "fileshare_protocol_test.moc"
