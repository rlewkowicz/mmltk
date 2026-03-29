#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fastloader::gui {

enum class TrainCommandInputMode : int {
    Weights = 0,
    Resume = 1,
};

struct LocalGpuInfo {
    int device_id = -1;
    std::string name;
    std::uint64_t total_memory_bytes = 0;
};

struct TrainCommandConfig {
    std::string train_compiled_path;
    std::string val_compiled_path;
    std::string test_compiled_path;
    std::string output_dir;
    std::string weights_path;
    std::string resume_path;
    std::string cpu_affinity;
    TrainCommandInputMode input_mode = TrainCommandInputMode::Weights;
    std::vector<int> device_ids;
    int batch_size = 1;
    int val_batch_size = 0;
    int epochs = 1;
    int grad_accum_steps = 1;
    int eval_max_dets = 500;
    int lr_drop = 100;
    int prefetch_factor = 2;
    int seed = 42;
    int workers = 0;
    int lanes = 0;
    double lr = 1.0e-4;
    double lr_encoder = 1.5e-4;
    double lr_component_decay = 0.7;
    double encoder_layer_decay = 0.8;
    double momentum = 0.95;
    double weight_decay = 1.0e-4;
    double warmup_epochs = 0.0;
    double warmup_momentum = 0.0;
    double lr_min_factor = 0.0;
    double clip_max_norm = 0.1;
    bool use_ema = false;
    bool amp = true;
    bool progress_bar = false;
    bool freeze_encoder = false;
    std::string optimizer = "adamw";
    std::string lr_scheduler = "step";
    std::string compile_mode = "selective";
};

std::vector<LocalGpuInfo> enumerate_local_gpus(std::string* error);
std::vector<std::string> build_train_command_arguments(const TrainCommandConfig& config);
std::filesystem::path current_executable_path();
std::filesystem::path resolve_sibling_fastloader_cli(const std::filesystem::path& executable_path);

} // namespace fastloader::gui
