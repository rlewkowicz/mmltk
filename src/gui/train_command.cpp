#include "train_command.h"
#include "runtime_paths.h"
#include "rfdetr/cli/workflow_cli_shared.h"
#include "mmltk/rfdetr/workflow_requests.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace mmltk::gui {

std::vector<LocalGpuInfo> enumerate_local_gpus(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }

    int device_count = 0;
    const cudaError_t count_status = ::cudaGetDeviceCount(&device_count);
    if (count_status != cudaSuccess) {
        if (error != nullptr) {
            *error = std::string("failed to enumerate visible CUDA devices: ") + ::cudaGetErrorString(count_status);
        }
        return {};
    }

    std::vector<LocalGpuInfo> devices;
    devices.reserve(static_cast<size_t>(std::max(0, device_count)));
    for (int device_id = 0; device_id < device_count; ++device_id) {
        cudaDeviceProp properties{};
        const cudaError_t property_status = ::cudaGetDeviceProperties(&properties, device_id);
        if (property_status != cudaSuccess) {
            if (error != nullptr) {
                *error = std::string("failed to read CUDA device properties: ") + ::cudaGetErrorString(property_status);
            }
            return {};
        }
        LocalGpuInfo info;
        info.device_id = device_id;
        info.name = properties.name;
        info.total_memory_bytes = static_cast<std::uint64_t>(properties.totalGlobalMem);
        devices.push_back(std::move(info));
    }

    return devices;
}

std::vector<std::string> build_train_command_arguments(const mmltk::rfdetr::TrainRequest& request,
                                                       const std::string_view fallback_preset_name) {
    mmltk::rfdetr::cli_shared::TrainCommandSpec spec;
    spec.request = request;
    spec.device_ids_text = mmltk::rfdetr::cli_shared::join_device_ids(request.device_ids);
    spec.compile_mode = mmltk::rfdetr::compilation_mode_cli_value(request.compilation_mode);
    spec.optimizer = mmltk::rfdetr::train_optimizer_cli_value(request.optimizer);

    if (!fallback_preset_name.empty() ||
        !mmltk::rfdetr::cli_shared::infer_train_recipe_preset_name_with_native_fallback(request).empty()) {
        mmltk::rfdetr::cli_shared::finalize_train_command(spec, fallback_preset_name);
    }

    return mmltk::rfdetr::cli_shared::serialize_train_command_arguments(spec.request);
}

std::filesystem::path current_executable_path() {
    return mmltk::runtime_paths::current_executable_path();
}

std::filesystem::path resolve_sibling_mmltk_cli(const std::filesystem::path& executable_path) {
    const std::filesystem::path cli_path = executable_path.parent_path() / "mmltk";
    if (!std::filesystem::exists(cli_path)) {
        throw std::runtime_error("failed to locate sibling mmltk next to mmltk-gui: " + cli_path.string());
    }
    return cli_path;
}

}  
