#pragma once
#include "compiled_format.h"
#include <cstdint>
#include <functional>
#include <string>

namespace mmltk {

enum class CompileProgressPhase : uint8_t {
    kLabels,
    kPixels,
    kSyncing,
};

struct CompileProgress {
    size_t done = 0;
    size_t total = 0;
    CompileProgressPhase phase = CompileProgressPhase::kLabels;
    size_t active = 0;
    size_t dropped_annotations = 0;
};

struct CompilerConfig {
    std::string source_dir;
    std::string output_dir;
    std::string split;
    uint32_t target_width = 432;
    uint32_t target_height = 432;
    int num_workers = -1;
    int cuda_mask_batch_size = 0;
    int cuda_device_id = 0;
};

class DatasetCompiler {
   public:
    static void compile(const CompilerConfig& config,
                        const std::function<void(const CompileProgress&)>& progress_cb = nullptr);
};

}  // namespace mmltk
