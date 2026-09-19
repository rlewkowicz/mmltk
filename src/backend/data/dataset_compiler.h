#pragma once
#include "src/backend/imaging/resample/image_resize.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include "src/backend/data/catalog/class_catalog.h"
#include <utility>
#include <vector>
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/common/concurrency/cancellation_observation.h"
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_compile_phase.h"
namespace mmltk::backend::data {
namespace compiler_internal {
struct ProgressCounter;
}
struct CompileProgress {
    size_t done = 0;
    size_t total = 0;
    std::uint64_t elapsed_seconds = 0;
    std::uint64_t remaining_seconds = 0;
    std::uint64_t throughput_per_second = 0;
    DatasetCompilePhase phase = DatasetCompilePhase::Labels;
    size_t label_done = 0;
    size_t label_total = 0;
    size_t pixel_done = 0;
    size_t pixel_total = 0;
    size_t active_workers = 0;
    std::uint64_t dropped_instances = 0;
};
struct ProgressEstimate final {
    std::uint64_t remaining_seconds = 0U;
    std::uint64_t throughput_per_second = 0U;
    bool operator==(const ProgressEstimate&) const = default;
};
[[nodiscard]] ProgressEstimate estimate_progress(std::uint64_t completed, std::uint64_t total, std::uint64_t elapsed_seconds) noexcept;
struct CompileProgressObserver final {
    void* context = nullptr;
    void (*report)(void*, const CompileProgress&) noexcept = nullptr;
    void operator()(const CompileProgress& progress) const noexcept { report(context, progress); }
};
class CompileTelemetry {
   public:
    using Clock = std::chrono::steady_clock;
    using Now = Clock::time_point (*)() noexcept;
    CompileTelemetry() noexcept : started_at_rep_(now_().time_since_epoch().count()) {}
    explicit CompileTelemetry(size_t num_images, CompileProgressObserver observer = {}, Now now = &Clock::now) noexcept
        : num_images_(num_images), observer_(observer), now_(now), started_at_rep_(now_().time_since_epoch().count()) {}
    [[nodiscard]] CompileProgress snapshot() const noexcept;

