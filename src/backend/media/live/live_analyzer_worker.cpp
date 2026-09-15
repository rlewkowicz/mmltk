// CLEANUP-IGNORE: This module implementation owns its concrete global-fragment dependencies.
module;
#include <cuda.h>
#include <cuda_runtime_api.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <utility>

#include "detail/live_module_dependencies.h"  // IWYU pragma: keep
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"

module mmltk.backend.media.live.live_session_controller;

import mmltk.backend.media.live.live_capture_region;
import mmltk.backend.media.live.live_frame_id;
import mmltk.backend.media.capture.capture_session;
import mmltk.backend.media.capture.capture_types;
import mmltk.backend.media.capture.live_video_source;
import mmltk.backend.media.capture.status;

#include "detail/live_analyzer_worker.h"
namespace mmltk::backend::media::live {
namespace runtime = mmltk::backend::ml::runtime;

LiveAnalyzerWorker::LiveAnalyzerWorker(LiveFrameFanout& fanout, const std::uint32_t count, const std::uint32_t regions,
                                       const std::uint32_t width, const std::uint32_t height, LivePhysicalCudaContext cuda)
    : fanout_(fanout),
      cuda_(std::move(cuda)),
      slots_(count == 0U ? nullptr : std::make_unique<AnalysisSlot[]>(count)),
      slot_count_(count),
      maximum_regions_(regions),
      width_(width),
      height_(height) {
    if (count == 0U || regions == 0U || width == 0U || height == 0U || regions > runtime::kMaximumAnalysisRegions || !cuda_.valid())
        throw std::invalid_argument("Live analyzer requires bounded CUDA storage");
    try {
        auto scope = cuda_.scope();
        if (!scope) throw std::runtime_error("enter Live analyzer CUDA scope");
        for (std::uint32_t index = 0; index < count; ++index) {
            AnalysisSlot& slot = slots_[index];
            slot.index = index;
            slot.owner = this;
            slot.annotations = std::make_unique<runtime::AnalysisAnnotationStorage[]>(regions);
            slot.allocations = std::make_unique<CUdeviceptr[]>(regions * 5U);
            if (scope.Record(cudaStreamCreateWithFlags(&slot.settlement_stream, cudaStreamNonBlocking)) != cudaSuccess)
                throw std::runtime_error("create Live analysis settlement stream");
            for (std::uint32_t region = 0; region < regions; ++region) {
                auto& annotation = slot.annotations[region];
                annotation.value_capacity = runtime::kMaximumAnalysisRegions;
                const std::size_t values = annotation.value_capacity;
                const std::size_t sizes[5] = {values * 4U * sizeof(float), values * sizeof(std::int32_t), values * sizeof(float),
                                              values * 3U * sizeof(std::uint8_t), values * width * height * sizeof(std::uint8_t)};
                for (std::size_t plane = 0; plane < 5U; ++plane) {
                    void* allocation = nullptr;
                    if (scope.Record(cudaMalloc(&allocation, sizes[plane])) != cudaSuccess)
                        throw std::runtime_error("allocate Live annotation storage");
                    slot.allocations[region * 5U + plane] = reinterpret_cast<CUdeviceptr>(allocation);
                }
                annotation.boxes_xyxy = {static_cast<std::uintptr_t>(slot.allocations[region * 5U]),
                                         sizes[0],
                                         {2U, {static_cast<std::uint32_t>(values), 4U}},
                                         runtime::AnalysisElementType::Float32};
                annotation.class_references = {static_cast<std::uintptr_t>(slot.allocations[region * 5U + 1U]),
                                           sizes[1],
                                           {1U, {static_cast<std::uint32_t>(values)}},
                                           runtime::AnalysisElementType::Int32};
                annotation.confidences = {static_cast<std::uintptr_t>(slot.allocations[region * 5U + 2U]),
                                          sizes[2],
                                          {1U, {static_cast<std::uint32_t>(values)}},
                                          runtime::AnalysisElementType::Float32};
                annotation.colors_rgb = {static_cast<std::uintptr_t>(slot.allocations[region * 5U + 3U]),
                                         sizes[3],
                                         {2U, {static_cast<std::uint32_t>(values), 3U}},
                                         runtime::AnalysisElementType::Uint8};
                annotation.masks = {static_cast<std::uintptr_t>(slot.allocations[region * 5U + 4U]),
                                    sizes[4],
                                    {3U, {static_cast<std::uint32_t>(values), height, width}},
                                    runtime::AnalysisElementType::Uint8};
            }
        }
    } catch (...) {
        release_storage();
        throw;
    }
}

LiveAnalyzerWorker::~LiveAnalyzerWorker() {
    stop();
    release_storage();
}

void LiveAnalyzerWorker::set_provider(std::shared_ptr<runtime::AnalysisProvider> provider) {
    if (running_.load(std::memory_order_acquire)) throw std::logic_error("cannot replace running Live analysis provider");
    provider_ = std::move(provider);
}

void LiveAnalyzerWorker::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) throw std::logic_error("Live analyzer already running");
    std::lock_guard lock(status_mutex_);
    status_ = {.running = true, .provider_attached = provider_ != nullptr};
}

