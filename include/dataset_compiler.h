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
    std::string source_dir;     // e.g. /path/to/seg-medium-synth
    std::string output_dir;     // where to write .bin files
    std::string split;          // "train" or "val"
    uint32_t target_width = 432;
    uint32_t target_height = 432;
    int num_workers = -1;       // -1 = all available CPUs in the effective cpuset
    int cuda_mask_batch_size = 0; // 0 = disable CUDA mask resize batching
    int cuda_device_id = 0;
};

// Compiles raw PNG+JSONL dataset into mmap-friendly binary.
// Single pass O(n): scan files -> parse labels -> decode+resize PNGs -> write binary.
class DatasetCompiler {
public:
    static void compile(const CompilerConfig& config,
                        const std::function<void(const CompileProgress&)>& progress_cb = nullptr);
};

} // namespace mmltk
