#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

#include "src/frameworks/serialization/serialization.h"

namespace mmltk::frameworks::transport {

inline constexpr std::size_t kBrowserRecordRingCapacity = 64U;

enum class BrowserRecordPriority : std::uint8_t {
    Transient,
    Critical,
    Progress,
};

enum class BrowserRecordPush : std::uint8_t {
    Enqueued,
    Dropped,
    ClosePeer,
};

struct BrowserOutputRecord final {
    mmltk::frameworks::serialization::wire::ByteBuffer bytes;
    BrowserRecordPriority priority = BrowserRecordPriority::Transient;
    // Canonical event identity, populated only for complete replaceable state.
    std::uint64_t state_system = 0U;
    std::uint64_t state_event = 0U;
    std::uint64_t state_revision = 0U;
};

// The ring has one uWebSockets-loop consumer and any number of system-worker
// producers. Storage is allocated once; indices and occupancy remain bounded.
// The short mutex section moves an already encoded record and never performs
// socket work.
class BrowserRecordRing final {
   public:
    [[nodiscard]] BrowserRecordPush push(BrowserOutputRecord record);
    [[nodiscard]] std::optional<BrowserOutputRecord> pop();
    void clear() noexcept;
    [[nodiscard]] mmltk::frameworks::serialization::wire::ByteBuffer acquire_progress_storage();
    void recycle(BrowserOutputRecord);
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

   private:
    mutable std::mutex mutex_;
    std::array<std::optional<BrowserOutputRecord>, kBrowserRecordRingCapacity> records_{};
    std::optional<BrowserOutputRecord> progress_;
    mmltk::frameworks::serialization::wire::ByteBuffer progress_storage_;
    std::size_t read_ = 0U;
    std::size_t write_ = 0U;
    std::size_t size_ = 0U;
};

}  // namespace mmltk::frameworks::transport
