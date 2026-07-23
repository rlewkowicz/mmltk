#include "mmltk/live/workspace_trace.h"

#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace mmltk::live {

namespace {

struct WorkspaceTraceState {
    WorkspaceTraceState() noexcept {
        const char* path = std::getenv("MMLTK_GUI_TRACE_FILE");
        if (path != nullptr && *path != '\0') {
            fd = ::open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        }
    }

    ~WorkspaceTraceState() {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    WorkspaceTraceState(const WorkspaceTraceState&) = delete;
    WorkspaceTraceState& operator=(const WorkspaceTraceState&) = delete;

    int fd = -1;
    std::mutex mutex;
};

WorkspaceTraceState& workspace_trace_state() noexcept {
    static WorkspaceTraceState state;
    return state;
}

void write_all(const int fd, const char* data, std::size_t size) noexcept {
    while (size != 0U) {
        const ssize_t written = ::write(fd, data, size);
        if (written > 0) {
            data += written;
            size -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

}  

bool workspace_trace_enabled() noexcept {
    return workspace_trace_state().fd >= 0;
}

void write_workspace_trace(const std::string_view source, const std::string_view event,
                           nlohmann::json fields) noexcept {
    WorkspaceTraceState& state = workspace_trace_state();
    if (state.fd < 0) {
        return;
    }
    try {
        nlohmann::json record{{"source", source}, {"level", "trace"}, {"event", event}, {"fields", std::move(fields)}};
        std::string line = record.dump();
        line.push_back('\n');
        std::lock_guard lock(state.mutex);
        write_all(state.fd, line.data(), line.size());
    } catch (...) {
        constexpr std::string_view fallback =
            "{\"source\":\"native\",\"level\":\"error\",\"event\":\"workspace.trace_serialize_failed\"}\n";
        std::lock_guard lock(state.mutex);
        write_all(state.fd, fallback.data(), fallback.size());
    }
}

}  
