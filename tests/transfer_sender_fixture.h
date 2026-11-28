#pragma once
#include <ds/dsengine.h>
#include <ds/outgoingtransfer.h>
#include <ds/peer.h>
#include <QCryptographicHash>
#include <utility>
#include <stdexcept>
using namespace ds::core;

// Deterministic in-process control counterpart for generic sender tests.
class CheckpointSendingPeer final : public ds::prot::Peer {
public:
    CheckpointSendingPeer() : Peer(ds::prot::ConnectionSocket::create(), {}) {}
    Direction getDirection() const noexcept override { return OUTGOING; }
    bool isConnected() const noexcept override { return true; }
    quint32 checkpointInterval() const noexcept override { return dsfm::minimum_checkpoint_interval; }
    void begin(const File::ptr_t &file) {
        file_ = file;
        owner_ = std::make_shared<OutgoingTransfer>(file, std::static_pointer_cast<PeerConnection>(shared_from_this()), OutgoingTransferStart::Offer);
        if (!DsEngine::instance().transfers().acceptWork(*file)) throw std::runtime_error("Test work acceptance failed");
        if (!DsEngine::instance().transfers().submit(owner_)) throw std::runtime_error("Test sender admission failed");
    }
    void resume(const File::ptr_t &file, QByteArray prefix, dsfm::DurableTransferProgress progress) {
        file_ = file; sent = std::move(prefix);
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Resume;
        event.request = 9; event.candidates = progress.checkpoints;
        owner_ = std::make_shared<OutgoingTransfer>(file, std::static_pointer_cast<PeerConnection>(shared_from_this()),
                                                  OutgoingTransferStart::Resume, std::move(event));
        if (!DsEngine::instance().transfers().acceptWork(*file)) throw std::runtime_error("Test work acceptance failed");
        if (!DsEngine::instance().transfers().submit(owner_)) throw std::runtime_error("Test resume admission failed");
    }
    uint64_t sendCheckpointResumeResult(quint64, const QByteArray &, FileResumeStatus,
            std::optional<dsfm::TransferAttempt> selected, dsfm::TransferPauseReason,
            FileCancelReason) override {
        if (selected) {
            resumed_offset = next_offset_ = selected->selected.offset;
            sent.truncate(qsizetype(next_offset_)); checkpoint_sequence_ = selected->next_sequence;
            TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ready; event.selection = selected;
            owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
        }
        return 0;
    }
    uint64_t offerFile(const File &) override {
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ready; event.selection = file_->checkpointAttempt();
        owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
        return 0;
    }
    uint64_t sendCheckpointChunk(const QByteArray &, QUuid, quint64 sequence, quint64 offset, const QByteArray &bytes, bool final) override {
        if (interrupt_after_checkpoint && offset >= dsfm::minimum_checkpoint_interval) {
            owner_->stop(TransferSession::Stop::Disconnected); return 0;
        }
        sent.append(bytes); next_sequence_ = sequence + 1; next_offset_ = offset + quint64(bytes.size()); final_sent = final;
        if (!final && mutate) std::exchange(mutate, {})();
        if (!final) ack(false);
        if (final || next_offset_ % dsfm::minimum_checkpoint_interval == 0) {
            TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
            event.checkpoint = {file_->getSize() == 0 ? 0 : checkpoint_sequence_++, next_offset_,
                QCryptographicHash::hash(sent, QCryptographicHash::Sha256), final, false};
            event.checkpoint_phase = dsfm::CheckpointPhase::Proposed;
            owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
        }
        return 0;
    }
    uint64_t sendFileCheckpoint(const QByteArray &, const QByteArray &, const dsfm::TransferAttempt &,
                                dsfm::DurableCheckpoint checkpoint, dsfm::CheckpointPhase) override {
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Checkpoint;
        event.checkpoint = checkpoint; event.checkpoint_phase = dsfm::CheckpointPhase::MutuallyConfirmed;
        owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
        if (checkpoint.final) ack(true);
        return 0;
    }
    uint64_t sendFileCancel(const QByteArray &, FileCancelReason reason) override { cancelled = reason == FileCancelReason::SourceChanged; return 0; }
    uint64_t sendFilePause(const QByteArray &, QUuid, dsfm::DurableCheckpoint, dsfm::TransferPauseReason) override { return 0; }
    QByteArray sent;
    bool final_sent = false, cancelled = false, interrupt_after_checkpoint = false;
    quint64 resumed_offset = 0;
    std::function<void()> mutate;
private:
    void ack(bool final) {
        TransferSession::Event event; event.kind = TransferSession::Event::Kind::Ack;
        event.sequence = next_sequence_; event.offset = next_offset_; event.final = final;
        owner_->deliver(owner_->connection(), owner_->attempt(), std::move(event));
    }
    File::ptr_t file_;
    std::shared_ptr<OutgoingTransfer> owner_;
    quint64 next_sequence_ = 0, next_offset_ = 0, checkpoint_sequence_ = 1;
};
