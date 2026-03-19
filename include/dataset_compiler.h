#pragma once
#include "compiled_format.h"
#include <functional>
#include <string>

namespace fastloader {

struct CompilerConfig {
    std::string source_dir;     // e.g. /path/to/seg-medium-synth
    std::string output_dir;     // where to write .bin files
    std::string split;          // "train" or "val"
    uint32_t target_width = 432;
    uint32_t target_height = 432;
    int num_workers = -1;       // -1 = cores - 2
};

// Compiles raw PNG+JSONL dataset into mmap-friendly binary.
// Single pass O(n): scan files -> parse labels -> decode+resize PNGs -> write binary.
class DatasetCompiler {
public:
    static void compile(const CompilerConfig& config,
                        const std::function<void(size_t done, size_t total)>& progress_cb = nullptr);
};

} // namespace fastloader
