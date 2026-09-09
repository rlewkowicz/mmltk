#include "src/frameworks/transport/browser_record_ring.h"

#include <utility>

namespace mmltk::frameworks::transport {

BrowserRecordPush BrowserRecordRing::push(BrowserOutputRecord record) {
    std::scoped_lock lock(mutex_);
    if (record.priority == BrowserRecordPriority::Progress) {
        if (progress_) progress_storage_ = std::move(progress_->bytes);
        progress_ = std::move(record);
        return BrowserRecordPush::Enqueued;
    }
    if (record.state_system != 0U && record.state_event != 0U) {
        for (std::size_t offset = 0U; offset != size_; ++offset) {
            auto& prior = records_[(read_ + offset) % records_.size()];
            if (prior->state_system != record.state_system || prior->state_event != record.state_event) continue;
            if (prior->state_revision >= record.state_revision) return BrowserRecordPush::Enqueued;
            // Append the replacement after every intervening discrete event.
            // The fixed 64-slot move is bounded and retains FIFO edge ordering.
            for (std::size_t next = offset + 1U; next != size_; ++next)
                records_[(read_ + next - 1U) % records_.size()] = std::move(records_[(read_ + next) % records_.size()]);
            write_ = (write_ + records_.size() - 1U) % records_.size();
            records_[write_].reset();
            --size_;
            break;
        }
    }
    if (size_ == records_.size()) {
        return record.priority == BrowserRecordPriority::Transient ? BrowserRecordPush::Dropped : BrowserRecordPush::ClosePeer;
    }
    records_[write_].emplace(std::move(record));
    write_ = (write_ + 1U) % records_.size();
    ++size_;
    return BrowserRecordPush::Enqueued;
}

std::optional<BrowserOutputRecord> BrowserRecordRing::pop() {
    std::scoped_lock lock(mutex_);
    if (progress_) return std::exchange(progress_, std::nullopt);
    if (size_ == 0U) return std::nullopt;
    auto result = std::move(records_[read_]);
    records_[read_].reset();
    read_ = (read_ + 1U) % records_.size();
    --size_;
    return result;
}

mmltk::frameworks::serialization::wire::ByteBuffer BrowserRecordRing::acquire_progress_storage() {
    std::scoped_lock lock(mutex_);
    auto bytes = std::move(progress_storage_);
    bytes.clear();
    return bytes;
}
void BrowserRecordRing::recycle(BrowserOutputRecord record) {
    if (record.priority != BrowserRecordPriority::Progress) return;
    std::scoped_lock lock(mutex_);
    if (record.bytes.capacity() > progress_storage_.capacity()) progress_storage_ = std::move(record.bytes);
}

void BrowserRecordRing::clear() noexcept {
    std::scoped_lock lock(mutex_);
    for (auto& record : records_)
        record.reset();
    progress_.reset();
    read_ = 0U;
    write_ = 0U;
    size_ = 0U;
}

std::size_t BrowserRecordRing::size() const noexcept {
    std::scoped_lock lock(mutex_);
    return size_ + (progress_ ? 1U : 0U);
}

bool BrowserRecordRing::empty() const noexcept { return size() == 0U; }

}  // namespace mmltk::frameworks::transport
