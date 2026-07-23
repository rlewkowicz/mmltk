#include "mmltk/live/workspace_surface_pool.h"

#include "live/live_helpers.h"
#include "live/workspace_source_lease_ring.h"
#include "mmltk/live/live_worker_runtime.h"
#include "mmltk/live/workspace_trace.h"

#include <algorithm>
#include <cstring>
#include <drm_fourcc.h>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace mmltk::live {

namespace {

std::mutex g_workspace_adapter_mutex;
std::optional<WorkspaceVulkanAdapterCapabilities> g_workspace_adapter;

[[nodiscard]] std::uint64_t next_workspace_generation() noexcept {
    static std::atomic<std::uint64_t> next_generation{1U};
    return next_generation.fetch_add(1U, std::memory_order_relaxed);
}

class CudaTimelineSemaphore {
   public:
    ~CudaTimelineSemaphore() {
        const cudaExternalSemaphore_t semaphore = semaphore_.exchange(nullptr, std::memory_order_acq_rel);
        if (semaphore != nullptr) {
            (void)cudaDestroyExternalSemaphore(semaphore);
        }
    }

    CudaTimelineSemaphore() = default;
    CudaTimelineSemaphore(const CudaTimelineSemaphore&) = delete;
    CudaTimelineSemaphore& operator=(const CudaTimelineSemaphore&) = delete;

    [[nodiscard]] bool import_fd(const int semaphore_fd, std::string* error_message) {
        if (semaphore_fd < 0) {
            if (error_message != nullptr) {
                *error_message = "workspace timeline semaphore fd is invalid";
            }
            return false;
        }
        if (semaphore_.load(std::memory_order_acquire) != nullptr) {
            (void)close(semaphore_fd);
            if (error_message != nullptr) {
                *error_message = "workspace timeline semaphore was already imported";
            }
            return false;
        }

        cudaExternalSemaphoreHandleDesc descriptor{};
        descriptor.type = cudaExternalSemaphoreHandleTypeTimelineSemaphoreFd;
        descriptor.handle.fd = semaphore_fd;
        cudaExternalSemaphore_t imported = nullptr;
        const cudaError_t status = cudaImportExternalSemaphore(&imported, &descriptor);
        if (status != cudaSuccess) {
            (void)close(semaphore_fd);
            if (error_message != nullptr) {
                *error_message = std::string("cudaImportExternalSemaphore for workspace timeline failed: ") +
                                 cudaGetErrorString(status);
            }
            return false;
        }

        cudaExternalSemaphore_t expected = nullptr;
        if (!semaphore_.compare_exchange_strong(expected, imported, std::memory_order_release,
                                                std::memory_order_acquire)) {
            (void)cudaDestroyExternalSemaphore(imported);
            if (error_message != nullptr) {
                *error_message = "workspace timeline semaphore was imported concurrently";
            }
            return false;
        }
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
    }

    [[nodiscard]] bool ready() const noexcept {
        return semaphore_.load(std::memory_order_acquire) != nullptr;
    }

    void signal(const std::uint64_t revision, cudaStream_t stream) const {
        signal_users_.fetch_add(1U, std::memory_order_acq_rel);
        cudaExternalSemaphore_t semaphore = semaphore_.load(std::memory_order_acquire);
        if (semaphore == nullptr) {
            finish_signal();
            return;
        }
        cudaExternalSemaphoreSignalParams parameters{};
        parameters.params.fence.value = revision;
        try {
            ensure_cuda_ok(cudaSignalExternalSemaphoresAsync(&semaphore, &parameters, 1U, stream),
                           "cudaSignalExternalSemaphoresAsync for workspace timeline");
        } catch (...) {
            finish_signal();
            throw;
        }
        finish_signal();
    }

    void reset(cudaStream_t stream) noexcept {
        const cudaExternalSemaphore_t semaphore = semaphore_.exchange(nullptr, std::memory_order_acq_rel);
        if (semaphore == nullptr) {
            return;
        }
        std::uint32_t users = signal_users_.load(std::memory_order_acquire);
        while (users != 0U) {
            signal_users_.wait(users, std::memory_order_acquire);
            users = signal_users_.load(std::memory_order_acquire);
        }
        (void)cudaStreamSynchronize(stream);
        (void)cudaDestroyExternalSemaphore(semaphore);
    }

   private:
    void finish_signal() const noexcept {
        if (signal_users_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
            signal_users_.notify_all();
        }
    }

    std::atomic<cudaExternalSemaphore_t> semaphore_{nullptr};
    mutable std::atomic<std::uint32_t> signal_users_{0U};
};

using WorkspaceSlotBase =
    worker_runtime::LiveFrameSlotCommon<WorkspaceDimensions, WorkspaceOutputBundle, DmaBufCudaRgbaSurface>;

struct WorkspaceSlot : WorkspaceSlotBase {
    std::atomic<std::uint64_t> revision{0U};
    std::atomic<bool> in_flight{false};
    CudaTimelineSemaphore timeline;
    std::uint32_t published_width = 0U;
    std::uint32_t published_height = 0U;
    LiveCaptureRegion dirty_rect{};
};

struct SemanticSourceCache {
    struct Slot {
        BgrPitchedDeviceBuffer device_buffer;
        CudaEventHandle ready_event;
        LiveFrameId frame_id{};
        LiveCaptureRegion region{};
        bool occupied = false;
    };

    void initialize(const std::uint32_t slot_count, const std::uint32_t width, const std::uint32_t height) {
        std::lock_guard<std::mutex> lock(mutex);
        slots.resize(slot_count);
        worker_runtime::initialize_slot_array(slots, [&](Slot& slot, const std::uint32_t) {
            worker_runtime::initialize_pitched_event_resource(slot, width, height,
                                                              "cudaMallocPitch for semantic source cache",
                                                              "cudaEventCreateWithFlags for semantic source cache");
        });
    }

