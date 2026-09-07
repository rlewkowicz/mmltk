module;
#include <cstdint>
export module mmltk.backend.media.live.live_frame_id;
export namespace mmltk::backend::media::live {
struct LiveFrameId final {
    std::uint64_t session = 0U;
    std::uint64_t sequence = 0U;
    [[nodiscard]] constexpr bool valid() const noexcept { return session != 0U && sequence != 0U; }
    bool operator==(const LiveFrameId&) const noexcept = default;
};
}  // namespace mmltk::backend::media::live
