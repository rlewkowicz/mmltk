#pragma once

#include <cstdint>

#if MMLTK_ENABLE_PROFILING
#include <nvtx3/nvToolsExt.h>
#endif

namespace mmltk {

#if MMLTK_ENABLE_PROFILING

class ScopedProfile {
public:
    explicit ScopedProfile(const char* name);
    ~ScopedProfile();

    ScopedProfile(const ScopedProfile&) = delete;
    ScopedProfile& operator=(const ScopedProfile&) = delete;

private:
    const char* name_;
    std::uint64_t start_ns_;
};

void profile_add_value(const char* name, std::uint64_t delta);
void profile_set_value(const char* name, std::uint64_t value);
void profile_record_duration_ns(const char* name, std::uint64_t elapsed_ns);
void profile_set_process_label(const char* label);
void profile_set_run_label(const char* label);
void profile_reset_iteration();
void profile_capture_iteration(const char* label);
void profile_flush();

#define MMLTK_DETAIL_CONCAT_IMPL(lhs, rhs) lhs##rhs
#define MMLTK_DETAIL_CONCAT(lhs, rhs) MMLTK_DETAIL_CONCAT_IMPL(lhs, rhs)

#define MMLTK_PROFILE_SCOPE(name) \
    ::mmltk::ScopedProfile MMLTK_DETAIL_CONCAT(mmltk_profile_scope_, __LINE__)(name)
#define MMLTK_PROFILE_ADD(name, value) \
    ::mmltk::profile_add_value(name, static_cast<std::uint64_t>(value))
#define MMLTK_PROFILE_SET(name, value) \
    ::mmltk::profile_set_value(name, static_cast<std::uint64_t>(value))
#define MMLTK_PROFILE_RECORD_DURATION_NS(name, elapsed_ns) \
    ::mmltk::profile_record_duration_ns(name, static_cast<std::uint64_t>(elapsed_ns))
#define MMLTK_PROFILE_PROCESS_LABEL(label) \
    ::mmltk::profile_set_process_label(label)
#define MMLTK_PROFILE_RUN_LABEL(label) \
    ::mmltk::profile_set_run_label(label)
#define MMLTK_PROFILE_RESET_ITERATION() \
    ::mmltk::profile_reset_iteration()
#define MMLTK_PROFILE_CAPTURE_ITERATION(label) \
    ::mmltk::profile_capture_iteration(label)
#define MMLTK_PROFILE_FLUSH() \
    ::mmltk::profile_flush()

#define MMLTK_NVTX_RANGE(name, color) \
    ::mmltk::ScopedNvtxRange MMLTK_DETAIL_CONCAT(mmltk_nvtx_range_, __LINE__)(name, color)

class ScopedNvtxRange {
public:
    ScopedNvtxRange(const char* name, uint32_t color) {
        nvtxEventAttributes_t eventAttrib{};
        eventAttrib.version = NVTX_VERSION;
        eventAttrib.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
        eventAttrib.colorType = NVTX_COLOR_ARGB;
        eventAttrib.color = color;
        eventAttrib.messageType = NVTX_MESSAGE_TYPE_ASCII;
        eventAttrib.message.ascii = name;
        nvtxRangePushEx(&eventAttrib);
    }
    ~ScopedNvtxRange() {
        nvtxRangePop();
    }
};

// Common colors for NVTX
#define NVTX_COLOR_RED    0xFFFF0000
#define NVTX_COLOR_GREEN  0xFF00FF00
#define NVTX_COLOR_BLUE   0xFF0000FF
#define NVTX_COLOR_YELLOW 0xFFFFFF00
#define NVTX_COLOR_ORANGE 0xFFFFA500
#define NVTX_COLOR_PURPLE 0xFF800080
#define NVTX_COLOR_CYAN   0xFF00FFFF
#define NVTX_COLOR_MAGENTA 0xFFFF00FF

#else

class ScopedProfile {
public:
    explicit ScopedProfile(const char*) {}
    ~ScopedProfile() = default;
};

inline void profile_add_value(const char*, std::uint64_t) {}
inline void profile_set_value(const char*, std::uint64_t) {}
inline void profile_record_duration_ns(const char*, std::uint64_t) {}
inline void profile_set_process_label(const char*) {}
inline void profile_set_run_label(const char*) {}
inline void profile_reset_iteration() {}
inline void profile_capture_iteration(const char*) {}
inline void profile_flush() {}

#define MMLTK_PROFILE_SCOPE(name) ((void)0)
#define MMLTK_PROFILE_ADD(name, value) ((void)0)
#define MMLTK_PROFILE_SET(name, value) ((void)0)
#define MMLTK_PROFILE_RECORD_DURATION_NS(name, elapsed_ns) ((void)0)
#define MMLTK_PROFILE_PROCESS_LABEL(label) ((void)0)
#define MMLTK_PROFILE_RUN_LABEL(label) ((void)0)
#define MMLTK_PROFILE_RESET_ITERATION() ((void)0)
#define MMLTK_PROFILE_CAPTURE_ITERATION(label) ((void)0)
#define MMLTK_PROFILE_FLUSH() ((void)0)

#define MMLTK_NVTX_RANGE(name, color) ((void)0)

#undef MMLTK_DETAIL_CONCAT
#undef MMLTK_DETAIL_CONCAT_IMPL

#endif

} // namespace mmltk
