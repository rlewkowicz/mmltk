#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <utility>
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/analysis_value_count.h"
#include "src/backend/data/catalog/class_catalog.h"
namespace mmltk::backend::ml::runtime {
inline constexpr std::size_t kMaximumAnalysisRank = 4U;
inline constexpr std::size_t kMaximumAnalysisRegions = 16U;
struct AnalysisIdentity final {
    std::uint64_t operation = 0U;
    std::uint64_t source = 0U;
    [[nodiscard]] constexpr bool valid() const noexcept { return operation != 0U && source != 0U; }
    bool operator==(const AnalysisIdentity&) const noexcept = default;
};
struct AnalysisRegion final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    [[nodiscard]] constexpr bool valid() const noexcept { return width != 0U && height != 0U; }
    bool operator==(const AnalysisRegion&) const noexcept = default;
};
enum class AnalysisElementType : std::uint8_t {
    Float16,
    Float32,
    Int32,
    Int64,
    Uint8,
};
struct AnalysisShape final {
    std::uint8_t rank = 0U;
    std::uint32_t extents[kMaximumAnalysisRank]{};
};
struct AnalysisDeviceBuffer final {
    std::uintptr_t address = 0U;
    std::size_t capacity_bytes = 0U;
    AnalysisShape shape{};
    AnalysisElementType element_type = AnalysisElementType::Float32;
};
struct AnalysisImageView final {
    AnalysisDeviceBuffer pixels{};
    std::size_t pitch_bytes = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint8_t channels = 0U;
    std::int32_t device = -1;
};
struct AnalysisCompletion final {
    std::int32_t device = -1;
    std::uintptr_t event = 0U;
    std::uintptr_t producer_stream = 0U;
    [[nodiscard]] constexpr bool valid() const noexcept { return device >= 0 && event != 0U && producer_stream != 0U; }
};
// This is the canonical model-neutral annotation storage vocabulary. RF-DETR
// writes its interpretation into these caller-owned buffers once; Live
// projects the same storage into its frame/overlay state without mirroring it.
struct AnalysisAnnotationStorage final {
    AnalysisRegion source_region{};
    std::size_t value_capacity = 0U;
    AnalysisValueCount count{};
    AnalysisDeviceBuffer boxes_xyxy{};
    AnalysisDeviceBuffer class_references{};
    AnalysisDeviceBuffer confidences{};
    AnalysisDeviceBuffer colors_rgb{};
    AnalysisDeviceBuffer masks{};
    // Capacity does not establish that a provider initialized a mask product.
    bool masks_available = false;
    mmltk::backend::data::catalog::ClassReferenceDomain class_domain = mmltk::backend::data::catalog::ClassReferenceDomain::RawOutputSlot;
    std::shared_ptr<const mmltk::backend::data::catalog::ClassCatalog> class_catalog{};
};
struct AnalysisRequest final {
    AnalysisIdentity identity{};
    std::uint64_t captured_ns = 0U;
    AnalysisImageView source{};
    AnalysisCompletion source_ready{};
    std::span<const AnalysisRegion> regions{};
    // The caller retains every allocation and descriptor until it explicitly
    // releases the completed AnalysisResult through the provider.
    std::span<AnalysisAnnotationStorage> annotations{};
    std::stop_token cancellation{};
};
enum class AnalysisTerminal : std::uint8_t {
    Completed,
    Refused,
    InvalidInput,
    Cancelled,
    DependencyFailure,
    // CLEANUP-IGNORE: AnalysisResult owns distinct provider-result custody despite a conventional move-only RAII shape.
    ExecutionFailure,
    // CLEANUP-IGNORE: The terminal enum boundary only coincidentally precedes another move-only runtime result.
};
class AnalysisProvider;
class AnalysisResult final {
   public:
    AnalysisResult() noexcept = default;
    AnalysisResult(const AnalysisResult&) = delete;
    AnalysisResult& operator=(const AnalysisResult&) = delete;
    AnalysisResult(AnalysisResult&& other) noexcept
        : owner_(std::move(other.owner_)),
          generation_(std::exchange(other.generation_, 0U)),
          identity_(std::exchange(other.identity_, {})),
          terminal_(other.terminal_),
          completed_ns_(std::exchange(other.completed_ns_, 0U)),
          output_count_(std::exchange(other.output_count_, 0U)),
          completion_(std::exchange(other.completion_, {})) {}
    AnalysisResult& operator=(AnalysisResult&&) = delete;
    ~AnalysisResult() noexcept;
    void Abandon() noexcept;
    [[nodiscard]] const AnalysisIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] AnalysisTerminal terminal() const noexcept { return terminal_; }
    [[nodiscard]] std::uint64_t completed_ns() const noexcept { return completed_ns_; }
    [[nodiscard]] std::size_t output_count() const noexcept { return output_count_; }
    [[nodiscard]] const AnalysisCompletion& completion() const noexcept { return completion_; }
    [[nodiscard]] bool owns_in_flight_work() const noexcept { return static_cast<bool>(owner_); }

   private:
    AnalysisResult(std::shared_ptr<AnalysisProvider> owner, std::uint64_t generation, AnalysisIdentity identity, AnalysisTerminal terminal,
                   std::uint64_t completed_ns, std::size_t output_count, AnalysisCompletion completion) noexcept
        : owner_(std::move(owner)),
          generation_(generation),
          identity_(identity),
          terminal_(terminal),
          completed_ns_(completed_ns),
          output_count_(output_count),
          completion_(completion) {}
    std::shared_ptr<AnalysisProvider> owner_;
    std::uint64_t generation_ = 0U;
    AnalysisIdentity identity_{};
    AnalysisTerminal terminal_ = AnalysisTerminal::InvalidInput;
    std::uint64_t completed_ns_ = 0U;
    std::size_t output_count_ = 0U;
    AnalysisCompletion completion_{};
    void Disarm() noexcept {
        owner_.reset();
        generation_ = 0U;
        completion_ = {};
    }
    friend class AnalysisProvider;
};
class AnalysisProvider : public std::enable_shared_from_this<AnalysisProvider> {
   public:
    AnalysisProvider() = default;
    virtual ~AnalysisProvider() {
        std::lock_guard lock(state_mutex_);
        state_ = ProviderState::Closed;
    }
    AnalysisProvider(const AnalysisProvider&) = delete;
    AnalysisProvider& operator=(const AnalysisProvider&) = delete;
    [[nodiscard]] AnalysisResult Analyze(const AnalysisRequest& request) noexcept {
        std::shared_ptr<AnalysisProvider> owner = weak_from_this().lock();
        if (!owner) { return Terminal(request.identity, AnalysisTerminal::DependencyFailure); }
        const std::uint64_t generation = ClaimIssuing();
        if (generation == 0U) { return Terminal(request.identity, AnalysisTerminal::Refused); }
        if (request.cancellation.stop_requested()) {
            RestoreIdleAfterIssue(generation);
            return Terminal(request.identity, AnalysisTerminal::Cancelled);
        }
        if (request.regions.empty() && request.annotations.empty()) {
            RestoreIdleAfterIssue(generation);
            return Terminal(request.identity, AnalysisTerminal::Refused);
        }
        if (!ValidateRequest(request)) {
            RestoreIdleAfterIssue(generation);
            return Terminal(request.identity, AnalysisTerminal::InvalidInput);
        }
        const ProviderWorkResult work = DoAnalyze(request);
        if (request.cancellation.stop_requested()) {
            RetireIssuedWork(work);
            RestoreIdleAfterIssue(generation);
            return Terminal(request.identity, AnalysisTerminal::Cancelled);
        }
        if (!ValidateResult(request, work)) {
            RetireIssuedWork(work);
            RestoreIdleAfterIssue(generation);
            return Terminal(request.identity, AnalysisTerminal::ExecutionFailure);
        }
        if (work.terminal != AnalysisTerminal::Completed) {
            RestoreIdleAfterIssue(generation);
            return Terminal(request.identity, work.terminal, work.completed_ns);
        }
        if (!PublishActive(generation, work)) {
            RetireIssuedWork(work);
            RestoreIdleAfterIssue(generation);
            return Terminal(request.identity, AnalysisTerminal::ExecutionFailure);
        }
        return AnalysisResult{std::move(owner), generation, work.identity, work.terminal, work.completed_ns, work.output_count, work.completion};
    }
    [[nodiscard]] bool ReleaseAfterCompletion(AnalysisResult&& result) noexcept {
        if (result.owner_.get() != this) { return false; }
        const std::shared_ptr<AnalysisProvider> keep_alive = result.owner_;
        const bool completed = SettleAnalysis(result.generation_, true);
        result.Disarm();
        static_cast<void>(keep_alive);
        if (!completed) {
            result.terminal_ = AnalysisTerminal::ExecutionFailure;
            result.completed_ns_ = 0U;
            result.output_count_ = 0U;
            result.completion_ = {};
        }
        return completed;
    }
    [[nodiscard]] bool Shutdown() noexcept {
        {
            std::lock_guard lock(state_mutex_);
            if (state_ == ProviderState::Closed) { return true; }
            if (state_ != ProviderState::Idle) { return false; }
            state_ = ProviderState::Settling;
        }
        DoShutdown();
        {
            std::lock_guard lock(state_mutex_);
            state_ = ProviderState::Closed;
        }
        return true;
    }