    void reset() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        for (Slot& slot : slots) {
            worker_runtime::reset_pitched_event_resource(slot);
        }
        slots.clear();
        next_index = 0U;
    }

    void clear_for_restart() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        for (Slot& slot : slots) {
            if (slot.occupied && slot.ready_event.get() != nullptr) {
                (void)cudaEventSynchronize(slot.ready_event.get());
            }
            slot.occupied = false;
            slot.frame_id = {};
            slot.region = {};
        }
        next_index = 0U;
    }

    void store(cudaStream_t source_stream, const OutputBundle& source) {
        std::lock_guard<std::mutex> lock(mutex);
        if (slots.empty() || source.data == 0 || source.dims.width == 0U || source.dims.height == 0U) {
            return;
        }
        Slot& slot = slots[next_index];
        const std::size_t row_bytes = static_cast<std::size_t>(source.dims.width) * 3U;
        if (slot.occupied) {
            ensure_cuda_ok(cudaStreamWaitEvent(source_stream, slot.ready_event.get(), 0),
                           "cudaStreamWaitEvent for semantic source cache reuse");
        }
        ensure_cuda_ok(
            cudaMemcpy2DAsync(device_ptr_as_void(slot.device_buffer.data()), slot.device_buffer.pitch_bytes(),
                              device_ptr_as_const_void(source.data), source.dims.pitch_bytes, row_bytes,
                              source.dims.height, cudaMemcpyDeviceToDevice, source_stream),
            "cudaMemcpy2DAsync for semantic source cache");
        ensure_cuda_ok(cudaEventRecord(slot.ready_event.get(), source_stream),
                       "cudaEventRecord for semantic source cache");
        slot.frame_id = source.frame_id;
        slot.region = source.region;
        slot.occupied = true;
        next_index = (next_index + 1U) % slots.size();
    }

    template <typename Fn>
    [[nodiscard]] bool with_slot(const LiveFrameId& frame_id, Fn&& fn, std::string* error_message) const {
        std::lock_guard<std::mutex> lock(mutex);
        for (const Slot& slot : slots) {
            if (slot.occupied && slot.frame_id == frame_id) {
                return fn(slot);
            }
        }
        if (error_message != nullptr) {
            *error_message = "semantic readback source is no longer cached";
        }
        return false;
    }

    mutable std::mutex mutex;
    std::vector<Slot> slots;
    std::size_t next_index = 0U;
};

struct SemanticReadback {
    void initialize() {
        worker_runtime::initialize_stream_event_resource(*this, "cudaStreamCreateWithPriority for semantic readback",
                                                         "cudaEventCreateWithFlags for semantic readback");
    }

    void reset() noexcept {
        host_buffer.reset();
        worker_runtime::reset_stream_event_resource(*this);
    }

    [[nodiscard]] bool copy(const SemanticSourceCache::Slot& source, const int cuda_device_index,
                            std::vector<std::uint8_t>* pixels_bgr, std::uint32_t* width, std::uint32_t* height,
                            LiveCaptureRegion* region, std::string* error_message) {
        std::lock_guard<std::mutex> lock(mutex);
        ensure_cuda_ok(cudaSetDevice(cuda_device_index), "cudaSetDevice for semantic readback");
        const std::size_t row_bytes = static_cast<std::size_t>(source.region.width) * 3U;
        const std::size_t required_bytes = row_bytes * static_cast<std::size_t>(source.region.height);
        host_buffer.ensure_capacity(required_bytes, "cudaHostAlloc for semantic readback");
        ensure_cuda_ok(cudaStreamWaitEvent(stream.get(), source.ready_event.get(), 0),
                       "cudaStreamWaitEvent for semantic readback");
        ensure_cuda_ok(
            cudaMemcpy2DAsync(host_buffer.data(), row_bytes, device_ptr_as_const_void(source.device_buffer.data()),
                              source.device_buffer.pitch_bytes(), row_bytes, source.region.height,
                              cudaMemcpyDeviceToHost, stream.get()),
            "cudaMemcpy2DAsync for semantic readback");
        ensure_cuda_ok(cudaEventRecord(ready_event.get(), stream.get()), "cudaEventRecord for semantic readback");
        ensure_cuda_ok(cudaEventSynchronize(ready_event.get()), "cudaEventSynchronize for semantic readback");
        pixels_bgr->assign(host_buffer.data(), host_buffer.data() + required_bytes);
        *width = source.region.width;
        *height = source.region.height;
        *region = source.region;
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
    }

    std::mutex mutex;
    CudaStreamHandle stream;
    CudaEventHandle ready_event;
    PinnedUploadBuffer<std::uint8_t> host_buffer;
};

}  

void publish_workspace_vulkan_adapter(WorkspaceVulkanAdapterCapabilities capabilities) {
    capabilities.rgba8_modifiers.erase(
        std::remove(capabilities.rgba8_modifiers.begin(), capabilities.rgba8_modifiers.end(),
                    kWorkspaceDmaBufDrmFormatModInvalid),
        capabilities.rgba8_modifiers.end());
    std::sort(capabilities.rgba8_modifiers.begin(), capabilities.rgba8_modifiers.end());
    capabilities.rgba8_modifiers.erase(
        std::unique(capabilities.rgba8_modifiers.begin(), capabilities.rgba8_modifiers.end()),
        capabilities.rgba8_modifiers.end());
    if (!capabilities.valid()) {
        trace_workspace("native", "workspace.adapter_rejected", [&] {
            return nlohmann::json{{"render_major", capabilities.render_major},
                                  {"render_minor", capabilities.render_minor},
                                  {"modifier_count", capabilities.rgba8_modifiers.size()},
                                  {"timeline_semaphore", capabilities.timeline_semaphore}};
        });
        throw std::runtime_error(
            "Firefox WebGPU adapter does not expose a render node, timeline semaphore, and explicit RGBA modifier");
    }
    trace_workspace("native", "workspace.adapter_negotiated", [&] {
        return nlohmann::json{{"render_major", capabilities.render_major},
                              {"render_minor", capabilities.render_minor},
                              {"modifier_count", capabilities.rgba8_modifiers.size()},
                              {"modifiers", capabilities.rgba8_modifiers},
                              {"timeline_semaphore", capabilities.timeline_semaphore}};
    });
    std::lock_guard lock(g_workspace_adapter_mutex);
    g_workspace_adapter = std::move(capabilities);
}

