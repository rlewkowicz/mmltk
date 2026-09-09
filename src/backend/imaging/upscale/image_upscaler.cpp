module;
#include <NvInferVersion.h>
#include <cuda_runtime.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <chrono>
#include <thread>

#include "detail/image_upscaler_cuda.h"
#include "detail/image_upscaler_nis.h"
#include "src/backend/data/benchmark_hash.h"
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/common/io/scoped_fd.h"
#include "src/common/system/runtime_paths.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/image_failure.h"
#include "upscale_execution.h"

module mmltk.backend.imaging.upscale.image_upscaler;

import mmltk.common.logging.mmltk_logging;

#include "detail/image_upscaler_internal.h"

namespace mmltk::backend::imaging::upscale {

using mmltk::backend::ml::runtime::TensorRtEngineOptions;
using mmltk::backend::ml::runtime::TensorRtOptimizationProfile;
using mmltk::backend::ml::runtime::TensorRtProfilingVerbosity;
using mmltk::frameworks::gpu::ensure_cuda_ok;

namespace {

constexpr std::size_t kUpscalerCount = 2U;
constexpr std::uint64_t kTensorRtWorkspaceBytes = 1ULL << 30U;

[[nodiscard]] std::size_t kind_index(const ImageUpscalerKind kind) noexcept {
    const std::size_t index = static_cast<std::size_t>(kind);
    if (index >= kUpscalerCount) { std::terminate(); }
    return index;
}

[[nodiscard]] std::filesystem::path engine_cache_directory() {
    return mmltk::common::system::runtime_paths::repository_root() / ".cache" / "mmltk" / "upscalers";
}

[[nodiscard]] std::string_view precision_policy(const ImageUpscalerDescriptor& descriptor) noexcept {
    return descriptor.allow_fp16 ? "mixed-fp16" : "strict-fp32";
}

class FileLock final {
   public:
    explicit FileLock(const std::filesystem::path& path) : descriptor_(::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0664)) {
        if (descriptor_.get() < 0) {
            throw std::runtime_error("failed to open Image upscaler cache lock " + path.string() + ": " + std::strerror(errno));
        }
    }
    [[nodiscard]] ImageUpscalerOutcome Acquire(ImageUpscalerCurrent current, const ImageUpscalerExecutionCheckpoint& checkpoint) {
        while (current()) {
            if (::flock(descriptor_.get(), LOCK_EX | LOCK_NB) == 0) return ImageUpscalerOutcome::Completed;
            const int error = errno;
            if (error != EWOULDBLOCK && error != EINTR)
                throw std::runtime_error(std::string("failed to lock Image upscaler cache: ") + std::strerror(error));
            if (!image_upscaler_admitted(checkpoint, ImageUpscalerExecutionStage::CacheLockWaiting, current))
                return ImageUpscalerOutcome::Cancelled;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return ImageUpscalerOutcome::Cancelled;
    }

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

   private:
    mmltk::common::io::ScopedFd descriptor_{};
};

[[nodiscard]] std::string engine_identity(const ImageUpscalerDescriptor& descriptor, const cudaDeviceProp& properties) {
    return std::string(descriptor.cache_name) + "-" + std::string(descriptor.sha256.substr(0U, 16U)) + "-trt" +
           std::to_string(NV_TENSORRT_MAJOR) + "." + std::to_string(NV_TENSORRT_MINOR) + "." + std::to_string(NV_TENSORRT_PATCH) + "." +
           std::to_string(NV_TENSORRT_BUILD) + "-cuda" + std::to_string(CUDART_VERSION) + "-sm" + std::to_string(properties.major) +
           std::to_string(properties.minor) + "-" + std::string(precision_policy(descriptor)) + "-tile" +
           std::to_string(kImageUpscalerInputExtent);
}

void trace_activation_stage(const std::string_view stage, const ImageUpscalerKind kind, const std::string_view detail = {}) {
    mmltk::common::logging::trace([&](auto& logger) {
        logger.trace("event=image_upscaler_activation stage={} kind={} detail={}", stage, static_cast<std::uint32_t>(kind), detail);
    });
}

struct ImageUpscalerInFlightGate {
    static constexpr std::size_t kCapacity = 16U;
    [[nodiscard]] bool acquire() {
        std::lock_guard lock(mutex);
        if (!accepting || count == kCapacity) { return false; }
        ++count;
        return true;
    }

    void release() noexcept {
        std::lock_guard lock(mutex);
        if (count == 0U) std::terminate();
        --count;
    }

    void open() {
        std::lock_guard lock(mutex);
        accepting = true;
    }

    void close() {
        std::lock_guard lock(mutex);
        accepting = false;
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lock(mutex);
        return count == 0U;
    }

    mutable std::mutex mutex;
    std::size_t count = 0U;
    bool accepting = false;
};

}  // namespace

struct ImageUpscaler::Impl {
    enum class CustodyState : std::uint8_t {
        Active,
        Completed,
        Fatal,
    };

    struct Slot {
        Slot(const ImageUpscalerKind kind_in, const ImageUpscalerModelHandle handle_in) : kind(kind_in), handle(handle_in) {}

        mutable std::mutex mutex;
        std::shared_ptr<ImageUpscalerRuntime> runtime;
        ImageUpscalerKind kind = ImageUpscalerKind::ShiftLUT;
        ImageUpscalerModelHandle handle{};
    };

    struct DefaultModel final {
        enum class CleanupState : std::uint8_t {
            Active,
            Completed,
            Fatal,
        };

