#pragma once

#include "src/acceptance/tests/async_test_utils.hpp"
#include "src/controller/subsystems/validate/validation_system.h"
#include "src/controller/subsystems/validate/validation_runtime.h"
#include "src/controller/subsystems/export/export_system.h"
#include "src/controller/subsystems/system/predict_system.h"
#include "src/controller/subsystems/system/detail/prediction_preview.h"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace mmltk::controller::test_support {

// Stop-aware, reusable admission gate; TestGate instead models one engagement.
class StopGate final {
 public:
    void Release();
    void Reset();
    [[nodiscard]] bool Wait(std::stop_token);
 private:
    std::mutex mutex_;
    std::condition_variable_any condition_;
    bool released_ = false;
};

// Owns actual source storage. The aliasing custody includes the allocating
// context, and can be dropped explicitly by a scenario observing weak lifetime.
class PredictionSource final {
 public:
    using Detection = mmltk::backend::models::rfdetr::Prediction;
    using Catalog = std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog>;
    static PredictionSource Device(const mmltk::frameworks::gpu::DeviceExecution&, VisualExtent,
        std::span<const float> pixels, std::vector<Detection> detections = {}, Catalog = {},
        std::span<const std::uint8_t> masks = {});
    static PredictionSource Decoded(VisualExtent, std::span<const std::uint8_t> pixels, Catalog = {});
    [[nodiscard]] const float* pixels() const noexcept { return pixels_; }
    [[nodiscard]] const std::uint8_t* rgb8() const noexcept { return rgb8_; }
    [[nodiscard]] VisualExtent extent() const noexcept { return extent_; }
    [[nodiscard]] const auto& detections() const noexcept { return detections_; }
    [[nodiscard]] const auto& annotations() const noexcept { return annotations_; }
    [[nodiscard]] const auto& classes() const noexcept { return classes_; }
    [[nodiscard]] std::shared_ptr<void>& custody() noexcept { return custody_; }
 private:
    PredictionSource(VisualExtent, Catalog);
    VisualExtent extent_;
    Catalog classes_;
    std::vector<Detection> detections_;
    mmltk::backend::ml::runtime::AnalysisAnnotationStorage annotations_{};
    std::shared_ptr<void> custody_;
    const float* pixels_ = nullptr;
    const std::uint8_t* rgb8_ = nullptr;
};

class PredictionReceiverFault final {
 public:
    mmltk::testsupport::TestGate upload{"prediction receiver upload"};
    bool terminal = false;
    std::atomic_bool enabled = false;
    std::atomic_bool partial_draw = false;
    std::atomic_uint draw_failures_remaining = 0U;
    std::atomic_size_t draws = 0U;
    std::atomic_size_t uploads = 0U;
    std::size_t fail_upload_at = 0U;
    PredictRuntime::PreviewRetirement retirement;
    std::weak_ptr<void> decoded;
    [[nodiscard]] static detail::PredictionPreviewPool::TransferOperations Operations();
 private:
    static cudaError_t Upload(void*, const void*, std::size_t, cudaMemcpyKind, cudaStream_t);
    static int Convert(const float*, std::uint32_t, std::uint32_t, std::uint8_t*, std::size_t, std::uintptr_t) noexcept;
};

// Declare before users; release/stop/join guards belong after their futures or
// systems. Restoration happens only after all callback users have gone away.
class ScopedPredictionReceiverFault final {
 public:
    explicit ScopedPredictionReceiverFault(PredictionReceiverFault&);
    ~ScopedPredictionReceiverFault();
    ScopedPredictionReceiverFault(const ScopedPredictionReceiverFault&) = delete;
    ScopedPredictionReceiverFault& operator=(const ScopedPredictionReceiverFault&) = delete;
 private:
    PredictionReceiverFault* previous_;
};