void clear_workspace_vulkan_adapter() noexcept {
    trace_workspace("native", "workspace.adapter_cleared", [] { return nlohmann::json::object(); });
    std::lock_guard lock(g_workspace_adapter_mutex);
    g_workspace_adapter.reset();
}

bool workspace_vulkan_adapter_ready() noexcept {
    std::lock_guard lock(g_workspace_adapter_mutex);
    return g_workspace_adapter.has_value();
}

namespace {

[[nodiscard]] std::vector<std::uint64_t> ordered_shared_workspace_modifiers(
    const std::vector<std::uint64_t>& native_modifiers, const std::vector<std::uint64_t>& browser_modifiers) {
    std::vector<std::uint64_t> shared;
    shared.reserve(std::min(native_modifiers.size(), browser_modifiers.size()));
    const auto append_matching = [&](const bool linear) {
        for (const std::uint64_t modifier : native_modifiers) {
            if (modifier == kWorkspaceDmaBufDrmFormatModInvalid || (modifier == DRM_FORMAT_MOD_LINEAR) != linear ||
                !std::binary_search(browser_modifiers.begin(), browser_modifiers.end(), modifier) ||
                std::find(shared.begin(), shared.end(), modifier) != shared.end()) {
                continue;
            }
            shared.push_back(modifier);
        }
    };
    append_matching(false);
    append_matching(true);
    return shared;
}

}  

std::uint64_t require_workspace_drm_modifier(const int cuda_device_index) {
    WorkspaceVulkanAdapterCapabilities browser;
    {
        std::lock_guard lock(g_workspace_adapter_mutex);
        if (!g_workspace_adapter.has_value()) {
            trace_workspace("native", "workspace.negotiation_failed",
                            [] { return nlohmann::json{{"reason", "adapter_unavailable"}}; });
            throw std::runtime_error(
                "Firefox WebGPU workspace adapter negotiation is not ready; no native display fallback is allowed");
        }
        browser = *g_workspace_adapter;
    }

    LinuxGpuInteropDevice native;
    native.ensure_open(cuda_device_index, "workspace adapter negotiation");
    struct stat render_stat {};
    if (::stat(native.render_node_path().c_str(), &render_stat) != 0 || !S_ISCHR(render_stat.st_mode)) {
        trace_workspace("native", "workspace.negotiation_failed", [&] {
            return nlohmann::json{{"reason", "native_render_node_unavailable"},
                                  {"render_node", native.render_node_path()}};
        });
        throw std::runtime_error("workspace adapter negotiation could not identify the native DRM render node");
    }
    if (static_cast<std::uint32_t>(::major(render_stat.st_rdev)) != browser.render_major ||
        static_cast<std::uint32_t>(::minor(render_stat.st_rdev)) != browser.render_minor) {
        trace_workspace("native", "workspace.negotiation_failed", [&] {
            return nlohmann::json{{"reason", "render_node_mismatch"},
                                  {"native_major", ::major(render_stat.st_rdev)},
                                  {"native_minor", ::minor(render_stat.st_rdev)},
                                  {"firefox_major", browser.render_major},
                                  {"firefox_minor", browser.render_minor}};
        });
        throw std::runtime_error(
            "Firefox Vulkan and CUDA/EGL resolved to different DRM render nodes; cross-GPU display is disabled");
    }

    ensure_cuda_driver_ok(cuInit(0), "cuInit for workspace adapter negotiation");
    CUdevice cuda_device{};
    ensure_cuda_driver_ok(cuDeviceGet(&cuda_device, cuda_device_index),
                          "cuDeviceGet for workspace adapter negotiation");
    CUuuid cuda_uuid{};
    ensure_cuda_driver_ok(cuDeviceGetUuid(&cuda_uuid, cuda_device),
                          "cuDeviceGetUuid for workspace adapter negotiation");
    static_assert(sizeof(cuda_uuid.bytes) == kWorkspaceDeviceUuidBytes);
    if (std::memcmp(cuda_uuid.bytes, browser.device_uuid.data(), browser.device_uuid.size()) != 0) {
        trace_workspace("native", "workspace.negotiation_failed", [&] {
            return nlohmann::json{{"reason", "device_uuid_mismatch"}, {"cuda_device", cuda_device_index}};
        });
        throw std::runtime_error("Firefox Vulkan and CUDA device UUIDs differ; cross-GPU display is disabled");
    }

    const std::vector<std::uint64_t> shared_modifiers =
        ordered_shared_workspace_modifiers(native.dma_buf_modifiers(), browser.rgba8_modifiers);
    if (!shared_modifiers.empty()) {
        const std::uint64_t modifier = shared_modifiers.front();
        trace_workspace("native", "workspace.modifier_selected", [&] {
            return nlohmann::json{{"cuda_device", cuda_device_index},
                                  {"render_node", native.render_node_path()},
                                  {"modifier", modifier},
                                  {"candidate_count", shared_modifiers.size()},
                                  {"native_modifier_count", native.dma_buf_modifiers().size()},
                                  {"firefox_modifier_count", browser.rgba8_modifiers.size()}};
        });
        return modifier;
    }
    trace_workspace("native", "workspace.negotiation_failed", [&] {
        return nlohmann::json{{"reason", "modifier_intersection_empty"},
                              {"native_modifier_count", native.dma_buf_modifiers().size()},
                              {"firefox_modifier_count", browser.rgba8_modifiers.size()}};
    });
    throw std::runtime_error(
        "Firefox Vulkan and CUDA/EGL have no common explicit RGBA DMA-BUF modifier; implicit display is disabled");
}