void LiveAnalyzerWorker::close_admission() noexcept { running_.store(false, std::memory_order_release); }

void LiveAnalyzerWorker::ScrubProduct(AnalysisSlot& slot) noexcept {
    slot.frame.reset();
    // CLEANUP-IGNORE: Analyzer slot scrubbing clears its result-release fact before shared latest-slot publication.
    slot.release_ready.store(false, std::memory_order_relaxed);
}

void LiveAnalyzerWorker::publish_slot(AnalysisSlot& slot, const SlotState published) noexcept {
    publish_latest_live_owner_slot(latest_, slot.index, slot.state, published, [&slot] noexcept { ScrubProduct(slot); });
}

void LiveAnalyzerWorker::stop() noexcept {
    close_admission();
    if (slots_ == nullptr) return;
    latest_.store(-1, std::memory_order_release);
    auto scope = cuda_.scope();
    for (std::uint32_t index = 0; index < slot_count_; ++index) {
        AnalysisSlot& slot = slots_[index];
        bool settled = static_cast<bool>(scope);
        if (scope && slot.settlement_stream != nullptr)
            settled = scope.Record(cudaStreamSynchronize(slot.settlement_stream)) == cudaSuccess;
        if (scope && slot.frame.has_value()) {
            const auto completion = slot.frame->result().completion();
            if (completion.valid())
                settled = scope.Record(cudaEventSynchronize(reinterpret_cast<cudaEvent_t>(completion.event))) == cudaSuccess && settled;
        }
        const SlotState current = static_cast<SlotState>(slot.state.load(std::memory_order_acquire));
        bool owned = current == SlotState::Completing;
        if (!owned && current != SlotState::Free && current != SlotState::Terminal) owned = claim_live_slot(slot.state, current);
        if (!owned) continue;
        if (slot.frame.has_value()) {
            auto result = std::move(*slot.frame).release_result();
            slot.frame.reset();
            settled = scope && provider_ != nullptr && provider_->ReleaseAfterCompletion(std::move(result)) && settled;
        }
        publish_slot(slot, settled ? SlotState::Free : SlotState::Terminal);
    }
    std::lock_guard lock(status_mutex_);
    status_.running = false;
}

LiveAnalyzerWorker::AnalysisSlot* LiveAnalyzerWorker::reserve() noexcept {
    // CLEANUP-IGNORE: This analyzer searches its own fixed AnalysisSlot partition through the shared slot-state
    // protocol.
    for (std::uint32_t index = 0; index < slot_count_; ++index) {
        if (transition_slot_state(slots_[index].state, SlotState::Free, SlotState::Uploading)) return &slots_[index];
    }
    return nullptr;
}

