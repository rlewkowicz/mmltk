#pragma once
#include <array>
#include <filesystem>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/cancellation_observation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/controller/contracts/artifact.h"
#include "src/backend/data/catalog/class_catalog.h"
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/model.h"
namespace mmltk::backend::data {
struct BenchmarkCompileProgress;
struct CompileProgress;
}  // namespace mmltk::backend::data
namespace mmltk::controller::services {
[[nodiscard]] mmltk::controller::contracts::ArtifactProgress project_artifact_progress(const mmltk::backend::data::CompileProgress& value);
[[nodiscard]] mmltk::controller::contracts::ArtifactProgress project_artifact_progress(const mmltk::backend::data::BenchmarkCompileProgress& value);
struct ArtifactCancellationTag;
using ArtifactCancellationSource = mmltk::common::concurrency::EventCancellationSource<ArtifactCancellationTag, true>;
using ArtifactCancellationToken = mmltk::common::concurrency::EventCancellationToken<ArtifactCancellationTag>;
enum class ArtifactCompileKind : std::uint8_t {
    Directory,
    Benchmark,
};
struct ArtifactCompileRequest final {
    ArtifactCompileKind kind = ArtifactCompileKind::Directory;
    std::filesystem::path source;
    std::filesystem::path output;
    std::string preset;
    std::uint32_t resolution = 0U;
    bool overwrite = false;
    bool perceptual_downscale = false;
    [[nodiscard]] bool valid() const noexcept;
};
struct ArtifactCompileMaterializationError final {
    std::string detail;
};
[[nodiscard]] std::expected<ArtifactCompileRequest, ArtifactCompileMaterializationError> materialize_artifact_compile(
    const mmltk::controller::contracts::GuiSettingsState& settings) noexcept;
// A synchronous borrowed observer: ArtifactStore never retains caller work or
// a callback beyond compile's dynamic extent.
struct ArtifactProgressObserver final {
    void* context = nullptr;
    void (*report)(void*, const mmltk::controller::contracts::ArtifactProgress&) = nullptr;
    void operator()(const mmltk::controller::contracts::ArtifactProgress& value) const noexcept {
        if (report) report(context, value);
    }
};
struct ArtifactBenchmarkTraceObserver final {
    const void* context = nullptr;
    void (*report)(const void*, std::string_view, std::string_view) noexcept = nullptr;
    void operator()(const std::string_view event, const std::string_view fields) const noexcept {
        if (report != nullptr) report(context, event, fields);
    }
};
struct ArtifactDiagnosticObserver final {
    ArtifactBenchmarkTraceObserver benchmark{};
};
// Borrowed physical operations seam. ArtifactStore alone dispatches compile
// kinds and retains all request, conversion, inspection, and terminal policy.
class ArtifactCompilerOperations {
   public:
    virtual ~ArtifactCompilerOperations() = default;

   private:
    friend class ArtifactStore;
    virtual void compile_directory(const std::filesystem::path& source, const std::filesystem::path& output, std::uint32_t resolution,
                                   bool perceptual_downscale, mmltk::common::concurrency::CancellationObservation, ArtifactProgressObserver) const = 0;
    virtual void compile_benchmark(const std::filesystem::path& output, std::uint32_t resolution, bool perceptual_downscale,
                                   mmltk::common::concurrency::CancellationObservation, ArtifactProgressObserver, ArtifactBenchmarkTraceObserver) const = 0;
};
struct ArtifactCompileResult final {
    std::filesystem::path output;
    bool cancelled = false;
    mmltk::controller::contracts::ArtifactInspection inspection{};
};
struct ArtifactWeightAsset final {
    std::string filename;
    std::string url;
    std::string md5;
};
struct ArtifactWeightProgressObserver final {
    void* context = nullptr;
    void (*report)(void*, const mmltk::controller::contracts::ModelProgress&) noexcept = nullptr;
    void operator()(const mmltk::controller::contracts::ModelProgress& value) const noexcept {
        if (report != nullptr) report(context, value);
    }
};
// The sole acquisition seam. Production resolves the RF-DETR catalog and
// performs libcurl transfer; ArtifactStore owns cache policy and integrity.
class ArtifactWeightOperations {
   public:
    virtual ~ArtifactWeightOperations() = default;
    [[nodiscard]] virtual std::optional<ArtifactWeightAsset> find(std::string_view preset) const = 0;
    virtual void download(std::string_view url, const std::filesystem::path& output, const ArtifactCancellationToken& cancellation,
                          ArtifactWeightProgressObserver) const = 0;
};
// The sole owner of filesystem artifact inspection, compilation, and canonical
// RF-DETR weight acquisition. Its values are ordinary work results.
class ArtifactStore final {
   public:
    ArtifactStore();
    explicit ArtifactStore(std::filesystem::path cache_root);
    ArtifactStore(std::filesystem::path cache_root, const ArtifactWeightOperations& operations);
    ArtifactStore(std::filesystem::path cache_root, const ArtifactWeightOperations&, const ArtifactCompilerOperations&);
    ArtifactStore(std::filesystem::path, const ArtifactWeightOperations&, ArtifactCompilerOperations&&) = delete;
    [[nodiscard]] mmltk::controller::contracts::ArtifactInspection inspect(
        const std::array<std::filesystem::path, mmltk::controller::contracts::kArtifactSplitCapacity>& paths, std::string_view preset, std::uint32_t resolution,
        const ArtifactCancellationToken& cancellation) const;
    [[nodiscard]] ArtifactCompileResult compile(const ArtifactCompileRequest& request, const ArtifactCancellationToken& cancellation,
                                                ArtifactProgressObserver progress = {}, ArtifactDiagnosticObserver diagnostics = {}) const;
    [[nodiscard]] std::filesystem::path canonical_weight_path(std::string_view preset) const;
    [[nodiscard]] std::filesystem::path canonical_weight_path(std::string_view preset, const ArtifactCancellationToken& cancellation,
                                                              ArtifactWeightProgressObserver progress = {}) const;

   private:
    std::filesystem::path cache_root_;
    const ArtifactWeightOperations* weight_operations_ = nullptr;
    const ArtifactCompilerOperations* compiler_operations_;
};
}  // namespace mmltk::controller::services