struct WorkspaceSurfacePool::Impl {
    explicit Impl(WorkspaceSurfacePoolConfig config_in) : config(config_in), generation(next_workspace_generation()) {
        if (config.width == 0U || config.height == 0U || config.slot_count == 0U ||
            config.semantic_cache_slot_count == 0U ||
            config.negotiated_drm_modifier == kWorkspaceDmaBufDrmFormatModInvalid) {
            throw std::runtime_error(
                "workspace surface pool requires non-zero dimensions/slot counts and a negotiated DRM modifier");
        }
        ensure_cuda_ok(cudaSetDevice(config.cuda_device_index), "cudaSetDevice for workspace surface pool");
        interop_device.ensure_open(config.cuda_device_index, "GBM/EGL open for workspace surface pool");
        std::vector<std::uint64_t> browser_modifiers;
        {
            std::lock_guard lock(g_workspace_adapter_mutex);
            if (!g_workspace_adapter.has_value()) {
                throw std::runtime_error("Firefox WebGPU workspace adapter disappeared before surface allocation");
            }
            browser_modifiers = g_workspace_adapter->rgba8_modifiers;
        }
        const std::vector<std::uint64_t> candidates =
            ordered_shared_workspace_modifiers(interop_device.dma_buf_modifiers(), browser_modifiers);
        if (std::find(candidates.begin(), candidates.end(), config.negotiated_drm_modifier) == candidates.end()) {
            throw std::runtime_error("negotiated workspace DRM modifier is no longer shared by Firefox and CUDA/EGL");
        }

        std::string allocation_errors;
        for (const std::uint64_t candidate : candidates) {
            try {
                worker_runtime::allocate_dmabuf_cuda_rgba_slots(
                    slots, config.slot_count, interop_device, config.cuda_device_index, config.width, config.height,
                    candidate, "GBM/CUDA DMA-BUF allocation for workspace surface pool",
                    "cudaStreamCreateWithPriority for workspace surface pool",
                    "cudaEventCreateWithFlags for workspace surface pool");
                config.negotiated_drm_modifier = candidate;
                break;
            } catch (const std::exception& error) {
                trace_workspace("native", "workspace.modifier_allocation_rejected", [&] {
                    return nlohmann::json{{"generation", generation}, {"modifier", candidate}, {"error", error.what()}};
                });
                if (!allocation_errors.empty()) {
                    allocation_errors += '\n';
                }
                allocation_errors += error.what();
                slots.clear();
                interop_device.reset();
                interop_device.ensure_open(config.cuda_device_index,
                                           "GBM/EGL reopen for workspace modifier negotiation");
            }
        }
        if (slots.size() != config.slot_count) {
            throw std::runtime_error(
                "no shared Firefox WebGPU DMA-BUF modifier produced a CUDA-writable GBM allocation\n" +
                allocation_errors);
        }
        source_leases.initialize(slots.size());
        semantic_cache.initialize(config.semantic_cache_slot_count, config.width, config.height);
        semantic_readback.initialize();
        trace_workspace("native", "workspace.pool_allocated", [&] {
            return nlohmann::json{{"generation", generation},
                                  {"slot_count", slots.size()},
                                  {"width", config.width},
                                  {"height", config.height},
                                  {"modifier", config.negotiated_drm_modifier},
                                  {"render_node", interop_device.render_node_path()}};
        });
    }

    ~Impl() {
        trace_workspace("native", "workspace.pool_retiring",
                        [&] { return nlohmann::json{{"generation", generation}, {"slot_count", slots.size()}}; });
        (void)cudaSetDevice(config.cuda_device_index);
        semantic_readback.reset();
        semantic_cache.reset();
        slots.clear();
        interop_device.reset();
    }

    [[nodiscard]] WorkspaceFrameMetadata metadata(const WorkspaceSlot& slot,
                                                  const std::uint64_t revision) const noexcept {
        const std::uint32_t width = slot.published_width;
        const std::uint32_t height = slot.published_height;
        return WorkspaceFrameMetadata{
            revision,
            generation,
            slot.frame_id,
            WorkspaceDimensions{width, height, slot.device_buffer.pitch_bytes()},
            slot.device_buffer.dmabuf_image(width, height),
            slot.region,
            slot.dirty_rect,
            slot.capture_ns,
            slot.ready_ns,
            slot.short_frame,
        };
    }

    [[nodiscard]] WorkspacePresentSnapshot latest_present() const noexcept {
        for (std::uint32_t attempt = 0; attempt < 4U; ++attempt) {
            const std::uint64_t begin = present_sequence.load(std::memory_order_acquire);
            if ((begin & 1U) != 0U) {
                continue;
            }
            const bool valid = present_valid.load(std::memory_order_relaxed);
            const std::uint32_t slot_index = present_slot.load(std::memory_order_relaxed);
            const std::uint64_t revision = present_revision.load(std::memory_order_relaxed);
            const LiveFrameId frame_id{present_frame_session.load(std::memory_order_relaxed),
                                       present_frame_sequence.load(std::memory_order_relaxed)};
            const std::uint32_t width = present_width.load(std::memory_order_relaxed);
            const std::uint32_t height = present_height.load(std::memory_order_relaxed);
            const LiveCaptureRegion source_region{present_source_x.load(std::memory_order_relaxed),
                                                  present_source_y.load(std::memory_order_relaxed),
                                                  present_source_width.load(std::memory_order_relaxed),
                                                  present_source_height.load(std::memory_order_relaxed)};
            const LiveCaptureRegion dirty_rect{present_dirty_x.load(std::memory_order_relaxed),
                                               present_dirty_y.load(std::memory_order_relaxed),
                                               present_dirty_width.load(std::memory_order_relaxed),
                                               present_dirty_height.load(std::memory_order_relaxed)};
            const std::uint64_t capture_ns = present_capture_ns.load(std::memory_order_relaxed);
            const std::uint64_t ready_ns = present_ready_ns.load(std::memory_order_relaxed);
            const bool short_frame = present_short_frame.load(std::memory_order_relaxed);
            const std::uint64_t end = present_sequence.load(std::memory_order_acquire);
            if (begin != end || (end & 1U) != 0U) {
                continue;
            }
            if (!valid || slot_index >= slots.size()) {
                return {};
            }
            return make_workspace_present_snapshot(
                true, slot_index,
                WorkspaceFrameMetadata{
                    revision,
                    generation,
                    frame_id,
                    WorkspaceDimensions{width, height, slots[slot_index]->device_buffer.pitch_bytes()},
                    slots[slot_index]->device_buffer.dmabuf_image(width, height),
                    source_region,
                    dirty_rect,
                    capture_ns,
                    ready_ns,
                    short_frame,
                });
        }
        return {};
    }

