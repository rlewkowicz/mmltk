#pragma once

#include "fastloader/rfdetr/evaluation.h"
#include "fastloader/rfdetr/model_info.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fastloader::rfdetr {

enum class ValidationLogMode : std::uint8_t {
    Quiet,
    Interactive,
};

struct ValidationOptions {
    std::filesystem::path compiled_path;
    std::filesystem::path source_dir;
    std::filesystem::path onnx_path;
    std::filesystem::path tensorrt_path;
    std::filesystem::path save_engine_path;
    std::filesystem::path report_json_path;
    std::string split;
    std::string eval_order = "onnx,tensorrt";
    uint32_t resolution = 432;
    size_t limit_images = 0;
    size_t alignment_images = 16;
    size_t eval_max_dets = 500;
    size_t batch_size = 1;
    size_t prefetch_factor = 2;
    int device_id = 0;
    int workers = 0;
    int compile_workers = -1;
    int compile_cuda_mask_batch_size = 0;
    int compile_cuda_device_id = 0;
    std::string cpu_affinity;
    bool recompile = false;
    bool profile = false;
    bool allow_fp16 = true;
    bool write_report_json = true;
    ValidationLogMode log_mode = ValidationLogMode::Interactive;
};

struct PhaseTiming {
    double seconds = 0.0;
    double img_per_s = 0.0;
    size_t images = 0;
};

struct ValidationBackendResult {
    ModelInfo model_info;
    EvalSummary summary;
    PhaseTiming timing;
};

struct ValidationDeltaSummary {
    double bbox_ap = 0.0;
    double bbox_ap50 = 0.0;
    double mask_ap = 0.0;
    double mask_ap50 = 0.0;
};

struct ValidationRunResult {
    size_t images = 0;
    size_t categories = 0;
    std::vector<std::string> eval_order;
    std::unordered_map<std::string, ValidationBackendResult> backends;
    std::optional<AlignmentStats> alignment_probe;
    std::optional<ValidationDeltaSummary> delta_tensorrt_minus_onnx;
    std::optional<PhaseTiming> total_timing;
};

ValidationRunResult run_validation(const ValidationOptions& options);
void write_validation_report(const ValidationOptions& options, const ValidationRunResult& result);
void print_model_metadata(const ModelInfo& info, size_t images, size_t categories, ValidationLogMode log_mode);
void print_validation_run_summary(const ValidationOptions& options, const ValidationRunResult& result);

} // namespace fastloader::rfdetr