        ~DefaultModel() noexcept {
            if (stream != nullptr || terminal_event != nullptr || horizontal != nullptr || scaled != nullptr || orphaned != nullptr)
                std::terminate();
        }

        [[nodiscard]] cudaStream_t ensure_stream(const int device_id) {
            // A failed method keeps its stream alive until the caller settles
            // the bridge. Reconstruct only on the next operation admission.
            const bool retry = [this] {
                std::lock_guard lock(mutex);
                return failed;
            }();
            if (retry) {
                const auto status = Stop();
                if (status != cudaSuccess)
                    throw ImageUpscalerUnsettledFailure{
                        std::make_exception_ptr(mmltk::frameworks::gpu::CudaError(status, "release failed Basic method"))};
            }
            std::lock_guard lock(mutex);
            if (device_id < 0 || (owned_device >= 0 && owned_device != device_id)) return nullptr;
            cleanup_state = CleanupState::Active;
            cleanup = {};
            failed = false;
            if (stream != nullptr) {
                ensure_cuda_ok(cudaSetDevice(owned_device), "bind retained Basic stream");
                return stream;
            }
            owned_device = device_id;
            const auto bound = select_device();
            if (bound != cudaSuccess) {
                failed = true;
                ensure_cuda_ok(bound, "bind Basic device");
            }
            const cudaError_t stream_status = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
            if (stream_status != cudaSuccess) {
                failed = true;
                ensure_cuda_ok(stream_status, "create Basic stream");
            }
            const auto event_device = select_device();
            if (event_device != cudaSuccess) {
                failed = true;
                ensure_cuda_ok(event_device, "bind Basic event device");
            }
            const cudaError_t event_status = cudaEventCreateWithFlags(&terminal_event, cudaEventDisableTiming);
            if (event_status == cudaSuccess) return stream;

            if (terminal_event != nullptr) {
                if (cleanup.Record(cudaEventDestroy(terminal_event), "destroy partial Basic event")) { terminal_event = nullptr; }
            }
            if (stream != nullptr) {
                if (cleanup.Record(cudaStreamDestroy(stream), "destroy partial Basic stream")) { stream = nullptr; }
            }
            failed = true;
            if (cleanup.status() != cudaSuccess) {
                cleanup_state = CleanupState::Fatal;
                throw ImageUpscalerUnsettledFailure{
                    std::make_exception_ptr(mmltk::frameworks::gpu::CudaError(event_status, "create Basic completion event")),
                    cleanup.failure()};
            }
            ensure_cuda_ok(event_status, "create Basic completion event");
            return nullptr;
        }

        [[nodiscard]] ImageUpscalerOutcome run(const std::uint8_t* source, const std::size_t source_pitch, const std::uint32_t width,
                                               const std::uint32_t height, std::uint8_t* target, const std::size_t target_pitch,
                                               const cudaStream_t requested_stream, ImageUpscalerCurrent current,
                                               const ImageUpscalerExecutionCheckpoint& checkpoint) {
            std::lock_guard lock(mutex);
            if (failed || stream == nullptr || requested_stream != stream || owned_device < 0) {
                failed = true;
                throw std::runtime_error("Basic Upscale method is unavailable");
            }
            try {
                if (!current()) return ImageUpscalerOutcome::Cancelled;
                ensure_cuda_ok(cudaSetDevice(owned_device), "bind Basic launch device");
                const image_upscaler_nis::Configuration config{
                    .source_width = width,
                    .source_height = height,
                    .crop_x = 0U,
                    .crop_y = 0U,
                    .crop_width = width,
                    .crop_height = height,
                    .output_width = width * 4U,
                    .output_height = height * 4U,
                };
                const auto required = image_upscaler_nis::scratch_requirements(config);
                if (!required) throw std::runtime_error("invalid Basic scratch extent");
                if (!image_upscaler_admitted(checkpoint, ImageUpscalerExecutionStage::BasicAllocationAdmitted, current))
                    return ImageUpscalerOutcome::Cancelled;
                ensure_buffer(horizontal, horizontal_capacity, required->horizontal_bytes);
                if (!image_upscaler_admitted(checkpoint, ImageUpscalerExecutionStage::BasicAllocationAdmitted, current))
                    return ImageUpscalerOutcome::Cancelled;
                ensure_buffer(scaled, scaled_capacity, required->scaled_bytes);
                if (!current()) return ImageUpscalerOutcome::Cancelled;
                mmltk::common::logging::trace([&](auto& logger) {
                    logger.trace(
                        "event=image_upscaler_basic_buffers width={} height={} source_pitch={} target_pitch={} horizontal={} "
                        "horizontal_bytes={} scaled={} scaled_bytes={}",
                        width, height, source_pitch, target_pitch, reinterpret_cast<std::uintptr_t>(horizontal), horizontal_capacity,
                        reinterpret_cast<std::uintptr_t>(scaled), scaled_capacity);
                });
                if (!image_upscaler_admitted(checkpoint, ImageUpscalerExecutionStage::BasicLaunchAdmitted, current))
                    return ImageUpscalerOutcome::Cancelled;
                ensure_cuda_ok(image_upscaler_nis::launch_scale(source, source_pitch, horizontal, scaled, config, stream),
                               "launch Basic scaling");
                if (!image_upscaler_admitted(checkpoint, ImageUpscalerExecutionStage::BasicLaunchAdmitted, current))
                    return ImageUpscalerOutcome::Cancelled;
                ensure_cuda_ok(image_upscaler_nis::launch_sharpen(source, source_pitch, scaled, target, target_pitch, config, stream),
                               "launch Basic sharpening");
                ensure_cuda_ok(cudaEventRecord(terminal_event, stream), "record Basic completion");
                return ImageUpscalerOutcome::Completed;
            } catch (...) {
                failed = true;
                throw;
            }
        }