bool LiveAnalyzerWorker::process_latest() {
    if (!running_.load(std::memory_order_acquire)) return false;
    DeviceFrameView source{};
    if (!fanout_.try_acquire_analysis(&source)) return false;
    if (provider_ == nullptr) {
        fanout_.release_analysis(source.slot, source.ready, source.stream);
        std::lock_guard lock(status_mutex_);
        ++status_.refused;
        return true;
    }
    AnalysisSlot* slot = reserve();
    if (slot == nullptr) {
        fanout_.release_analysis(source.slot, source.ready, source.stream);
        return false;
    }

    auto& annotation = slot->annotations[0U];
    const runtime::AnalysisRegion region{0U, 0U, source.width, source.height};
    annotation.source_region = region;
    annotation.value_count = 0U;
    annotation.masks.shape = {3U, {static_cast<std::uint32_t>(annotation.value_capacity), region.height, region.width}};

    auto scope = cuda_.scope();
    if (!scope) {
        publish_slot(*slot, SlotState::Terminal);
        fanout_.release_analysis(source.slot, source.ready, source.stream);
        return false;
    }
    runtime::AnalysisRequest request{
        .identity = {source.frame.sequence, source.frame.session},
        .captured_ns = source.captured_ns,
        .source = {{source.pixels,
                    source.pitch_bytes * source.height,
                    {3U, {source.height, source.width, 3U}},
                    runtime::AnalysisElementType::Uint8},
                   source.pitch_bytes,
                   source.width,
                   source.height,
                   3U,
                   cuda_.device()},
        .source_ready = {cuda_.device(), reinterpret_cast<std::uintptr_t>(source.ready), reinterpret_cast<std::uintptr_t>(source.stream)},
        .regions = {&region, 1U},
        .annotations = {&annotation, 1U},
    };
    runtime::AnalysisResult result = provider_->Analyze(request);
    const auto completion = result.completion();
    fanout_.release_analysis(source.slot, completion.valid() ? reinterpret_cast<cudaEvent_t>(completion.event) : source.ready,
                             completion.valid() ? reinterpret_cast<cudaStream_t>(completion.producer_stream) : source.stream);
    if (result.terminal() != runtime::AnalysisTerminal::Completed) {
        const auto terminal = result.terminal();
        publish_slot(*slot, SlotState::Free);
        {
            std::lock_guard lock(status_mutex_);
            ++status_.refused;
        }
        if (terminal == runtime::AnalysisTerminal::InvalidInput || terminal == runtime::AnalysisTerminal::DependencyFailure ||
            terminal == runtime::AnalysisTerminal::ExecutionFailure)
            throw std::runtime_error("Live analysis provider failed");
        return true;
    }
    slot->frame.emplace(slot->index, source.frame, std::span{&annotation, 1U}, std::move(result));
    publish_live_slot_state(slot->state, SlotState::Published);
    latest_.store(static_cast<int>(slot->index), std::memory_order_release);
    std::lock_guard lock(status_mutex_);
    ++status_.completed;
    return true;
}

bool LiveAnalyzerWorker::try_acquire(const LiveFrameId frame, LiveAnalysisOverlayProjection* output) {
    if (output == nullptr || !frame.valid()) return false;
    const int preferred = latest_.load(std::memory_order_acquire);
    for (std::uint32_t offset = 0U; offset < slot_count_; ++offset) {
        const std::uint32_t index = preferred >= 0 ? (static_cast<std::uint32_t>(preferred) + offset) % slot_count_ : offset;
        AnalysisSlot& slot = slots_[index];
        if (!transition_slot_state(slot.state, SlotState::Published, SlotState::Acquired)) continue;
        if (!slot.frame.has_value() || slot.frame->frame() != frame) {
            publish_live_slot_state(slot.state, SlotState::Published);
            continue;
        }
        *output = {slot.frame->frame(), slot.index, slot.frame->annotations(), slot.frame->result().completion()};
        return true;
    }
    return false;
}

bool LiveAnalyzerWorker::discard(const LiveFrameId frame) {
    if (!frame.valid()) return false;
    for (std::uint32_t index = 0U; index < slot_count_; ++index) {
        AnalysisSlot& slot = slots_[index];
        if (!claim_live_slot(slot.state, SlotState::Published)) continue;
        if (!slot.frame.has_value() || slot.frame->frame() != frame) {
            publish_live_slot_state(slot.state, SlotState::Published);
            continue;
        }
        if (schedule_release(slot)) return true;
        release_slot(slot);
        return false;
    }
    return false;
}

