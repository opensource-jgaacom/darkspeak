#include <QtTest>
#include <QTcpServer>
#include "ds/connectionsocket.h"
#include "ds/peer.h"

namespace {
class ConnectedPeer final : public ds::prot::Peer {
public:
    explicit ConnectedPeer(ds::prot::ConnectionSocket::ptr_t socket) : Peer(std::move(socket), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
};
}

class TransportPriorityTest final : public QObject {
    Q_OBJECT
private slots:
    void qtPendingBytesRemainBounded() {
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost));
        auto socket = ds::prot::ConnectionSocket::create();
        socket->connectToHost(QHostAddress::LocalHost, listener.serverPort());
        QTRY_COMPARE(socket->state(), QAbstractSocket::ConnectedState);
        QTRY_VERIFY(listener.hasPendingConnections());
        const std::unique_ptr<QTcpSocket> receiver{listener.nextPendingConnection()};
        QSignalSpy drained(socket.get(), &ds::prot::ConnectionSocket::outputBufferEmptied);
        const QByteArray bytes(ds::prot::ConnectionSocket::maxOutgoingBytes(), 'p');
        socket->write(bytes);
        // No event processing: sendMore has handed the record to Qt, whose
        // internal queue must still consume the complete local output budget.
        QCOMPARE(socket->bytesToWrite(), qint64(bytes.size()));
        QCOMPARE(socket->queuedBytes(), size_t(bytes.size()));
        QVERIFY_EXCEPTION_THROWN(socket->write(QByteArray(1, 'x')), std::runtime_error);
        QCOMPARE(socket->queuedBytes(), size_t(bytes.size()));
        // A partial notification is not a complete drain notification.
        QVERIFY(QMetaObject::invokeMethod(socket.get(), "bytesWritten", Qt::DirectConnection, Q_ARG(qint64, 1)));
        QCOMPARE(drained.count(), 0);
        QTRY_COMPARE(socket->queuedBytes(), size_t(0));
        QTRY_VERIFY(drained.count() > 0);
        QTRY_COMPARE(receiver->bytesAvailable(), qint64(bytes.size()));
        QCOMPARE(receiver->readAll(), bytes);
    }

    void chatObserverPrecedesNextPayloadQuantum() {
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost));
        auto socket = ds::prot::ConnectionSocket::create();
        socket->connectToHost(QHostAddress::LocalHost, listener.serverPort());
        QTRY_COMPARE(socket->state(), QAbstractSocket::ConnectedState);
        QTRY_VERIFY(listener.hasPendingConnections());
        const std::unique_ptr<QTcpSocket> receiver{listener.nextPendingConnection()};
        ConnectedPeer peer{socket};
        QVERIFY(peer.transferWritable());
        bool chat_pending = true;
        int blocked = 0, payloads = 0;
        // Production Contact installs its chat observer before session payload
        // observers. The first new payload must yield to that queued chat.
        connect(&peer, &ds::core::PeerConnection::outputBufferEmptied, this, [&] {
            if (chat_pending) { chat_pending = false; socket->write(QByteArray("chat")); }
        });
        connect(&peer, &ds::core::PeerConnection::outputBufferEmptied, this, [&] {
            if (payloads) return;
            if (!peer.transferWritable()) { ++blocked; return; }
            ++payloads;
            socket->write(QByteArray("second"));
        });
        socket->write(QByteArray("first"));
        QVERIFY(!peer.transferWritable());
        QTRY_COMPARE(payloads, 1);
        QCOMPARE(blocked, 1);
        QTRY_COMPARE(socket->queuedBytes(), size_t(0));
        QTRY_COMPARE(receiver->bytesAvailable(), qint64(15));
        QCOMPARE(receiver->readAll(), QByteArray("firstchatsecond"));
        QVERIFY(peer.transferWritable());
    }
};

QTEST_GUILESS_MAIN(TransportPriorityTest)
#include "transport_priority_test.moc"
