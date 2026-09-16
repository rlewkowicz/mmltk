module;
#include <cstdint>
export module mmltk.common.logging.profile_utils;
export namespace mmltk::common::logging {
inline constexpr std::uint32_t nvtx_color_red = 0xFFFF0000U;
inline constexpr std::uint32_t nvtx_color_green = 0xFF00FF00U;
inline constexpr std::uint32_t nvtx_color_blue = 0xFF0000FFU;
inline constexpr std::uint32_t nvtx_color_yellow = 0xFFFFFF00U;
inline constexpr std::uint32_t nvtx_color_orange = 0xFFFFA500U;
inline constexpr std::uint32_t nvtx_color_purple = 0xFF800080U;
inline constexpr std::uint32_t nvtx_color_cyan = 0xFF00FFFFU;
inline constexpr std::uint32_t nvtx_color_magenta = 0xFFFF00FFU;
class ScopedProfile final {
   public:
    explicit ScopedProfile(const char* name) noexcept;
    ~ScopedProfile();
    ScopedProfile(const ScopedProfile&) = delete;
    ScopedProfile& operator=(const ScopedProfile&) = delete;

   private:
    const char* name_ = nullptr;
    std::uint64_t start_ns_ = 0U;
    bool active_ = false;
};
class ScopedNvtxRange final {
   public:
    ScopedNvtxRange(const char* name, std::uint32_t color) noexcept;
    ~ScopedNvtxRange();
    ScopedNvtxRange(const ScopedNvtxRange&) = delete;
    ScopedNvtxRange& operator=(const ScopedNvtxRange&) = delete;

   private:
    bool active_ = false;
};
[[nodiscard]] bool profile_enabled() noexcept;
void profile_enable() noexcept;
void profile_add_value(const char* name, std::uint64_t delta);
void profile_set_value(const char* name, std::uint64_t value);
void profile_record_duration_ns(const char* name, std::uint64_t elapsed_ns);
void profile_set_process_label(const char* label);
void profile_set_run_label(const char* label);
void profile_reset_iteration();
void profile_capture_iteration(const char* label);
void profile_flush();
}  // namespace mmltk::common::logging
