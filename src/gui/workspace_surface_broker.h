#pragma once

#include "mmltk/live/live_types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>

namespace mmltk::live {
class WorkspaceSurfacePool;
}

namespace mmltk::gui {

struct WorkspaceSurfacePresent {
    std::uint64_t generation = 0U;
    std::uint64_t revision = 0U;
    std::uint32_t slot = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    mmltk::live::LiveCaptureRegion source_region{};
    std::uint64_t capture_ns = 0U;
    std::uint64_t ready_ns = 0U;
};

class WorkspaceSurfaceBroker {
   public:
    WorkspaceSurfaceBroker(const std::filesystem::path& socket_path, std::string session_token);
    ~WorkspaceSurfaceBroker();

    WorkspaceSurfaceBroker(const WorkspaceSurfaceBroker&) = delete;
    WorkspaceSurfaceBroker& operator=(const WorkspaceSurfaceBroker&) = delete;

    [[nodiscard]] const std::filesystem::path& socket_path() const noexcept;
    [[nodiscard]] int poll_fd() const noexcept;
    void set_expected_process_group(pid_t process_group);
    void pump();
    void update_pool(std::shared_ptr<mmltk::live::WorkspaceSurfacePool> pool);
    [[nodiscard]] std::optional<WorkspaceSurfacePresent> poll_latest_present();
    void release(std::uint64_t generation, std::uint32_t slot, std::uint64_t revision) noexcept;
    void release_all() noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::optional<mmltk::live::WorkspaceSwapchainDescriptor> descriptor() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  