// Delegates real allocations/events/streams to CUDA. Failure is reported only
// after the actual output stream settles, preserving a safe test process.
class PredictionSettlementFault final {
 public:
    mmltk::testsupport::TestGate settlement{"composed outer settlement"};
    std::atomic_bool enabled = false;
    std::atomic_bool callback_returned = false;
    std::atomic_size_t streams_created = 0U;
    std::atomic_size_t streams_destroyed = 0U;
    [[nodiscard]] std::shared_ptr<mmltk::frameworks::gpu::ImageCopyBackend> Backend();
};

class PredictionTransferFault final {
 public:
    struct Selection { int fail_copy = 0; bool fail_record = false; bool fail_settle = false; bool fail_wait = false; };
    PredictionTransferFault();
    ~PredictionTransferFault();
    PredictionTransferFault(const PredictionTransferFault&) = delete;
    PredictionTransferFault& operator=(const PredictionTransferFault&) = delete;
    void Reset();
    void Reset(Selection);
    int copies = 0;
    int settlements = 0;
    int waits = 0;
    cudaStream_t waited_stream = nullptr;
    [[nodiscard]] static detail::PredictionPreviewPool::TransferOperations Operations();
 private:
    Selection selection_;
    PredictionTransferFault* previous_;
    static CUresult Copy(CUdeviceptr, CUcontext, CUdeviceptr, CUcontext, std::size_t, CUstream);
    static cudaError_t Record(cudaEvent_t, cudaStream_t);
    static cudaError_t Settle(cudaStream_t);
    static cudaError_t Wait(cudaStream_t, cudaEvent_t, unsigned);
};

[[nodiscard]] detail::PredictionPreviewPool::TransferOperations RefusePinnedRegistration();
void CountPredictionSourceStop(void*) noexcept;

class PredictionContextFault final {
 public:
    enum class Failure { None, Query, RestoreOnce, RestoreAlways };
    explicit PredictionContextFault(Failure failure) : failure_(failure) {}
    bool armed = false;
    bool observe_candidate = false;
    unsigned failures = 0U;
    unsigned calls = 0U;
    unsigned restores = 0U;
    CUcontext candidate = nullptr;
    [[nodiscard]] mmltk::frameworks::gpu::CudaContextApi Api() noexcept;
 private:
    Failure failure_;
    static CUresult Get(void*, CUcontext*) noexcept;
    static CUresult Set(void*, CUcontext) noexcept;
};

struct ComputeScenario final {
    std::shared_ptr<StopGate> gate;
    bool fail = false;
};
class ComputeSequence final {
 public:
    explicit ComputeSequence(ComputeScenario scenario) : scenario_(std::move(scenario)) {}
    contracts::ComputeTerminal Run(std::stop_token, const ComputeProgressSink&);
 private:
    ComputeScenario scenario_;
};
class FakeNonvisualComputeRuntime final : public ValidationRuntime, public ExportRuntime {
 public:
    explicit FakeNonvisualComputeRuntime(ComputeScenario scenario) : sequence_(std::move(scenario)) {}
    ValidationRuntimeResult Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token, const ComputeProgressSink&, const mmltk::backend::models::rfdetr::ValidationDelivery&) override;
    contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::ModelExportRequest, std::stop_token, const ComputeProgressSink&) override;
 private:
    ComputeSequence sequence_;
};
struct PredictionScenario final {
    ComputeScenario compute;
    std::shared_ptr<std::atomic_size_t> predictions;
    bool refuse_preview = false;
    std::shared_ptr<std::atomic_int64_t> source_index;
    std::size_t labels = 0U;
    std::shared_ptr<StopGate> after_product;
    std::shared_ptr<PredictionReceiverFault> receiver_fault;
};
class FakePredictRuntime final : public PredictRuntime {
 public:
    explicit FakePredictRuntime(PredictionScenario);
    contracts::ComputeTerminal Run(mmltk::backend::models::rfdetr::PredictRequest, std::stop_token,
        const ComputeProgressSink&, const ProductSink&, const PlaybackGate&, VisualExtent,
        const ContextProvider&, const PreviewRetirement&) override;
 private:
    ComputeSequence sequence_;
    PredictionScenario scenario_;
    std::unique_ptr<detail::PredictionPreviewPool> preview_;
};
} // namespace mmltk::controller::test_support
