#pragma once
#include <filesystem>
namespace mmltk::controller::services {
// Reserve an empty destination. Fresh runs always use atomically created numeric
// children; a manually selected empty root may be used directly when requested.
// Every admitted directory retains an exclusive claim so an empty run is never
// reused. Claims are filesystem state and survive process exit.
[[nodiscard]] std::filesystem::path reserve_run_output(const std::filesystem::path& root, bool use_empty_root = false);
}  // namespace mmltk::controller::services