        [[nodiscard]] cudaError_t Stop() noexcept {
            std::lock_guard lock(mutex);
            if (cleanup_state == CleanupState::Completed) return cudaSuccess;
            if (cleanup_state == CleanupState::Fatal) return cleanup.status() == cudaSuccess ? cudaErrorUnknown : cleanup.status();
            if (stream == nullptr && (horizontal != nullptr || scaled != nullptr || orphaned != nullptr)) {
                cleanup_state = CleanupState::Fatal;
                return cudaErrorInvalidResourceHandle;
            }
            if (stream != nullptr && !cleanup.Record(select_device(), "bind Basic retirement device")) {
                cleanup_state = CleanupState::Fatal;
                return cleanup.status();
            }
            if (stream != nullptr) {
                const cudaError_t synchronized = cudaStreamSynchronize(stream);
                if (!cleanup.Record(synchronized, "settle Basic execution")) {
                    cleanup_state = CleanupState::Fatal;
                    return synchronized;
                }
            }
            for (void** allocation : {&horizontal, &scaled, &orphaned}) {
                if (*allocation != nullptr && cleanup.Record(cudaFreeAsync(*allocation, stream), "release Basic scratch"))
                    *allocation = nullptr;
            }
            horizontal_capacity = 0U;
            scaled_capacity = 0U;
            if (stream != nullptr) {
                const auto settled = cudaStreamSynchronize(stream);
                if (!cleanup.Record(settled, "settle Basic scratch release")) {
                    cleanup_state = CleanupState::Fatal;
                    return settled;
                }
            }
            if (terminal_event != nullptr) {
                if (cleanup.Record(cudaEventDestroy(terminal_event), "destroy Basic terminal event")) terminal_event = nullptr;
            }
            if (stream != nullptr) {
                if (cleanup.Record(cudaStreamDestroy(stream), "destroy Basic stream")) stream = nullptr;
            }
            cleanup_state = cleanup.status() == cudaSuccess ? CleanupState::Completed : CleanupState::Fatal;
            return cleanup.status();
        }

        [[nodiscard]] bool has_stream() const noexcept {
            std::lock_guard lock(mutex);
            return stream != nullptr;
        }
        [[nodiscard]] std::exception_ptr cleanup_failure() const noexcept { return cleanup.failure(); }

       private:
        [[nodiscard]] cudaError_t select_device() const noexcept {
            return owned_device >= 0 ? cudaSetDevice(owned_device) : cudaErrorInvalidDevice;
        }

        void ensure_buffer(void*& buffer, std::size_t& capacity, const std::size_t required) {
            if (required <= capacity) return;
            ensure_cuda_ok(cudaSetDevice(owned_device), "bind Basic allocation device");
            void* replacement = nullptr;
            ensure_cuda_ok(cudaMallocAsync(&replacement, required, stream), "allocate Basic scratch");
            if (buffer != nullptr) {
                const auto status = cudaFreeAsync(buffer, stream);
                if (status != cudaSuccess) {
                    orphaned = replacement;
                    ensure_cuda_ok(status, "release replaced Basic scratch");
                }
            }
            buffer = replacement;
            capacity = required;
        }

        mutable std::mutex mutex;
        cudaStream_t stream = nullptr;
        cudaEvent_t terminal_event = nullptr;
        void* horizontal = nullptr;
        void* scaled = nullptr;
        void* orphaned = nullptr;
        std::size_t horizontal_capacity = 0U;
        std::size_t scaled_capacity = 0U;
        int owned_device = -1;
        CleanupState cleanup_state = CleanupState::Active;
        bool failed = false;
        UpscalerCleanup cleanup;
    };

    explicit Impl(const std::uint64_t generation_in, const std::int32_t device_id_in, const std::uint64_t device_generation_in,
                  const ImageUpscalerAggregateConfig& config)
        : slots{std::make_unique<Slot>(ImageUpscalerKind::ShiftLUT, config.models[1U]),
                std::make_unique<Slot>(ImageUpscalerKind::RealPLKSR, config.models[2U])},
          generation(generation_in),
          device_id(device_id_in),
          device_generation(device_generation_in) {
        checkpoint = config.checkpoint;
        if (config.models[0U].device_id != device_id || config.models[0U].backend_generation != device_generation)
            throw ImageUpscalerStartError::DefaultNis;
        if (config.models[1U].device_id != device_id || config.models[1U].backend_generation != device_generation)
            throw ImageUpscalerStartError::ShiftLut;
        if (config.models[2U].device_id != device_id || config.models[2U].backend_generation != device_generation)
            throw ImageUpscalerStartError::RealPlksr;
    }

    ~Impl() {
        const CustodyState terminal = custody.load(std::memory_order_acquire);
        const bool construction_unwind = terminal == CustodyState::Active && std::uncaught_exceptions() != 0;
        if (terminal != CustodyState::Completed && !construction_unwind) std::terminate();
        for (cudaStream_t stream : operation_streams) {
            if (stream != nullptr) std::terminate();
        }
    }

