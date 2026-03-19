#pragma once

#include <cstdint>

#if FASTLOADER_ENABLE_PROFILING
#include <nvtx3/nvToolsExt.h>
#endif

namespace fastloader {

#if FASTLOADER_ENABLE_PROFILING

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

#define FASTLOADER_DETAIL_CONCAT_IMPL(lhs, rhs) lhs##rhs
#define FASTLOADER_DETAIL_CONCAT(lhs, rhs) FASTLOADER_DETAIL_CONCAT_IMPL(lhs, rhs)

#define FASTLOADER_PROFILE_SCOPE(name) \
    ::fastloader::ScopedProfile FASTLOADER_DETAIL_CONCAT(fastloader_profile_scope_, __LINE__)(name)
#define FASTLOADER_PROFILE_ADD(name, value) \
    ::fastloader::profile_add_value(name, static_cast<std::uint64_t>(value))
#define FASTLOADER_PROFILE_SET(name, value) \
    ::fastloader::profile_set_value(name, static_cast<std::uint64_t>(value))
#define FASTLOADER_PROFILE_RECORD_DURATION_NS(name, elapsed_ns) \
    ::fastloader::profile_record_duration_ns(name, static_cast<std::uint64_t>(elapsed_ns))
#define FASTLOADER_PROFILE_PROCESS_LABEL(label) \
    ::fastloader::profile_set_process_label(label)
#define FASTLOADER_PROFILE_RUN_LABEL(label) \
    ::fastloader::profile_set_run_label(label)
#define FASTLOADER_PROFILE_RESET_ITERATION() \
    ::fastloader::profile_reset_iteration()
#define FASTLOADER_PROFILE_CAPTURE_ITERATION(label) \
    ::fastloader::profile_capture_iteration(label)
#define FASTLOADER_PROFILE_FLUSH() \
    ::fastloader::profile_flush()

#define FASTLOADER_NVTX_RANGE(name, color) \
    ::fastloader::ScopedNvtxRange FASTLOADER_DETAIL_CONCAT(fastloader_nvtx_range_, __LINE__)(name, color)

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

#define FASTLOADER_PROFILE_SCOPE(name) ((void)0)
#define FASTLOADER_PROFILE_ADD(name, value) ((void)0)
#define FASTLOADER_PROFILE_SET(name, value) ((void)0)
#define FASTLOADER_PROFILE_RECORD_DURATION_NS(name, elapsed_ns) ((void)0)
#define FASTLOADER_PROFILE_PROCESS_LABEL(label) ((void)0)
#define FASTLOADER_PROFILE_RUN_LABEL(label) ((void)0)
#define FASTLOADER_PROFILE_RESET_ITERATION() ((void)0)
#define FASTLOADER_PROFILE_CAPTURE_ITERATION(label) ((void)0)
#define FASTLOADER_PROFILE_FLUSH() ((void)0)

#define FASTLOADER_NVTX_RANGE(name, color) ((void)0)

#undef FASTLOADER_DETAIL_CONCAT
#undef FASTLOADER_DETAIL_CONCAT_IMPL

#endif

} // namespace fastloader