   private:
    struct alignas(64) StageProgress {
        std::atomic<size_t> done{0U};
        std::atomic<size_t> active_workers{0U};
    };
    void reset(size_t num_images) noexcept;
    void add_label_done(size_t count) noexcept;
    void add_pixel_done(size_t count) noexcept;
    void enter_labels() noexcept;
    void enter_pixels() noexcept;
    void begin_label_worker() noexcept;
    void end_label_worker() noexcept;
    void begin_pixel_worker() noexcept;
    void end_pixel_worker() noexcept;
    void enter_syncing() noexcept;
    void enter_publishing() noexcept;
    void set_dropped_instances(std::uint64_t count) noexcept;
    [[nodiscard]] CompileProgress snapshot(DatasetCompilePhase phase) const noexcept;
    void publish_semantic(DatasetCompilePhase phase) noexcept;
    void publish_counter() noexcept;
    StageProgress labels_;
    StageProgress pixels_;
    std::atomic<size_t> num_images_{0U};
    std::atomic<DatasetCompilePhase> phase_{DatasetCompilePhase::Planning};
    std::atomic<std::uint64_t> dropped_instances_{0U};
    CompileProgressObserver observer_{};
    std::atomic<std::size_t> published_counter_quantum_{0U};
    std::mutex publication_mutex_;
    const Now now_ = &Clock::now;
    std::atomic<Clock::duration::rep> started_at_rep_;
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
    // CLEANUP-IGNORE: Diagnostic bounding boxes are fixed ABI facts, unrelated to reflected settings paths.
    std::array<double, 4> declared_bbox{};
    // CLEANUP-IGNORE: The measured mask box is a separate compiler diagnostic fact with the same fixed extent.
    std::array<double, 4> mask_bbox{};
};
struct CompilerConfig {
    mmltk::backend::imaging::resample::ImageResizeMode resize_mode = mmltk::backend::imaging::resample::ImageResizeMode::Stretch;
    bool perceptual_downscale = false;
    // CLEANUP-IGNORE: Compiler input/output paths remain backend execution facts with canonical reflected limits.
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string source_dir;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::string output_dir;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string split;
    [[= mmltk::frameworks::reflection::Minimum<std::uint32_t>{
        1U}]][[= mmltk::frameworks::reflection::Maximum<std::uint32_t>{MAX_IMAGE_EXTENT}]] uint32_t target_width = 432;
    [[= mmltk::frameworks::reflection::Minimum<std::uint32_t>{
        1U}]][[= mmltk::frameworks::reflection::Maximum<std::uint32_t>{MAX_IMAGE_EXTENT}]] uint32_t target_height = 432;
    // CLEANUP-IGNORE: Compiler worker controls are backend execution fields, distinct from training request controls.
    [[= mmltk::frameworks::reflection::Minimum<int>{-1}]] int num_workers = -1;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int cuda_mask_batch_size = 0;
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]] int cuda_device_id = 0;
    [[= mmltk::frameworks::reflection::MaxItems{mmltk::frameworks::reflection::kMaximumCpuIdentifiers}]] std::vector<int> worker_cpus;
    std::vector<CompileDiagnostic>* diagnostics = nullptr;
};
MMLTK_REFLECT_FIELDS(CompilerConfig)
enum class CompilerConfigViolation : std::uint8_t {
    MissingStorage,
    InvalidStorage,
    InvalidWorkerCpu,
};
static_assert(mmltk::frameworks::reflection::ingress_policies_are_valid<CompilerConfig>());
[[nodiscard]] inline std::expected<void, CompilerConfigViolation> validate_compiler_config(const CompilerConfig& config) noexcept {
    if (config.source_dir.empty() || config.output_dir.empty()) { return std::unexpected(CompilerConfigViolation::MissingStorage); }
    if (mmltk::frameworks::reflection::validate_reflected_fields(config)) { return std::unexpected(CompilerConfigViolation::InvalidStorage); }
    if (!mmltk::frameworks::reflection::unique_nonnegative_identifiers(config.worker_cpus)) {
        return std::unexpected(CompilerConfigViolation::InvalidWorkerCpu);
    }
    return {};
}
[[nodiscard]] inline constexpr std::string_view compiler_config_validation_message(const CompilerConfigViolation violation) noexcept {
    switch (violation) {
        case CompilerConfigViolation::MissingStorage: return "dataset compilation requires source and output directories";
        case CompilerConfigViolation::InvalidStorage: return "dataset compilation field violates its reflected application policy";
        case CompilerConfigViolation::InvalidWorkerCpu: return "dataset compilation worker CPUs must be unique non-negative identifiers";
    }
    return "invalid dataset compiler configuration";
}
struct DatasetCompileSplitPlan {
    std::string split;
    std::uint32_t image_count = 0;
};
struct DatasetCompilePlan {
    CompilerConfig config;
    catalog::ClassCatalog class_catalog;
    std::uint8_t source_category_base = 0;
    std::vector<DatasetCompileSplitPlan> splits;
    [[nodiscard]] size_t total_steps() const noexcept {
        size_t total = 0;
        for (const DatasetCompileSplitPlan& split : splits) { total += static_cast<size_t>(split.image_count) * 2U + 1U; }
        return total;
    }
};
class DatasetCompiler {
   public:
    static DatasetCompilePlan prepare(CompilerConfig config, const std::vector<std::string>& splits,
                                      mmltk::common::concurrency::CancellationObservation cancellation = {});
    static void compile(const DatasetCompilePlan& plan, size_t split_index, CompileTelemetry* telemetry = nullptr,
                        mmltk::common::concurrency::CancellationObservation cancellation = {});
};
}  // namespace mmltk::backend::data