    [[nodiscard]] cudaError_t Stop() noexcept {
        std::lock_guard lifecycle_lock(lifecycle_mutex);
        const CustodyState state = custody.load(std::memory_order_acquire);
        if (state == CustodyState::Completed) return cudaSuccess;
        if (state == CustodyState::Fatal) return stop_failure;
        in_flight->close();
        if (!in_flight->empty()) return cudaErrorNotReady;
        if (!cleanup.Record(cudaSetDevice(device_id), "bind Upscale retirement device")) return FailStop(cleanup.status());
        for (const ImageUpscalerKind kind : {ImageUpscalerKind::ShiftLUT, ImageUpscalerKind::RealPLKSR}) {
            Slot& target = slot(kind);
            std::lock_guard slot_lock(target.mutex);
            auto& stream = operation_streams[kind_index(kind)];
            if (stream != nullptr && !cleanup.Record(cudaStreamSynchronize(stream), "settle Upscale operation stream")) continue;
            bool released = true;
            if (target.runtime != nullptr) {
                released = cleanup.Record(target.runtime->Stop(), "release Upscale method runtime");
                runtime_cleanup_failure =
                    mmltk::frameworks::gpu::combine_image_failures(runtime_cleanup_failure, target.runtime->cleanup_failure());
            }
            if (!released) continue;
            target.runtime.reset();
            if (stream != nullptr && cleanup.Record(cudaStreamDestroy(stream), "destroy Upscale operation stream")) stream = nullptr;
        }
        cleanup.Record(basic.Stop(), "release Basic Upscale method");
        if (cleanup.status() != cudaSuccess) return FailStop(cleanup.status());
        custody.store(CustodyState::Completed, std::memory_order_release);
        return cudaSuccess;
    }

    [[nodiscard]] cudaError_t FailStop(const cudaError_t failure) noexcept {
        if (stop_failure == cudaSuccess) stop_failure = failure;
        custody.store(CustodyState::Fatal, std::memory_order_release);
        return stop_failure;
    }

    [[nodiscard]] Slot& slot(const ImageUpscalerKind kind) { return *slots[kind_index(kind)]; }

    [[nodiscard]] const Slot& slot(const ImageUpscalerKind kind) const { return *slots[kind_index(kind)]; }

    ImageUpscalerOutcome verify_model(const ImageUpscalerDescriptor& descriptor, ImageUpscalerCurrent current) {
        struct Cancellation {
            ImageUpscalerCurrent current;
            bool cancelled() const noexcept { return !current(); }
        } cancellation{current};
        if (!image_upscaler_admitted(checkpoint, ImageUpscalerExecutionStage::ChecksumAdmitted, current))
            return ImageUpscalerOutcome::Cancelled;
        const std::filesystem::path path = image_upscaler_model_path(descriptor);
        const auto digest =
            mmltk::backend::data::try_sha256_file(path, mmltk::common::concurrency::CancellationObservation::Borrow(cancellation));
        if (!digest) return ImageUpscalerOutcome::Cancelled;
        const std::string actual = mmltk::backend::data::sha256_hex(*digest);
        if (actual != descriptor.sha256) throw std::runtime_error(std::string(descriptor.label) + " model checksum mismatch");
        return ImageUpscalerOutcome::Completed;
    }

    ImageUpscalerOutcome initialize_onnx(Slot& target, ImageUpscalerCurrent current) {
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        const auto& descriptor = image_upscaler_descriptor(target.kind);
        trace_activation_stage("onnx_checksum_started", target.kind);
        if (verify_model(descriptor, current) == ImageUpscalerOutcome::Cancelled) return ImageUpscalerOutcome::Cancelled;
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        trace_activation_stage("onnx_checksum_completed", target.kind);
        trace_activation_stage("onnx_runtime_started", target.kind);
        try {
            target.runtime = make_onnx_upscaler_runtime(descriptor, image_upscaler_model_path(descriptor), device_id, checkpoint);
        } catch (const std::exception& error) {
            trace_activation_stage("onnx_runtime_failed", target.kind, error.what());
            throw;
        }
        if (target.runtime == nullptr) throw std::runtime_error("ShiftLUT runtime unavailable");
        trace_activation_stage("onnx_runtime_completed", target.kind);
        return ImageUpscalerOutcome::Completed;
    }