   protected:
    struct ProviderWorkResult final {
        AnalysisIdentity identity{};
        AnalysisTerminal terminal = AnalysisTerminal::ExecutionFailure;
        std::uint64_t completed_ns = 0U;
        std::size_t output_count = 0U;
        AnalysisCompletion completion{};
    };
    [[nodiscard]] virtual ProviderWorkResult DoAnalyze(const AnalysisRequest& request) noexcept = 0;
    [[nodiscard]] virtual bool ObserveCompletion(const AnalysisCompletion& completion) noexcept = 0;
    virtual void RetireIssuedWork(const ProviderWorkResult& work) noexcept = 0;
    virtual void DoShutdown() noexcept = 0;

   private:
    enum class ProviderState : std::uint8_t {
        Idle,
        Issuing,
        Active,
        Settling,
        Closed,
    };
    [[nodiscard]] static constexpr std::size_t ElementBytes(const AnalysisElementType type) noexcept {
        switch (type) {
            case AnalysisElementType::Float16: return 2U;
            case AnalysisElementType::Float32:
            case AnalysisElementType::Int32: return 4U;
            case AnalysisElementType::Int64: return 8U;
            case AnalysisElementType::Uint8: return 1U;
        }
        return 0U;
    }
    [[nodiscard]] static bool ValidateBuffer(const AnalysisDeviceBuffer& buffer, const std::uint8_t rank, const std::span<const std::uint32_t> extents,
                                             const AnalysisElementType type, const bool optional = false) noexcept {
        if (optional && buffer.address == 0U) { return buffer.capacity_bytes == 0U; }
        if (buffer.address == 0U || buffer.shape.rank != rank || extents.size() != rank || buffer.element_type != type) { return false; }
        std::size_t elements = 1U;
        for (std::size_t axis = 0U; axis < rank; ++axis) {
            if (extents[axis] == 0U || buffer.shape.extents[axis] != extents[axis] || extents[axis] > std::numeric_limits<std::size_t>::max() / elements) {
                return false;
            }
            elements *= extents[axis];
        }
        const std::size_t bytes = ElementBytes(type);
        return bytes != 0U && elements <= std::numeric_limits<std::size_t>::max() / bytes && buffer.capacity_bytes >= elements * bytes;
    }
    [[nodiscard]] static bool ValidateRequest(const AnalysisRequest& request) noexcept {
        if (!request.identity.valid() || request.source.device < 0 || request.source.width == 0U || request.source.height == 0U ||
            (request.source.channels != 3U && request.source.channels != 4U) || !request.source_ready.valid() ||
            request.source_ready.device != request.source.device || request.regions.empty() || request.regions.size() > kMaximumAnalysisRegions ||
            request.annotations.size() != request.regions.size()) {
            return false;
        }
        const std::uint32_t image_extents[]{
            request.source.height,
            request.source.width,
            request.source.channels,
        };
        if (!ValidateBuffer(request.source.pixels, 3U, image_extents, AnalysisElementType::Uint8)) { return false; }
        const std::size_t row_bytes = static_cast<std::size_t>(request.source.width) * request.source.channels;
        if (request.source.pitch_bytes < row_bytes || request.source.height > std::numeric_limits<std::size_t>::max() / request.source.pitch_bytes ||
            request.source.pixels.capacity_bytes < request.source.pitch_bytes * request.source.height) {
            return false;
        }
        for (std::size_t index = 0U; index < request.regions.size(); ++index) {
            const AnalysisRegion& region = request.regions[index];
            const AnalysisAnnotationStorage& output = request.annotations[index];
            if (!region.valid() || output.source_region != region || !output.count.empty() || region.width > request.source.width ||
                region.height > request.source.height || region.x > request.source.width - region.width || region.y > request.source.height - region.height ||
                !ValidateAnnotationStorage(output)) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] static bool ValidateAnnotationStorage(const AnalysisAnnotationStorage& output) noexcept {
        if (output.value_capacity > std::numeric_limits<std::uint32_t>::max()) { return false; }
        if (output.value_capacity == 0U) {
            return output.count.empty() && !output.masks_available && output.boxes_xyxy.capacity_bytes == 0U && output.class_references.capacity_bytes == 0U &&
                   output.confidences.capacity_bytes == 0U && output.colors_rgb.capacity_bytes == 0U && output.masks.capacity_bytes == 0U;
        }
        const auto capacity = static_cast<std::uint32_t>(output.value_capacity);
        const std::uint32_t boxes[]{capacity, 4U};
        const std::uint32_t values[]{capacity};
        const std::uint32_t colors[]{capacity, 3U};
        const std::uint32_t masks[]{
            capacity,
            output.source_region.height,
            output.source_region.width,
        };
        return ValidateBuffer(output.boxes_xyxy, 2U, boxes, AnalysisElementType::Float32) &&
               ValidateBuffer(output.class_references, 1U, values, AnalysisElementType::Int32) &&
               ValidateBuffer(output.confidences, 1U, values, AnalysisElementType::Float32) &&
               ValidateBuffer(output.colors_rgb, 2U, colors, AnalysisElementType::Uint8) &&
               ValidateBuffer(output.masks, 3U, masks, AnalysisElementType::Uint8, true);
    }
    [[nodiscard]] static bool ValidateResult(const AnalysisRequest& request, const ProviderWorkResult& result) noexcept {
        if (result.identity != request.identity) { return false; }
        if (result.terminal != AnalysisTerminal::Completed) {
            if (result.output_count != 0U || result.completion.valid()) { return false; }
            for (const AnalysisAnnotationStorage& output : request.annotations) {
                if (!output.count.empty()) { return false; }
            }
            return true;
        }
        if (request.cancellation.stop_requested() || result.completed_ns == 0U || result.output_count != request.annotations.size() ||
            !result.completion.valid() || result.completion.device != request.source.device) {
            return false;
        }
        for (const AnalysisAnnotationStorage& output : request.annotations) {
            if (!output.count.valid(output.value_capacity) || !ValidateAnnotationStorage(output)) { return false; }
        }
        return true;
    }
    [[nodiscard]] static AnalysisResult Terminal(const AnalysisIdentity identity, const AnalysisTerminal terminal,
                                                 const std::uint64_t completed_ns = 0U) noexcept {
        return AnalysisResult{{}, 0U, identity, terminal, completed_ns, 0U, {}};
    }
    [[nodiscard]] std::uint64_t ClaimIssuing() noexcept {
        std::lock_guard lock(state_mutex_);
        if (state_ != ProviderState::Idle) { return 0U; }
        state_ = ProviderState::Issuing;
        if (++generation_ == 0U) { ++generation_; }
        return generation_;
    }
    void RestoreIdleAfterIssue(const std::uint64_t generation) noexcept {
        std::lock_guard lock(state_mutex_);
        if (state_ == ProviderState::Issuing && generation_ == generation) { state_ = ProviderState::Idle; }
    }
    [[nodiscard]] bool PublishActive(const std::uint64_t generation, const ProviderWorkResult& work) noexcept {
        std::lock_guard lock(state_mutex_);
        if (state_ == ProviderState::Issuing && generation_ == generation) {
            active_work_ = work;
            state_ = ProviderState::Active;
            return true;
        }
        return false;
    }
    [[nodiscard]] bool SettleAnalysis(const std::uint64_t generation, const bool observe_completion) noexcept {
        ProviderWorkResult work;
        {
            std::lock_guard lock(state_mutex_);
            if (state_ != ProviderState::Active || generation != generation_) { return false; }
            state_ = ProviderState::Settling;
            work = active_work_;
        }
        const bool completed = observe_completion && ObserveCompletion(work.completion);
        if (!completed) { RetireIssuedWork(work); }
        {
            std::lock_guard lock(state_mutex_);
            if (state_ == ProviderState::Settling && generation == generation_) {
                active_work_ = {};
                state_ = ProviderState::Idle;
            }
        }
        return completed;
    }
    mutable std::mutex state_mutex_;
    ProviderState state_ = ProviderState::Idle;
    std::uint64_t generation_ = 0U;
    ProviderWorkResult active_work_{};
    friend class AnalysisResult;
};
inline AnalysisResult::~AnalysisResult() noexcept { Abandon(); }
inline void AnalysisResult::Abandon() noexcept {
    if (owner_) {
        static_cast<void>(owner_->SettleAnalysis(generation_, false));
        Disarm();
        terminal_ = AnalysisTerminal::Cancelled;
        completed_ns_ = 0U;
        output_count_ = 0U;
    }
}
}  // namespace mmltk::backend::ml::runtime
