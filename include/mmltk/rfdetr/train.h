#pragma once

#include "mmltk/rfdetr/artifacts.h"
#include "mmltk/rfdetr/evaluation.h"
#include "mmltk/rfdetr/train_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mmltk::rfdetr {

struct TrainOptimizerTuningConfig : TrainHyperparameterConfig {
    double clip_max_norm = 0.1;
};

template <typename PathType, typename SizeType, typename OptimizerType>
struct TrainRuntimeSharedConfig {
    PathType train_compiled_path;
    PathType val_compiled_path;
    PathType test_compiled_path;
    PathType weights_path;
    PathType resume_path;
    SizeType batch_size = static_cast<SizeType>(1);
    SizeType val_batch_size = static_cast<SizeType>(0);
    SizeType eval_max_dets = static_cast<SizeType>(500);
    std::string cpu_affinity;
    std::string lr_scheduler = "step";
    int epochs = 1;
    int grad_accum_steps = 1;
    int lr_drop = 100;
    int print_freq = 100;
    int prefetch_factor = 2;
    int seed = 42;
    int workers = 0;
    int lanes = 0;
    bool use_ema = false;
    bool validation_loss = false;
    bool validation_profile = false;
    bool amp = true;
    bool progress_bar = true;
    bool freeze_encoder = false;
    GpuAugmentationConfig gpu_augmentation;
    OptimizerType optimizer = OptimizerType{};
    int resolution = 0;
};

using TrainOptionsSharedConfig = TrainRuntimeSharedConfig<std::filesystem::path, std::size_t, TrainOptimizerKind>;

struct TrainOptions : TrainOptimizerTuningConfig, TrainOptionsSharedConfig {
    std::filesystem::path output_dir;
    std::string preset_name;
    double ema_decay = 0.993;
    std::filesystem::path distributed_store_path;
    std::vector<int> device_ids;
    int ema_tau = 100;
    int device_id = 0;
    int distributed_rank = 0;
    int distributed_world_size = 1;
    bool fused_optimizer = true;
    bool distributed_worker = false;
    CompilationMode compilation_mode = CompilationMode::kSelective;
};

[[nodiscard]] constexpr const char* train_optimizer_cli_value(const TrainOptimizerKind kind) noexcept {
    switch (kind) {
        case TrainOptimizerKind::AdamW:
            return "adamw";
        case TrainOptimizerKind::Muon:
            return "muon";
    }
    return "adamw";
}

[[nodiscard]] constexpr int compilation_mode_index(const CompilationMode mode) noexcept {
    switch (mode) {
        case CompilationMode::kNone:
            return 0;
        case CompilationMode::kSelective:
            return 1;
        case CompilationMode::kFullTrace:
            return 2;
    }
    return 1;
}

[[nodiscard]] constexpr CompilationMode compilation_mode_from_index(const int index) noexcept {
    switch (index) {
        case 0:
            return CompilationMode::kNone;
        case 1:
            return CompilationMode::kSelective;
        case 2:
            return CompilationMode::kFullTrace;
        default:
            return CompilationMode::kSelective;
    }
}

[[nodiscard]] constexpr const char* compilation_mode_cli_value(const CompilationMode mode) noexcept {
    switch (mode) {
        case CompilationMode::kNone:
            return "none";
        case CompilationMode::kSelective:
            return "selective";
        case CompilationMode::kFullTrace:
            return "full";
    }
    return "selective";
}

[[nodiscard]] constexpr const char* compilation_mode_display_label(const CompilationMode mode) noexcept {
    switch (mode) {
        case CompilationMode::kNone:
            return "None";
        case CompilationMode::kSelective:
            return "Selective";
        case CompilationMode::kFullTrace:
            return "Full";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* compilation_mode_display_label_from_index(const int index) noexcept {
    switch (index) {
        case 0:
        case 1:
        case 2:
            return compilation_mode_display_label(compilation_mode_from_index(index));
        default:
            return "Unknown";
    }
}

struct TrainEpochSummary {
    int epoch = 0;
    double train_loss = 0.0;
    std::optional<double> val_loss;
    EvalSummary val_summary;
    std::optional<double> ema_val_loss;
    std::optional<EvalSummary> ema_val_summary;
};

struct TrainRunResult {
    ResolvedModelArtifacts artifacts;
    GpuAugmentationConfig gpu_augmentation;
    std::filesystem::path output_dir;
    std::filesystem::path checkpoint_path;
    std::optional<std::filesystem::path> best_checkpoint_path;
    std::optional<std::filesystem::path> best_regular_checkpoint_path;
    std::optional<std::filesystem::path> best_ema_checkpoint_path;
    bool best_is_ema = false;
    int last_epoch = -1;
    std::vector<TrainEpochSummary> history;
    std::optional<EvalSummary> test_summary;
};

TrainRunResult run_training(const TrainOptions& options);
void print_training_summary(const TrainOptions& options, const TrainRunResult& result);

}  