    ImageUpscalerOutcome initialize_tensorrt(Slot& target, ImageUpscalerCurrent current) {
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        const auto& descriptor = image_upscaler_descriptor(target.kind);
        trace_activation_stage("tensorrt_checksum_started", target.kind);
        if (verify_model(descriptor, current) == ImageUpscalerOutcome::Cancelled) return ImageUpscalerOutcome::Cancelled;
        if (!current()) return ImageUpscalerOutcome::Cancelled;
        trace_activation_stage("tensorrt_checksum_completed", target.kind);
        ensure_cuda_ok(cudaSetDevice(device_id), "cudaSetDevice for Image upscaler engine");
        cudaDeviceProp properties{};
        ensure_cuda_ok(cudaGetDeviceProperties(&properties, device_id), "cudaGetDeviceProperties for Image upscaler cache");

        const std::filesystem::path cache = engine_cache_directory();
        std::filesystem::create_directories(cache);
        const std::string identity = engine_identity(descriptor, properties);
        const std::filesystem::path engine_path = cache / (identity + ".engine");
        const std::filesystem::path lock_path = cache / (identity + ".lock");
        trace_activation_stage("tensorrt_cache_lock_started", target.kind);
        if (!image_upscaler_admitted(checkpoint, ImageUpscalerExecutionStage::CacheLockAdmitted, current))
            return ImageUpscalerOutcome::Cancelled;
        FileLock cache_lock(lock_path);
        if (cache_lock.Acquire(current, checkpoint) == ImageUpscalerOutcome::Cancelled) return ImageUpscalerOutcome::Cancelled;
        trace_activation_stage("tensorrt_cache_lock_completed", target.kind);
        TensorRtEngineOptions options{
            .device = device_id,
            .allow_fp16 = descriptor.allow_fp16,
            .allow_tf32 = descriptor.allow_tf32,
            .workspace_bytes = kTensorRtWorkspaceBytes,
            .profiling_verbosity = TensorRtProfilingVerbosity::LayerNames,
            .save_engine_path = {},
            .optimization_profiles = {TensorRtOptimizationProfile{.input_name = std::string(descriptor.input_name),
                                                                  .minimum = {1, 3, kImageUpscalerInputExtent, kImageUpscalerInputExtent},
                                                                  .optimum = {1, 3, kImageUpscalerInputExtent, kImageUpscalerInputExtent},
                                                                  .maximum = {1, 3, kImageUpscalerInputExtent, kImageUpscalerInputExtent}}},
            .context = std::string("Image upscaler ") + std::string(descriptor.label),
            .log = {},
            .continue_build = [current] { return current(); },
        };
        if (std::filesystem::is_regular_file(engine_path)) {
            if (!current()) return ImageUpscalerOutcome::Cancelled;
            try {
                trace_activation_stage("tensorrt_cache_load_started", target.kind);
                auto engine = std::make_unique<TensorRtEngine>(engine_path, options);
                if (engine->cancelled()) return ImageUpscalerOutcome::Cancelled;
                trace_activation_stage("tensorrt_runtime_started", target.kind);
                target.runtime = make_tensorrt_upscaler_runtime(descriptor, std::move(engine), device_id, checkpoint);
                if (target.runtime != nullptr) {
                    trace_activation_stage("tensorrt_cache_load_completed", target.kind);
                    return ImageUpscalerOutcome::Completed;
                }
            } catch (const mmltk::backend::ml::runtime::TensorRtCacheIntegrityError&) {
                // Only decoding and immutable tensor-contract validation run
                // here. Activation occurs after the runtime has an owner.
                target.runtime.reset();
            }
            std::error_code removal_error;
            if (!std::filesystem::remove(engine_path, removal_error) || removal_error) {
                throw std::runtime_error("failed to remove invalid Image upscaler engine cache " + engine_path.string() + ": " +
                                         removal_error.message());
            }
        }

        const std::filesystem::path temporary = cache / (identity + ".tmp." + std::to_string(static_cast<long long>(::getpid())));
        std::error_code removal_error;
        std::filesystem::remove(temporary, removal_error);
        options.save_engine_path = temporary;
        if (!image_upscaler_admitted(checkpoint, ImageUpscalerExecutionStage::BuildAdmitted, current))
            return ImageUpscalerOutcome::Cancelled;
        try {
            trace_activation_stage("tensorrt_engine_build_started", target.kind);
            auto engine = std::make_unique<TensorRtEngine>(image_upscaler_model_path(descriptor), std::move(options));
            if (engine->cancelled()) {
                std::filesystem::remove(temporary, removal_error);
                return ImageUpscalerOutcome::Cancelled;
            }
            trace_activation_stage("tensorrt_engine_build_completed", target.kind);
            trace_activation_stage("tensorrt_runtime_started", target.kind);
            target.runtime = make_tensorrt_upscaler_runtime(descriptor, std::move(engine), device_id, checkpoint);
            if (target.runtime == nullptr) throw std::runtime_error("RealPLKSR runtime unavailable");
            trace_activation_stage("tensorrt_runtime_completed", target.kind);
            std::filesystem::rename(temporary, engine_path);
        } catch (...) {
            std::filesystem::remove(temporary, removal_error);
            throw;
        }
        return ImageUpscalerOutcome::Completed;
    }

    std::array<std::unique_ptr<Slot>, kUpscalerCount> slots;
    std::shared_ptr<ImageUpscalerInFlightGate> in_flight = std::make_shared<ImageUpscalerInFlightGate>();
    std::atomic<CustodyState> custody{CustodyState::Active};
    mutable std::mutex lifecycle_mutex;
    cudaError_t stop_failure = cudaSuccess;
    UpscalerCleanup cleanup;
    std::exception_ptr runtime_cleanup_failure;
    ImageUpscalerExecutionCheckpoint checkpoint;
    DefaultModel basic;
    std::array<cudaStream_t, 2U> operation_streams{};
    std::uint64_t generation = 0U;
    std::int32_t device_id = -1;
    std::uint64_t device_generation = 0U;
};

namespace {
std::atomic<std::uint64_t> g_image_upscaler_generation{1U};
}

struct ImageUpscaler::Resolver final {
    static constexpr std::size_t kCapacity = 4U;

    struct Slot final {
        std::weak_ptr<Impl> owner;
        std::uint64_t service_generation = 0U;
        std::uint64_t core_generation = 0U;
        bool live = false;
    };

    struct Coordinate final {
        std::uint8_t slot = 0U;
        std::uint64_t service_generation = 0U;
    };

    [[nodiscard]] static Resolver& instance() noexcept {
        static Resolver resolver;
        return resolver;
    }

