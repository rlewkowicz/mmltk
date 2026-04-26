#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::rfdetr {

struct TrainRequest;

}

namespace mmltk::gui {

struct LocalGpuInfo {
    int device_id = -1;
    std::string name;
    std::uint64_t total_memory_bytes = 0;
};

std::vector<LocalGpuInfo> enumerate_local_gpus(std::string* error);
std::vector<std::string> build_train_command_arguments(const mmltk::rfdetr::TrainRequest& request,
                                                       std::string_view fallback_preset_name = {});
std::filesystem::path current_executable_path();
std::filesystem::path resolve_sibling_mmltk_cli(const std::filesystem::path& executable_path);

}  // namespace mmltk::gui
