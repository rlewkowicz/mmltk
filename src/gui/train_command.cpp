#include "train_command.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace fastloader::gui {

namespace {

void append_bool_flag(std::vector<std::string>& args, bool value, std::string_view enabled, std::string_view disabled) {
    args.emplace_back(value ? enabled : disabled);
}

void append_option(std::vector<std::string>& args, std::string_view name, const std::string& value) {
    if (value.empty()) {
        return;
    }
    args.emplace_back(name);
    args.push_back(value);
}

void append_option(std::vector<std::string>& args, std::string_view name, int value) {
    args.emplace_back(name);
    args.push_back(std::to_string(value));
}

void append_option(std::vector<std::string>& args, std::string_view name, double value) {
    args.emplace_back(name);
    args.push_back(std::to_string(value));
}

std::string join_device_ids(const std::vector<int>& device_ids) {
    std::string joined;
    for (size_t index = 0; index < device_ids.size(); ++index) {
        if (index > 0) {
            joined.push_back(',');
        }
        joined += std::to_string(device_ids[index]);
    }
    return joined;
}

} // namespace

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
        cudaDeviceProp properties {};
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

std::vector<std::string> build_train_command_arguments(const TrainCommandConfig& config) {
    if (config.device_ids.empty()) {
        throw std::runtime_error("local GUI train requires at least one selected CUDA device");
    }

    std::vector<std::string> args;
    args.reserve(80);
    args.emplace_back("rfdetr");
    args.emplace_back("train");
    append_option(args, "--train-compiled", config.train_compiled_path);
    append_option(args, "--val-compiled", config.val_compiled_path);
    append_option(args, "--test-compiled", config.test_compiled_path);
    append_option(args, "--output-dir", config.output_dir);
    if (config.input_mode == TrainCommandInputMode::Weights) {
        append_option(args, "--weights", config.weights_path);
    } else {
        append_option(args, "--resume", config.resume_path);
    }

    append_option(args, "--batch-size", config.batch_size);
    append_option(args, "--val-batch-size", config.val_batch_size);
    append_option(args, "--epochs", config.epochs);
    append_option(args, "--grad-accum-steps", config.grad_accum_steps);
    append_option(args, "--optimizer", config.optimizer);
    append_option(args, "--lr", config.lr);
    append_option(args, "--lr-encoder", config.lr_encoder);
    append_option(args, "--lr-component-decay", config.lr_component_decay);
    append_option(args, "--encoder-layer-decay", config.encoder_layer_decay);
    append_option(args, "--momentum", config.momentum);
    append_option(args, "--weight-decay", config.weight_decay);
    append_option(args, "--lr-drop", config.lr_drop);
    append_option(args, "--lr-scheduler", config.lr_scheduler);
    append_option(args, "--lr-min-factor", config.lr_min_factor);
    append_option(args, "--warmup-epochs", config.warmup_epochs);
    append_option(args, "--warmup-momentum", config.warmup_momentum);
    append_option(args, "--clip-max-norm", config.clip_max_norm);
    append_option(args, "--eval-max-dets", config.eval_max_dets);
    append_option(args, "--workers", config.workers);
    append_option(args, "--lanes", config.lanes);
    append_option(args, "--prefetch-factor", config.prefetch_factor);
    append_option(args, "--seed", config.seed);
    append_option(args, "--cpu-affinity", config.cpu_affinity);
    append_bool_flag(args, config.amp, "--amp", "--no-amp");
    append_bool_flag(args, config.use_ema, "--use-ema", "--no-ema");
    append_bool_flag(args, config.progress_bar, "--progress", "--no-progress");
    append_bool_flag(args, config.freeze_encoder, "--freeze-encoder", "--no-freeze-encoder");
    append_option(args, "--compile-mode", config.compile_mode);

    if (config.device_ids.size() == 1U) {
        append_option(args, "--device-id", config.device_ids.front());
    } else {
        append_option(args, "--device-ids", join_device_ids(config.device_ids));
    }

    return args;
}

std::filesystem::path current_executable_path() {
    std::vector<char> buffer(1024, '\0');
    while (true) {
        const ssize_t bytes = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (bytes < 0) {
            throw std::runtime_error(std::string("failed to resolve current executable path: ") + std::strerror(errno));
        }
        if (static_cast<size_t>(bytes) < buffer.size() - 1U) {
            buffer[static_cast<size_t>(bytes)] = '\0';
            return std::filesystem::path(buffer.data());
        }
        buffer.resize(buffer.size() * 2U, '\0');
    }
}

std::filesystem::path resolve_sibling_fastloader_cli(const std::filesystem::path& executable_path) {
    const std::filesystem::path cli_path = executable_path.parent_path() / "fastloader_cli";
    if (!std::filesystem::exists(cli_path)) {
        throw std::runtime_error("failed to locate sibling fastloader_cli next to fastloader_gui: " + cli_path.string());
    }
    return cli_path;
}

} // namespace fastloader::gui