    [[nodiscard]] static std::optional<Coordinate> register_owner(const std::shared_ptr<Impl>& owner,
                                                                  const std::uint64_t core_generation) noexcept {
        Resolver& resolver = instance();
        std::lock_guard lock(resolver.mutex);
        for (std::size_t index = 0U; index < resolver.slots.size(); ++index) {
            const Slot& slot = resolver.slots[index];
            if (!slot.live || slot.core_generation != core_generation) continue;
            const std::shared_ptr<Impl> registered = slot.owner.lock();
            if (registered == owner)
                return Coordinate{.slot = static_cast<std::uint8_t>(index), .service_generation = slot.service_generation};
        }
        for (std::size_t index = 0U; index < resolver.slots.size(); ++index) {
            Slot& slot = resolver.slots[index];
            if (slot.live || slot.service_generation == std::numeric_limits<std::uint64_t>::max()) continue;
            ++slot.service_generation;
            slot.owner = owner;
            slot.core_generation = core_generation;
            slot.live = true;
            return Coordinate{.slot = static_cast<std::uint8_t>(index), .service_generation = slot.service_generation};
        }
        return std::nullopt;
    }

    [[nodiscard]] static bool seal(const std::uint8_t service_slot, const std::uint64_t service_generation,
                                   const std::uint64_t core_generation, const std::shared_ptr<Impl>& expected) noexcept {
        Resolver& resolver = instance();
        std::lock_guard lock(resolver.mutex);
        if (service_slot >= resolver.slots.size()) return false;
        Slot& slot = resolver.slots[service_slot];
        std::shared_ptr<Impl> owner = slot.owner.lock();
        if (!slot.live || slot.service_generation != service_generation || slot.core_generation != core_generation || owner != expected)
            return false;
        owner->in_flight->close();
        return true;
    }

    [[nodiscard]] static std::shared_ptr<Impl> resolve(const std::uint8_t service_slot, const std::uint64_t service_generation,
                                                       const std::uint64_t core_generation) noexcept {
        Resolver& resolver = instance();
        std::lock_guard lock(resolver.mutex);
        if (service_slot >= resolver.slots.size()) return {};
        const Slot& slot = resolver.slots[service_slot];
        std::shared_ptr<Impl> owner = slot.owner.lock();
        if (!slot.live || slot.service_generation != service_generation || slot.core_generation != core_generation || owner == nullptr ||
            owner->generation != core_generation)
            return {};
        return owner;
    }

    static void remove(const std::uint8_t service_slot, const std::uint64_t service_generation, const std::uint64_t core_generation,
                       const std::shared_ptr<Impl>& expected) noexcept {
        Resolver& resolver = instance();
        std::lock_guard lock(resolver.mutex);
        if (service_slot >= resolver.slots.size()) return;
        Slot& slot = resolver.slots[service_slot];
        if (expected == nullptr) return;
        const std::shared_ptr<Impl> registered = slot.owner.lock();
        if (!slot.live || slot.service_generation != service_generation || slot.core_generation != core_generation ||
            registered == nullptr || registered != expected)
            return;
        slot.live = false;
        slot.core_generation = 0U;
        slot.owner.reset();
    }