    void store_present(const std::uint32_t slot_index, const std::uint64_t revision,
                       const WorkspaceSurfacePublishInfo& info) noexcept {
        present_sequence.fetch_add(1U, std::memory_order_acq_rel);
        present_valid.store(true, std::memory_order_relaxed);
        present_slot.store(slot_index, std::memory_order_relaxed);
        present_revision.store(revision, std::memory_order_relaxed);
        present_frame_session.store(info.frame_id.session_nonce, std::memory_order_relaxed);
        present_frame_sequence.store(info.frame_id.sequence, std::memory_order_relaxed);
        present_width.store(info.width, std::memory_order_relaxed);
        present_height.store(info.height, std::memory_order_relaxed);
        present_source_x.store(info.source_region.x, std::memory_order_relaxed);
        present_source_y.store(info.source_region.y, std::memory_order_relaxed);
        present_source_width.store(info.source_region.width, std::memory_order_relaxed);
        present_source_height.store(info.source_region.height, std::memory_order_relaxed);
        present_dirty_x.store(info.dirty_rect.x, std::memory_order_relaxed);
        present_dirty_y.store(info.dirty_rect.y, std::memory_order_relaxed);
        present_dirty_width.store(info.dirty_rect.width, std::memory_order_relaxed);
        present_dirty_height.store(info.dirty_rect.height, std::memory_order_relaxed);
        present_capture_ns.store(info.capture_ns, std::memory_order_relaxed);
        present_ready_ns.store(info.ready_ns, std::memory_order_relaxed);
        present_short_frame.store(info.short_frame, std::memory_order_relaxed);
        present_sequence.fetch_add(1U, std::memory_order_release);
    }

    void clear_present() noexcept {
        present_sequence.fetch_add(1U, std::memory_order_acq_rel);
        present_valid.store(false, std::memory_order_relaxed);
        present_slot.store(0U, std::memory_order_relaxed);
        present_revision.store(0U, std::memory_order_relaxed);
        present_sequence.fetch_add(1U, std::memory_order_release);
        latest_slot.store(-1, std::memory_order_release);
        front_slot.store(-1, std::memory_order_release);
        front_revision.store(0U, std::memory_order_release);
    }

    WorkspaceSurfacePoolConfig config;
    std::uint64_t generation = 0U;
    std::atomic<std::uint64_t> next_revision{1U};
    LinuxGpuInteropDevice interop_device;
    std::vector<std::unique_ptr<WorkspaceSlot>> slots;
    WorkspaceSourceLeaseRing source_leases;
    SemanticSourceCache semantic_cache;
    SemanticReadback semantic_readback;
    std::atomic<int> latest_slot{-1};
    std::atomic<int> front_slot{-1};
    std::atomic<std::uint64_t> front_revision{0U};
    std::atomic<std::uint64_t> present_sequence{0U};
    std::atomic<bool> present_valid{false};
    std::atomic<std::uint32_t> present_slot{0U};
    std::atomic<std::uint64_t> present_revision{0U};
    std::atomic<std::uint64_t> present_frame_session{0U};
    std::atomic<std::uint64_t> present_frame_sequence{0U};
    std::atomic<std::uint32_t> present_width{0U};
    std::atomic<std::uint32_t> present_height{0U};
    std::atomic<std::uint32_t> present_source_x{0U};
    std::atomic<std::uint32_t> present_source_y{0U};
    std::atomic<std::uint32_t> present_source_width{0U};
    std::atomic<std::uint32_t> present_source_height{0U};
    std::atomic<std::uint32_t> present_dirty_x{0U};
    std::atomic<std::uint32_t> present_dirty_y{0U};
    std::atomic<std::uint32_t> present_dirty_width{0U};
    std::atomic<std::uint32_t> present_dirty_height{0U};
    std::atomic<std::uint64_t> present_capture_ns{0U};
    std::atomic<std::uint64_t> present_ready_ns{0U};
    std::atomic<bool> present_short_frame{false};
    std::atomic<std::uint64_t> presentation_epoch{1U};
    std::atomic<std::uint64_t> availability_epoch{1U};
};

WorkspaceSurfacePool::WorkspaceSurfacePool(WorkspaceSurfacePoolConfig config) : impl_(std::make_unique<Impl>(config)) {}

WorkspaceSurfacePool::~WorkspaceSurfacePool() = default;

