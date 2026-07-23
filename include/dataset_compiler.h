#pragma once
#include "compiled_format.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mmltk {

namespace compiler_internal {
struct ProgressCounter;
}

enum class CompileProgressPhase : uint8_t {
    kPlanning,
    kLabels,
    kPixels,
    kSyncing,
    kPublishing,
};

struct CompileProgress {
    size_t done = 0;
    size_t total = 0;
    CompileProgressPhase phase = CompileProgressPhase::kLabels;
    size_t label_done = 0;
    size_t label_total = 0;
    size_t pixel_done = 0;
    size_t pixel_total = 0;
    size_t active_workers = 0;
    std::uint64_t dropped_instances = 0;
};

class CompileTelemetry {
   public:
    CompileTelemetry() = default;
    explicit CompileTelemetry(size_t num_images) noexcept {
        reset(num_images);
    }

    [[nodiscard]] CompileProgress snapshot() const noexcept;

   private:
    struct alignas(64) StageProgress {
        std::atomic<size_t> done{0U};
        std::atomic<size_t> active_workers{0U};
    };

    void reset(size_t num_images) noexcept;
    void add_label_done(size_t count) noexcept;
    void add_pixel_done(size_t count) noexcept;
    void begin_label_worker() noexcept;
    void end_label_worker() noexcept;
    void begin_pixel_worker() noexcept;
    void end_pixel_worker() noexcept;
    void enter_syncing() noexcept;
    void set_dropped_instances(std::uint64_t count) noexcept;

    StageProgress labels_;
    StageProgress pixels_;
    std::atomic<size_t> num_images_{0U};
    std::atomic<CompileProgressPhase> phase_{CompileProgressPhase::kPlanning};
    std::atomic<std::uint64_t> dropped_instances_{0U};

    friend class DatasetCompiler;
    friend struct compiler_internal::ProgressCounter;
};

enum class CompileDiagnosticKind : uint8_t {
    kDroppedInstanceAfterResize,
    kSourceBoundingBoxMismatch,
};

struct CompileDiagnostic {
    CompileDiagnosticKind kind = CompileDiagnosticKind::kDroppedInstanceAfterResize;
    std::string annotation_path;
    std::string class_name;
    size_t line = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t target_width = 0;
    uint32_t target_height = 0;
    size_t source_foreground = 0;
    std::array<int64_t, 4> declared_bbox{};
    std::array<int64_t, 4> mask_bbox{};
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
    std::vector<int> worker_cpus;
    std::atomic<bool>* cancel_requested = nullptr;
    std::vector<CompileDiagnostic>* diagnostics = nullptr;
};

struct DatasetCompileSplitPlan {
    std::string split;
    std::uint32_t image_count = 0;
};

struct DatasetCompilePlan {
    CompilerConfig config;
    std::unordered_map<std::string, std::uint8_t> class_map;
    std::vector<DatasetCompileSplitPlan> splits;

    [[nodiscard]] size_t total_steps() const noexcept {
        size_t total = 0;
        for (const DatasetCompileSplitPlan& split : splits) {
            total += static_cast<size_t>(split.image_count) * 2U + 1U;
        }
        return total;
    }
};

class DatasetCompiler {
   public:
    static DatasetCompilePlan prepare(CompilerConfig config, std::vector<std::string> splits);
    static void compile(const DatasetCompilePlan& plan, size_t split_index, CompileTelemetry* telemetry = nullptr);
};

}  
