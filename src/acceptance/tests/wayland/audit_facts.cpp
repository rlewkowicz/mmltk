#include "audit_facts.h"
#include <filesystem>
#include "src/common/io/scoped_fd.h"
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
namespace mmltk::acceptance::wayland {
using mmltk::common::io::ScopedFd;
void append_acceptance_record(const std::filesystem::path& path, nlohmann::json record) {
    record["kind"] = "acceptance_runtime";
    record["owner"] = "acceptance";
    record["steady_ns"] = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string payload = record.dump() + '\n';
    ScopedFd output{::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC)};
    if (output.get() < 0) throw std::runtime_error("failed to append workspace Wayland acceptance evidence");
    std::size_t offset = 0U;
    while (offset != payload.size()) {
        ssize_t written = -1;
        do { written = ::write(output.get(), payload.data() + offset, payload.size() - offset); } while (written < 0 && errno == EINTR);
        if (written <= 0) throw std::runtime_error("failed to write workspace Wayland acceptance evidence");
        offset += static_cast<std::size_t>(written);
    }
}
}  // namespace mmltk::acceptance::wayland