    std::mutex mutex;
    std::array<Slot, kCapacity> slots{};
};

std::expected<std::unique_ptr<ImageUpscaler>, ImageUpscalerStartError> ImageUpscaler::Create(
    const std::int32_t device_id, const std::uint64_t device_generation, const ImageUpscalerAggregateConfig config) noexcept {
    if (device_id < 0 || device_generation == 0U || !config.valid()) return std::unexpected(ImageUpscalerStartError::InvalidCoordinate);
    std::unique_ptr<ImageUpscaler> result{new (std::nothrow) ImageUpscaler};
    if (result == nullptr) return std::unexpected(ImageUpscalerStartError::Registration);
    const std::uint64_t generation = g_image_upscaler_generation.fetch_add(1U, std::memory_order_relaxed);
    if (generation == 0U || generation == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(ImageUpscalerStartError::Registration);
    try {
        result->owner_ = std::make_shared<Impl>(generation, device_id, device_generation, config);
    } catch (const ImageUpscalerStartError error) { return std::unexpected(error); } catch (...) {
        return std::unexpected(ImageUpscalerStartError::Registration);
    }
    result->generation_ = generation;
    return result;
}

std::expected<void, ImageUpscalerStartError> ImageUpscaler::Activate() noexcept {
    if (owner_ == nullptr || owner_->generation != generation_) return std::unexpected(ImageUpscalerStartError::Registration);
    const auto coordinate = Resolver::register_owner(owner_, generation_);
    if (!coordinate.has_value()) return std::unexpected(ImageUpscalerStartError::Registration);
    client_ =
        ImageUpscalerClient{coordinate->slot, coordinate->service_generation, generation_, owner_->device_id, owner_->device_generation};
    owner_->in_flight->open();
    return {};
}

ImageUpscaler::~ImageUpscaler() = default;

bool ImageUpscalerClient::valid() const noexcept {
    if (service_slot_ == kInvalidService || service_generation_ == 0U || core_generation_ == 0U || device_id_ < 0 ||
        device_generation_ == 0U)
        return false;
    std::shared_ptr<ImageUpscaler::Impl> owner = ImageUpscaler::Resolver::resolve(service_slot_, service_generation_, core_generation_);
    if (owner == nullptr || owner->device_id != device_id_ || owner->device_generation != device_generation_) return false;
    std::lock_guard claim_lock(owner->in_flight->mutex);
    return owner->in_flight->accepting;
}

ImageUpscalerProcessOwner ImageUpscalerClient::ClaimOperation() const noexcept {
    if (service_slot_ == kInvalidService || service_generation_ == 0U || core_generation_ == 0U || device_id_ < 0 ||
        device_generation_ == 0U)
        return {};
    std::shared_ptr<ImageUpscaler::Impl> owner = ImageUpscaler::Resolver::resolve(service_slot_, service_generation_, core_generation_);
    if (owner == nullptr || owner->device_id != device_id_ || owner->device_generation != device_generation_ ||
        !owner->in_flight->acquire())
        return {};
    // CLEANUP-IGNORE: This move-only owner has Upscale-specific process custody and release semantics.
    return ImageUpscalerProcessOwner{std::move(owner)};
}

ImageUpscalerProcessOwner::ImageUpscalerProcessOwner(std::shared_ptr<void> owner) noexcept : owner_(std::move(owner)) {}

ImageUpscalerProcessOwner::ImageUpscalerProcessOwner(ImageUpscalerProcessOwner&&) noexcept = default;

ImageUpscalerProcessOwner& ImageUpscalerProcessOwner::operator=(ImageUpscalerProcessOwner&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::move(other.owner_);
    }
    return *this;
}

ImageUpscalerProcessOwner::~ImageUpscalerProcessOwner() { release(); }

void ImageUpscalerProcessOwner::release() noexcept {
    if (owner_ == nullptr) return;
    static_cast<ImageUpscaler::Impl*>(owner_.get())->in_flight->release();
    owner_.reset();
}

ImageUpscalerProcessOwner::operator bool() const noexcept { return owner_ != nullptr; }

ImageUpscalerOutcome ImageUpscalerProcessOwner::run_rgba8(const ImageUpscalerModelHandle handle, const ImageUpscalerMode mode,
                                                          const std::uint8_t* source, const std::size_t source_pitch,
                                                          const std::uint32_t width, const std::uint32_t height, std::uint8_t* target,
                                                          const std::size_t target_pitch, const std::uintptr_t stream_handle,
                                                          ImageUpscalerCurrent current) {
    const auto stream = reinterpret_cast<cudaStream_t>(stream_handle);
    if (source == nullptr || target == nullptr || stream == nullptr || width == 0U || height == 0U ||
        width > std::numeric_limits<std::uint32_t>::max() / 4U || height > std::numeric_limits<std::uint32_t>::max() / 4U)
        throw std::invalid_argument("invalid Upscale execution geometry");
    if (source_pitch < static_cast<std::size_t>(width) * 4U || target_pitch < static_cast<std::size_t>(width) * 16U ||
        source_pitch > std::numeric_limits<std::size_t>::max() / height ||
        target_pitch > std::numeric_limits<std::size_t>::max() / (static_cast<std::size_t>(height) * 4U))
        throw std::invalid_argument("invalid Upscale execution pitch");
    if (owner_ == nullptr) throw std::invalid_argument("missing Upscale execution owner");
    if (!current()) return ImageUpscalerOutcome::Cancelled;
    auto* const owner = static_cast<ImageUpscaler::Impl*>(owner_.get());
    if (handle.device_id != owner->device_id || handle.backend_generation != owner->device_generation)
        throw std::invalid_argument("invalid Upscale execution identity");
    if (mode == ImageUpscalerMode::Basic)
        return owner->basic.run(source, source_pitch, width, height, target, target_pitch, stream, current, owner->checkpoint);
    const ImageUpscalerKind kind = mode == ImageUpscalerMode::ShiftLUT ? ImageUpscalerKind::ShiftLUT : ImageUpscalerKind::RealPLKSR;
    ImageUpscaler::Impl::Slot& slot = owner->slot(kind);
    std::lock_guard lock(slot.mutex);
    if (slot.handle != handle) throw std::invalid_argument("invalid Upscale method identity");
    enum class ActivationProgress { PreexistingResident, Activating, CompletedResident };
    auto activation = slot.runtime ? ActivationProgress::PreexistingResident : ActivationProgress::Activating;
    const auto settle = [&](const std::exception_ptr& primary, const bool retire_runtime) {
        if (slot.runtime && slot.runtime->Settle() != cudaSuccess)
            throw ImageUpscalerUnsettledFailure{primary, slot.runtime->cleanup_failure()};
        const auto settled = cudaStreamSynchronize(stream);
        if (settled != cudaSuccess)
            throw ImageUpscalerUnsettledFailure{
                primary, std::make_exception_ptr(mmltk::frameworks::gpu::CudaError(settled, "settle neural Upscale consumer"))};
        if (slot.runtime) {
            try {
                slot.runtime->mark_consumed(stream);
            } catch (...) { throw ImageUpscalerUnsettledFailure{primary, std::current_exception()}; }
            if (retire_runtime) {
                if (slot.runtime->Stop() != cudaSuccess) throw ImageUpscalerUnsettledFailure{primary, slot.runtime->cleanup_failure()};
                slot.runtime.reset();
            }
        }
    };
    const auto cancel = [&]() {
        settle({}, activation == ActivationProgress::Activating);
        return ImageUpscalerOutcome::Cancelled;
    };
    try {
        if (slot.runtime == nullptr) {
            if (!image_upscaler_admitted(owner->checkpoint, ImageUpscalerExecutionStage::InitializationAdmitted, current)) return cancel();
            const auto initialized =
                kind == ImageUpscalerKind::ShiftLUT ? owner->initialize_onnx(slot, current) : owner->initialize_tensorrt(slot, current);
            if (initialized == ImageUpscalerOutcome::Cancelled) return cancel();
            if (slot.runtime->Activate(current) == ImageUpscalerOutcome::Cancelled) return cancel();
            activation = ActivationProgress::CompletedResident;
        }
        if (!image_upscaler_admitted(owner->checkpoint, ImageUpscalerExecutionStage::PreprocessAdmitted, current)) return cancel();
        mmltk::common::logging::trace([&](auto& logger) {
            logger.trace(
                "event=image_upscaler_conversion_buffers mode={} width={} height={} source_pitch={} target_pitch={} "
                "source={} target={} stream={}",
                static_cast<std::uint32_t>(mode), width, height, source_pitch, target_pitch, reinterpret_cast<std::uintptr_t>(source),
                reinterpret_cast<std::uintptr_t>(target), stream_handle);
        });
        const ImageUpscalerRequest request{.device_pixels = source,
                                           .source_pitch = source_pitch,
                                           .target_pixels = target,
                                           .target_pitch = target_pitch,
                                           .source_width = width,
                                           .source_height = height,
                                           .crop_x = 0U,
                                           .crop_y = 0U,
                                           .crop_width = width,
                                           .crop_height = height,
                                           .current = current};
        const ImageUpscalerRuntimeOutput output = slot.runtime->enqueue(request, stream);
        if (owner->checkpoint) owner->checkpoint(ImageUpscalerExecutionStage::RuntimeEnqueued);
        if (output.outcome == ImageUpscalerOutcome::Cancelled || !current()) return cancel();
        if (output.device_pixels == nullptr || output.width != width * 4U || output.height != height * 4U)
            throw std::runtime_error("invalid Upscale runtime output");
        slot.runtime->mark_consumed(stream);
        ensure_cuda_ok(cudaPeekAtLastError(), "convert neural Upscale output");
        return ImageUpscalerOutcome::Completed;
    } catch (const ImageUpscalerUnsettledFailure&) { throw; } catch (...) {
        const auto failure = std::current_exception();
        settle(failure, true);
        if (activation == ActivationProgress::Activating) throw ImageUpscalerInitializationFailure(failure);
        std::rethrow_exception(failure);
    }
}

std::uintptr_t ImageUpscalerProcessOwner::operation_stream(const ImageUpscalerMode mode, const int device_id,
                                                           ImageUpscalerCurrent current) {
    if (!current()) return 0U;
    const std::size_t index = static_cast<std::size_t>(mode);
    if (owner_ == nullptr) return 0U;
    auto* const owner = static_cast<ImageUpscaler::Impl*>(owner_.get());
    if (index >= static_cast<std::size_t>(ImageUpscalerMode::Count) || device_id < 0 || device_id != owner->device_id) return 0U;
    std::lock_guard lock(owner->lifecycle_mutex);
    if (mode == ImageUpscalerMode::Basic) return reinterpret_cast<std::uintptr_t>(owner->basic.ensure_stream(device_id));
    cudaStream_t& stream = owner->operation_streams[index - 1U];
    if (stream == nullptr) {
        ensure_cuda_ok(cudaSetDevice(device_id), "bind neural Upscale operation device");
        ensure_cuda_ok(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "create neural Upscale operation stream");
    }
    return reinterpret_cast<std::uintptr_t>(stream);
}
bool ImageUpscalerProcessOwner::graph_replay(const ImageUpscalerMode mode) const {
    if (!owner_ || mode == ImageUpscalerMode::Basic || mode >= ImageUpscalerMode::Count) return false;
    auto& slot = static_cast<ImageUpscaler::Impl*>(owner_.get())
                     ->slot(mode == ImageUpscalerMode::ShiftLUT ? ImageUpscalerKind::ShiftLUT : ImageUpscalerKind::RealPLKSR);
    std::lock_guard lock(slot.mutex);
    return slot.runtime && slot.runtime->graph_replay();
}

ImageUpscalerClient ImageUpscaler::client() const noexcept {
    return owner_ != nullptr && owner_->generation == generation_ ? client_ : ImageUpscalerClient{};
}

ImageUpscalerStatus ImageUpscaler::Stop() noexcept {
    if (owner_ == nullptr || owner_->generation != generation_) return cudaSuccess;
    const bool activated = client_.service_generation_ != 0U;
    if (activated) {
        if (!Resolver::seal(client_.service_slot_, client_.service_generation_, client_.core_generation_, owner_))
            return cudaErrorInvalidResourceHandle;
        Resolver::remove(client_.service_slot_, client_.service_generation_, client_.core_generation_, owner_);
        client_ = {};
    }
    const cudaError_t failure = owner_->Stop();
    if (failure == cudaSuccess) { owner_.reset(); }
    return static_cast<ImageUpscalerStatus>(failure);
}

std::uint64_t ImageUpscaler::core_generation() const noexcept {
    return owner_ != nullptr && owner_->generation == generation_ ? generation_ : 0U;
}

std::exception_ptr ImageUpscaler::cleanup_failure() const noexcept {
    if (!owner_) return {};
    return mmltk::frameworks::gpu::combine_image_failures(
        owner_->cleanup.failure(),
        mmltk::frameworks::gpu::combine_image_failures(owner_->runtime_cleanup_failure, owner_->basic.cleanup_failure()));
}

}  // namespace mmltk::backend::imaging::upscale
