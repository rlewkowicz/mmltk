module;
#include <cstddef>
#include <cstdint>
export module mmltk.backend.media.capture.capture_types;
export namespace mmltk::backend::media::capture {
struct CaptureRegion final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    [[nodiscard]] bool operator==(const CaptureRegion&) const noexcept = default;
};
struct CaptureSessionIdentity {
    std::uint64_t session = 0;
    std::uint64_t generation = 0;
    [[nodiscard]] bool valid() const noexcept { return session != 0U && generation != 0U; }
    [[nodiscard]] bool operator==(const CaptureSessionIdentity&) const noexcept = default;
};
struct CaptureFormatInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bytes_per_line = 0;
};
class FilledCaptureSlotLease final {
   public:
    FilledCaptureSlotLease() = default;
    ~FilledCaptureSlotLease() noexcept;
    FilledCaptureSlotLease(const FilledCaptureSlotLease&) = delete;
    FilledCaptureSlotLease& operator=(const FilledCaptureSlotLease&) = delete;
    FilledCaptureSlotLease(FilledCaptureSlotLease&& other) noexcept;
    FilledCaptureSlotLease& operator=(FilledCaptureSlotLease&& other) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] CaptureSessionIdentity identity() const noexcept { return identity_; }
    [[nodiscard]] std::uint32_t slot() const noexcept { return slot_; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t stride_bytes() const noexcept { return stride_bytes_; }
    [[nodiscard]] std::uint32_t pixel_format() const noexcept { return pixel_format_; }
    [[nodiscard]] CaptureRegion region() const noexcept { return region_; }
    [[nodiscard]] std::uint64_t capture_ns() const noexcept { return capture_ns_; }
    [[nodiscard]] bool short_frame() const noexcept { return short_frame_; }

   private:
    FilledCaptureSlotLease(CaptureSessionIdentity identity, std::uint32_t slot, std::uint64_t sequence, const std::uint8_t* data, std::size_t bytes,
                           std::size_t stride_bytes, std::uint32_t pixel_format, CaptureRegion region, std::uint64_t capture_ns, bool short_frame) noexcept;
    void reset() noexcept;
    CaptureSessionIdentity identity_{};
    std::uint32_t slot_ = 0;
    std::uint64_t sequence_ = 0;
    const std::uint8_t* data_ = nullptr;
    std::size_t bytes_ = 0;
    std::size_t stride_bytes_ = 0;
    std::uint32_t pixel_format_ = 0;
    CaptureRegion region_{};
    std::uint64_t capture_ns_ = 0U;
    bool short_frame_ = false;
    friend class FilledCaptureSlotLeaseAuthority;
};
class FilledCaptureSlotLeaseAuthority final {
   public:
    [[nodiscard]] static FilledCaptureSlotLease Create(CaptureSessionIdentity identity, std::uint32_t slot, std::uint64_t sequence, const std::uint8_t* data,
                                                       std::size_t bytes, std::size_t stride_bytes, std::uint32_t pixel_format, CaptureRegion region,
                                                       std::uint64_t capture_ns, bool short_frame) noexcept;
    static void Consume(FilledCaptureSlotLease& lease) noexcept;
};
struct CaptureStats {
    std::uint64_t queued_v4l2_buffers = 0;
    std::uint64_t dequeued_v4l2_buffers = 0;
    std::uint64_t bytes_captured = 0;
    std::uint64_t filled_frames_published = 0;
    std::uint64_t h2d_frames_admitted = 0;
    std::uint64_t h2d_frames_completed = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t empty_frames_dropped = 0;
    std::uint64_t replaced_filled_frames = 0;
    std::uint64_t active_occupancy_drops = 0;
    std::uint64_t short_frames = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t requeue_failures = 0;
    bool running = false;
};
}  // namespace mmltk::backend::media::capture