bool LiveAnalyzerWorker::schedule_release(AnalysisSlot& slot) noexcept {
    if (!slot.frame.has_value()) return false;
    const auto completion = slot.frame->result().completion();
    auto scope = cuda_.scope();
    if (!scope || !completion.valid()) return false;
    cudaError_t status = scope.Record(cudaStreamWaitEvent(slot.settlement_stream, reinterpret_cast<cudaEvent_t>(completion.event), 0U));
    if (status == cudaSuccess) status = scope.Record(cudaLaunchHostFunc(slot.settlement_stream, ReadyToRelease, &slot));
    // CLEANUP-IGNORE: This CUDA callback advances AnalysisSlot custody owned exclusively by the analyzer.
    return status == cudaSuccess;
}

void CUDART_CB LiveAnalyzerWorker::ReadyToRelease(void* context) noexcept {
    auto& slot = *static_cast<AnalysisSlot*>(context);
    slot.release_ready.store(true, std::memory_order_release);
    slot.owner->ready_.notify();
}

bool LiveAnalyzerWorker::drain_releases() {
    bool released = false;
    for (std::uint32_t index = 0U; index < slot_count_; ++index) {
        AnalysisSlot& slot = slots_[index];
        if (!slot_state_is(slot.state, SlotState::Completing) || !slot.release_ready.exchange(false, std::memory_order_acq_rel)) continue;
        release_slot(slot);
        released = true;
    }
    return released;
}

void LiveAnalyzerWorker::release(const std::uint32_t index) noexcept {
    if (index >= slot_count_) {
        cuda_.Record(cudaErrorUnknown);
        return;
    }
    if (!claim_live_slot(slots_[index].state, SlotState::Acquired)) {
        cuda_.Record(cudaErrorUnknown);
        return;
    }
    release_slot(slots_[index]);
}

void LiveAnalyzerWorker::terminalize(const std::uint32_t index) noexcept {
    AnalysisSlot* const slot = index < slot_count_ ? &slots_[index] : nullptr;
    if (!claim_acquired_live_slot(slot == nullptr ? nullptr : &slot->state, cuda_)) return;
    publish_slot(*slot, SlotState::Terminal);
}

void LiveAnalyzerWorker::release_slot(AnalysisSlot& slot) noexcept {
    if (!slot.frame.has_value()) {
        publish_slot(slot, SlotState::Free);
        return;
    }
    auto scope = cuda_.scope();
    auto result = std::move(*slot.frame).release_result();
    slot.frame.reset();
    const bool released = scope && provider_ != nullptr && provider_->ReleaseAfterCompletion(std::move(result));
    if (!released) cuda_.Record(cudaErrorUnknown);
    publish_slot(slot, released ? SlotState::Free : SlotState::Terminal);
}

void LiveAnalyzerWorker::set_ready_listener(std::function<void()> listener) { ready_.set_listener(std::move(listener)); }

LiveAnalyzerWorker::Status LiveAnalyzerWorker::status() const {
    std::lock_guard lock(status_mutex_);
    return status_;
}

void LiveAnalyzerWorker::release_storage() noexcept {
    if (slots_ == nullptr) return;
    auto scope = cuda_.scope();
    for (std::uint32_t slot = 0; slot < slot_count_; ++slot) {
        if (scope) {
            synchronize_live_cuda_stream(scope, slots_[slot].settlement_stream);
            destroy_live_cuda_stream(scope, slots_[slot].settlement_stream);
        }
        slots_[slot].settlement_stream = nullptr;
        slots_[slot].frame.reset();
        if (slots_[slot].allocations != nullptr) {
            for (std::uint32_t allocation = 0; allocation < maximum_regions_ * 5U; ++allocation) {
                if (scope) free_live_cuda_allocation(scope, slots_[slot].allocations[allocation]);
                slots_[slot].allocations[allocation] = 0U;
            }
        }
        slots_[slot].allocations.reset();
        slots_[slot].annotations.reset();
    }
    provider_.reset();
    slots_.reset();
}
}  // namespace mmltk::backend::media::live
