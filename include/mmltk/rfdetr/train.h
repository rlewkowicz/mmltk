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

struct TrainOptions {
    size_t batch_size = 1;
    size_t val_batch_size = 0;
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
    double ema_decay = 0.993;
    size_t eval_max_dets = 500;
    std::string cpu_affinity;
    std::string lr_scheduler = "step";
    std::filesystem::path train_compiled_path;
    std::filesystem::path val_compiled_path;
    std::filesystem::path test_compiled_path;
    std::filesystem::path output_dir;
    std::filesystem::path weights_path;
    std::filesystem::path resume_path;
    std::filesystem::path distributed_store_path;
    std::vector<int> device_ids;
    int epochs = 1;
    int grad_accum_steps = 1;
    int lr_drop = 100;
    int ema_tau = 100;
    int print_freq = 100;
    int prefetch_factor = 2;
    int seed = 42;
    int device_id = 0;
    int workers = 0;
    int lanes = 0;
    int distributed_rank = 0;
    int distributed_world_size = 1;
    bool use_ema = false;
    bool amp = true;
    bool progress_bar = true;
    bool fused_optimizer = true;
    bool distributed_worker = false;
    bool freeze_encoder = false;
    TrainOptimizerKind optimizer = TrainOptimizerKind::AdamW;
    CompilationMode compilation_mode = CompilationMode::kSelective;
};

struct TrainEpochSummary {
    int epoch = 0;
    double train_loss = 0.0;
    double val_loss = 0.0;
    EvalSummary val_summary;
    std::optional<double> ema_val_loss;
    std::optional<EvalSummary> ema_val_summary;
};

struct TrainRunResult {
    ResolvedModelArtifacts artifacts;
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

} // namespace mmltk::rfdetr