WorkspaceSwapchainDescriptor WorkspaceSurfacePool::descriptor() const {
    WorkspaceSwapchainDescriptor descriptor;
    descriptor.width = impl_->config.width;
    descriptor.height = impl_->config.height;
    descriptor.generation = impl_->generation;
    descriptor.cuda_device_index = impl_->config.cuda_device_index;
    descriptor.render_node_path = impl_->interop_device.render_node_path();
    descriptor.slots.reserve(impl_->slots.size());
    for (const auto& slot : impl_->slots) {
        descriptor.slots.push_back(WorkspaceSwapchainSlotDescriptor{
            slot->slot_index,
            WorkspaceDimensions{impl_->config.width, impl_->config.height, slot->device_buffer.pitch_bytes()},
            slot->device_buffer.dmabuf_image(impl_->config.width, impl_->config.height),
            slot->revision.load(std::memory_order_acquire),
        });
    }
    return descriptor;
}

WorkspacePresentSnapshot WorkspaceSurfacePool::latest_present() const noexcept {
    return impl_->latest_present();
}

std::uint64_t WorkspaceSurfacePool::generation() const noexcept {
    return impl_->generation;
}

WorkspaceSurfacePoolStatus WorkspaceSurfacePool::snapshot_status() const noexcept {
    const WorkspaceSourceLeaseRingStatus source = impl_->source_leases.snapshot_status();
    return WorkspaceSurfacePoolStatus{
        source.acquire_waits,
        source.leases_acquired,
        source.leases_released,
        source.stale_releases,
        source.skipped_stale_frames,
        source.slot_pressure,
        source.release_latency_ns,
        impl_->front_slot.load(std::memory_order_acquire),
        impl_->front_revision.load(std::memory_order_acquire),
    };
}

bool WorkspaceSurfacePool::try_reserve_write(WorkspaceSurfaceWriteLease* out) {
    if (out == nullptr) {
        throw std::runtime_error("workspace surface reservation requires an output lease");
    }
    *out = {};
    bool source_pressure = false;
    for (auto& slot : impl_->slots) {
        if (slot->in_flight.load(std::memory_order_acquire)) {
            source_pressure = true;
            continue;
        }
        if (impl_->front_slot.load(std::memory_order_acquire) == static_cast<int>(slot->slot_index)) {
            continue;
        }
        std::uint32_t expected = to_slot_state_value(SlotState::kFree);
        if (!slot->state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kWriting),
                                                 std::memory_order_acq_rel)) {
            if ((expected != to_slot_state_value(SlotState::kPublished) &&
                 expected != to_slot_state_value(SlotState::kPendingFree)) ||
                !event_ready(slot->ready_event_handle(), "cudaEventQuery for workspace surface reuse")) {
                source_pressure = source_pressure || expected == to_slot_state_value(SlotState::kAcquired);
                continue;
            }
            const std::uint32_t reusable_state = expected;
            expected = reusable_state;
            if (!slot->state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kWriting),
                                                     std::memory_order_acq_rel)) {
                continue;
            }
            slot->reset_for_reuse();
        }

        *out = WorkspaceSurfaceWriteLease{
            true,
            impl_->generation,
            slot->slot_index,
            slot->device_buffer.data(),
            slot->device_buffer.pitch_bytes(),
            slot->device_buffer.surface_object(),
            slot->stream.get(),
            slot->device_buffer.is_pitch_frame(),
            slot->device_buffer.is_array_frame(),
        };
        return true;
    }
    if (source_pressure) {
        impl_->source_leases.note_slot_pressure();
    }
    trace_workspace("native", "workspace.write_dropped", [&] {
        return nlohmann::json{{"generation", impl_->generation},
                              {"reason", source_pressure ? "all_slots_in_flight" : "no_reusable_slot"}};
    });
    return false;
}

WorkspacePresentSnapshot WorkspaceSurfacePool::publish_write(WorkspaceSurfaceWriteLease* lease,
                                                             const WorkspaceSurfacePublishInfo& info) {
    if (lease == nullptr || !lease->valid || lease->generation != impl_->generation ||
        lease->slot_index >= impl_->slots.size() || info.width == 0U || info.height == 0U ||
        info.width > impl_->config.width || info.height > impl_->config.height) {
        throw std::runtime_error("workspace surface publication has an invalid write lease or dimensions");
    }
    WorkspaceSlot& slot = *impl_->slots[lease->slot_index];
    if (slot.state.load(std::memory_order_acquire) != to_slot_state_value(SlotState::kWriting)) {
        throw std::runtime_error("workspace surface publication lease is not writing");
    }

    const std::uint64_t revision = impl_->next_revision.fetch_add(1U, std::memory_order_relaxed);
    slot.timeline.signal(revision, slot.stream.get());
    ensure_cuda_ok(cudaEventRecord(slot.ready_event.get(), slot.stream.get()),
                   "cudaEventRecord for workspace surface publication");
    slot.frame_id = info.frame_id;
    slot.region = info.source_region;
    slot.published_width = info.width;
    slot.published_height = info.height;
    slot.dirty_rect = info.dirty_rect;
    slot.capture_ns = info.capture_ns;
    slot.ready_ns = info.ready_ns;
    slot.short_frame = info.short_frame;
    slot.revision.store(revision, std::memory_order_release);
    worker_runtime::publish_latest_slot(slot, impl_->latest_slot);
    impl_->front_slot.store(static_cast<int>(slot.slot_index), std::memory_order_release);
    impl_->front_revision.store(revision, std::memory_order_release);
    impl_->store_present(slot.slot_index, revision, info);
    impl_->source_leases.notify_all();
    trace_workspace("native", "workspace.cuda_timeline_signaled", [&] {
        return nlohmann::json{{"generation", impl_->generation}, {"slot", slot.slot_index}, {"revision", revision}};
    });
    trace_workspace("native", "workspace.composite_published", [&] {
        return nlohmann::json{{"generation", impl_->generation},
                              {"slot", slot.slot_index},
                              {"revision", revision},
                              {"width", info.width},
                              {"height", info.height},
                              {"capture_ns", info.capture_ns},
                              {"ready_ns", info.ready_ns}};
    });
    *lease = {};
    return make_workspace_present_snapshot(true, slot.slot_index, impl_->metadata(slot, revision));
}

void WorkspaceSurfacePool::cancel_write(WorkspaceSurfaceWriteLease* lease) noexcept {
    if (lease == nullptr || !lease->valid || lease->generation != impl_->generation ||
        lease->slot_index >= impl_->slots.size()) {
        return;
    }
    WorkspaceSlot& slot = *impl_->slots[lease->slot_index];
    std::uint32_t expected = to_slot_state_value(SlotState::kWriting);
    if (slot.state.load(std::memory_order_acquire) == expected) {
        if (cudaEventRecord(slot.ready_event.get(), slot.stream.get()) == cudaSuccess) {
            (void)slot.state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kPendingFree),
                                                     std::memory_order_acq_rel);
        }
    }
    *lease = {};
    impl_->source_leases.notify_all();
}

bool WorkspaceSurfacePool::request_next_source_frame(const std::uint64_t after_revision,
                                                     const std::atomic<bool>& caller_stop_requested,
                                                     const std::atomic<bool>& owner_stop_requested,
                                                     const std::atomic<bool>& owner_running,
                                                     WorkspaceSourceFrameLease* out) {
    return impl_->source_leases.request_next(
        impl_->slots, impl_->generation, after_revision, caller_stop_requested, owner_stop_requested,
        [&owner_running]() noexcept { return owner_running.load(std::memory_order_acquire); },
        [this](const std::uint32_t slot_index) {
            return slot_index < impl_->slots.size() ? impl_->slots[slot_index]->revision.load(std::memory_order_acquire)
                                                    : 0U;
        },
        [](const WorkspaceSlot& slot) {
            return event_ready(slot.ready_event_handle(), "cudaEventQuery for workspace source lease");
        },
        [this](const WorkspaceSlot& slot, const std::uint64_t, const std::uint64_t revision) {
            return make_workspace_source_frame_lease(true, slot.slot_index, impl_->metadata(slot, revision));
        },
        out);
}

void WorkspaceSurfacePool::release_source_frame(const WorkspaceSourceFrameLease& lease) noexcept {
    impl_->source_leases.release(impl_->slots, lease);
}

bool WorkspaceSurfacePool::try_acquire_latest(WorkspaceOutputBundle* out) {
    if (out == nullptr) {
        throw std::runtime_error("workspace surface acquire requires an output bundle");
    }
    *out = {};
    return worker_runtime::try_acquire_latest_ready_published_slot_view(impl_->slots, impl_->latest_slot, out,
                                                                        "cudaEventQuery for workspace surface acquire");
}

void WorkspaceSurfacePool::release_acquired(const std::uint32_t slot_index) {
    worker_runtime::release_slot_by_index(impl_->slots, slot_index, "workspace surface release index out of range",
                                          "workspace surface slot is not acquired");
    impl_->availability_epoch.fetch_add(1U, std::memory_order_release);
    impl_->source_leases.notify_all();
}

bool WorkspaceSurfacePool::acquire_slot_for_import(const std::uint32_t slot_index) {
    if (slot_index >= impl_->slots.size()) {
        throw std::runtime_error("workspace surface import index out of range");
    }
    WorkspaceSlot& slot = *impl_->slots[slot_index];
    std::uint32_t expected = to_slot_state_value(SlotState::kFree);
    if (!slot.state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kAcquired),
                                            std::memory_order_acq_rel)) {
        if (expected != to_slot_state_value(SlotState::kPublished) ||
            !event_ready(slot.ready_event_handle(), "cudaEventQuery for workspace import reservation")) {
            return false;
        }
        expected = to_slot_state_value(SlotState::kPublished);
        if (!slot.state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kAcquired),
                                                std::memory_order_acq_rel)) {
            return false;
        }
    }
    slot.in_flight.store(true, std::memory_order_release);
    return true;
}

bool WorkspaceSurfacePool::mark_slot_in_flight(const std::uint32_t slot_index, const bool in_flight) {
    if (slot_index >= impl_->slots.size()) {
        throw std::runtime_error("workspace surface in-flight index out of range");
    }
    WorkspaceSlot& slot = *impl_->slots[slot_index];
    if (in_flight) {
        std::uint32_t expected = to_slot_state_value(SlotState::kPublished);
        if (!slot.state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kAcquired),
                                                std::memory_order_acq_rel)) {
            return false;
        }
        slot.in_flight.store(true, std::memory_order_release);
        return true;
    }
    slot.in_flight.store(false, std::memory_order_release);
    std::uint32_t expected = to_slot_state_value(SlotState::kAcquired);
    const bool released =
        slot.state.compare_exchange_strong(expected, to_slot_state_value(SlotState::kFree), std::memory_order_acq_rel);
    if (released) {
        impl_->availability_epoch.fetch_add(1U, std::memory_order_release);
    }
    impl_->source_leases.notify_all();
    return released;
}

bool WorkspaceSurfacePool::import_timeline_semaphore(const std::uint64_t generation, const std::uint32_t slot_index,
                                                     const int semaphore_fd, std::string* error_message) {
    if (generation != impl_->generation || slot_index >= impl_->slots.size()) {
        if (semaphore_fd >= 0) {
            (void)close(semaphore_fd);
        }
        if (error_message != nullptr) {
            *error_message = generation != impl_->generation ? "workspace timeline generation is stale"
                                                             : "workspace timeline slot is out of range";
        }
        return false;
    }
    const cudaError_t device_status = cudaSetDevice(impl_->config.cuda_device_index);
    if (device_status != cudaSuccess) {
        if (semaphore_fd >= 0) {
            (void)close(semaphore_fd);
        }
        if (error_message != nullptr) {
            *error_message =
                std::string("cudaSetDevice for workspace timeline import failed: ") + cudaGetErrorString(device_status);
        }
        return false;
    }
    const bool imported = impl_->slots[slot_index]->timeline.import_fd(semaphore_fd, error_message);
    if (imported) {
        trace_workspace("native", "workspace.cuda_timeline_imported", [&] {
            return nlohmann::json{{"generation", generation}, {"slot", slot_index}, {"replacement", false}};
        });
    }
    return imported;
}

bool WorkspaceSurfacePool::replace_timeline_semaphore(const std::uint64_t generation, const std::uint32_t slot_index,
                                                      const int semaphore_fd, std::string* error_message) {
    if (generation != impl_->generation || slot_index >= impl_->slots.size()) {
        if (semaphore_fd >= 0) {
            (void)close(semaphore_fd);
        }
        if (error_message != nullptr) {
            *error_message = generation != impl_->generation ? "workspace timeline generation is stale"
                                                             : "workspace timeline slot is out of range";
        }
        return false;
    }
    if (cudaSetDevice(impl_->config.cuda_device_index) != cudaSuccess) {
        if (semaphore_fd >= 0) {
            (void)close(semaphore_fd);
        }
        if (error_message != nullptr) {
            *error_message = "cudaSetDevice for workspace timeline replacement failed";
        }
        return false;
    }
    WorkspaceSlot& slot = *impl_->slots[slot_index];
    slot.timeline.reset(slot.stream.get());
    const bool imported = slot.timeline.import_fd(semaphore_fd, error_message);
    if (imported) {
        trace_workspace("native", "workspace.cuda_timeline_imported", [&] {
            return nlohmann::json{{"generation", generation}, {"slot", slot_index}, {"replacement", true}};
        });
    }
    return imported;
}

bool WorkspaceSurfacePool::timeline_slot_ready(const std::uint64_t generation,
                                               const std::uint32_t slot_index) const noexcept {
    return generation == impl_->generation && slot_index < impl_->slots.size() &&
           impl_->slots[slot_index]->timeline.ready();
}

bool WorkspaceSurfacePool::timeline_sync_ready(const std::uint64_t generation) const noexcept {
    return generation == impl_->generation && !impl_->slots.empty() &&
           std::all_of(impl_->slots.begin(), impl_->slots.end(),
                       [](const auto& slot) { return slot->timeline.ready(); });
}

void WorkspaceSurfacePool::reset_timeline_semaphores() noexcept {
    if (cudaSetDevice(impl_->config.cuda_device_index) != cudaSuccess) {
        return;
    }
    for (const auto& slot : impl_->slots) {
        slot->timeline.reset(slot->stream.get());
    }
    trace_workspace("native", "workspace.cuda_timelines_reset", [&] {
        return nlohmann::json{{"generation", impl_->generation}, {"slot_count", impl_->slots.size()}};
    });
    invalidate_presentation();
}

void WorkspaceSurfacePool::invalidate_presentation() noexcept {
    impl_->clear_present();
    impl_->presentation_epoch.fetch_add(1U, std::memory_order_release);
    impl_->source_leases.notify_all();
}

std::uint64_t WorkspaceSurfacePool::presentation_epoch() const noexcept {
    return impl_->presentation_epoch.load(std::memory_order_acquire);
}

std::uint64_t WorkspaceSurfacePool::availability_epoch() const noexcept {
    return impl_->availability_epoch.load(std::memory_order_acquire);
}

void WorkspaceSurfacePool::cache_semantic_source(cudaStream_t source_stream, const OutputBundle& source) {
    impl_->semantic_cache.store(source_stream, source);
}

bool WorkspaceSurfacePool::readback_semantic_source(const LiveFrameId& frame_id, std::vector<std::uint8_t>* pixels_bgr,
                                                    std::uint32_t* width, std::uint32_t* height,
                                                    LiveCaptureRegion* region, std::string* error_message) {
    if (pixels_bgr == nullptr || width == nullptr || height == nullptr || region == nullptr) {
        throw std::runtime_error("semantic source readback requires non-null outputs");
    }
    const bool copied = impl_->semantic_cache.with_slot(
        frame_id,
        [&](const SemanticSourceCache::Slot& slot) {
            return impl_->semantic_readback.copy(slot, impl_->config.cuda_device_index, pixels_bgr, width, height,
                                                 region, error_message);
        },
        error_message);
    trace_workspace("native", "workspace.semantic_readback", [&] {
        return nlohmann::json{{"generation", impl_->generation},
                              {"frame_session", frame_id.session_nonce},
                              {"frame_sequence", frame_id.sequence},
                              {"copied", copied}};
    });
    return copied;
}

void WorkspaceSurfacePool::reset_for_producer_restart() noexcept {
    trace_workspace("native", "workspace.producer_restart",
                    [&] { return nlohmann::json{{"generation", impl_->generation}}; });
    (void)cudaSetDevice(impl_->config.cuda_device_index);
    for (auto& slot : impl_->slots) {
        const std::uint32_t state = slot->state.load(std::memory_order_acquire);
        if (state == to_slot_state_value(SlotState::kAcquired)) {
            continue;
        }
        if (state == to_slot_state_value(SlotState::kWriting) && slot->stream.get() != nullptr) {
            (void)cudaStreamSynchronize(slot->stream.get());
        } else if ((state == to_slot_state_value(SlotState::kPublished) ||
                    state == to_slot_state_value(SlotState::kPendingFree)) &&
                   slot->ready_event.get() != nullptr) {
            (void)cudaEventSynchronize(slot->ready_event.get());
        }
        slot->reset_for_reuse();
        slot->state.store(to_slot_state_value(SlotState::kFree), std::memory_order_release);
    }
    impl_->source_leases.reset_inactive_leases_for_restart();
    impl_->semantic_cache.clear_for_restart();
    impl_->clear_present();
    impl_->source_leases.notify_all();
}

void WorkspaceSurfacePool::reset_status() noexcept {
    impl_->source_leases.reset_counters();
}

void WorkspaceSurfacePool::notify_waiters() noexcept {
    impl_->source_leases.notify_all();
}

}  
