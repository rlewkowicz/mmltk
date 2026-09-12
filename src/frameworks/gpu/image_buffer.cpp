#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include "src/common/io/noexcept_io.h"
#include "src/common/io/scoped_fd.h"
#include "src/common/system/execution_policy.h"

namespace mmltk::frameworks::gpu {
[[nodiscard]] std::uint64_t next_image_allocation_identity() {
    static std::atomic<std::uint64_t> next{1U};
    auto value = next.load(std::memory_order_relaxed);
    do {
        if (value == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("image allocation identity exhausted");
    } while (!next.compare_exchange_weak(value, value + 1U, std::memory_order_relaxed));
    return value;
}
namespace {
class TransferTrace final {
   public:
    TransferTrace() {
        const auto* path = std::getenv("MMLTK_NUMA_TRANSFER_TRACE_FILE");
        if (path && *path) descriptor_.reset(::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
    }
    [[nodiscard]] bool enabled() const noexcept { return descriptor_.get() >= 0; }
    void Write(const char* record, const int size, const std::size_t capacity) const noexcept {
        if (size > 0 && static_cast<std::size_t>(size) < capacity)
            mmltk::common::io::write_all_noexcept(descriptor_.get(), {record, static_cast<std::size_t>(size)});
    }

   private:
    mmltk::common::io::ScopedFd descriptor_;
};
void LogStaging(const DeviceContext& receiver, int source_device, std::size_t bytes) noexcept {
    const TransferTrace trace;
    if (!trace.enabled()) return;
    const auto* execution = receiver.execution();
    char record[256];
    const int size =
        std::snprintf(record, sizeof(record),
                      "{\"event\":\"image_host_staging\",\"source_device\":%d,\"receiver_device\":%d,\"node\":%d,\"bytes\":%zu,"
                      "\"completion\":\"source-event,synchronous-d2h,receiver-stream\"}\n",
                      source_device, receiver.device(), execution ? execution->placement.numa_node : -1, bytes);
    trace.Write(record, size, sizeof(record));
}

void CheckCuda(const char* operation, const CUresult result) {
    if (result == CUDA_SUCCESS) return;
    const char* detail = nullptr;
    static_cast<void>(cuGetErrorString(result, &detail));
    throw std::runtime_error(std::string{operation} + ": " + (detail == nullptr ? "unknown CUDA error" : detail));
}
[[nodiscard]] std::exception_ptr CudaFailure(const char* operation, const CUresult result) noexcept {
    if (result == CUDA_SUCCESS) return {};
    try {
        CheckCuda(operation, result);
    } catch (...) { return std::current_exception(); }
    return {};
}
[[noreturn]] void ThrowExecutionFailure(const ImageCopyBackend::StreamSettlement& settled, std::exception_ptr initiating = {}) {
    throw ImageStreamExecutionFailure(combine_image_failures(std::move(initiating), settled.failure));
}
void LogCopyFailure(const DeviceContext& receiver, const char* boundary, const bool completion_reached) noexcept {
    const TransferTrace trace;
    if (!trace.enabled()) return;
    char record[256];
    const int size = std::snprintf(
        record, sizeof(record), "{\"event\":\"image_copy_failure\",\"boundary\":\"%s\",\"receiver_device\":%d,\"completion_reached\":%s}\n",
        boundary, receiver.device(), completion_reached ? "true" : "false");
    trace.Write(record, size, sizeof(record));
}

template <typename ReadView>
ImageCopyBackend::StreamSettlement RetainUnsettledRead(ImageStream& stream, ReadView& source, std::optional<ReadView>& retained) noexcept {
    const auto settled = stream.Settle();
    if (!settled.completion_reached) {
        source.Quarantine();
        retained.emplace(std::move(source));
    }
    return settled;
}
[[nodiscard]] constexpr bool CompletionBoundaryReached(const CUresult result) noexcept {
    switch (result) {
        case CUDA_ERROR_DEINITIALIZED:
        case CUDA_ERROR_NOT_INITIALIZED:
        case CUDA_ERROR_INVALID_CONTEXT:
        case CUDA_ERROR_INVALID_HANDLE:
            return false;
        default:
            return true;
    }
}

class NativeImageCopyBackend final : public ImageCopyBackend {
   public:
    [[nodiscard]] std::uintptr_t CreateContext(const int device, const DeviceContextMode mode) override {
        CheckCuda("initialize CUDA", cuInit(0U));
        CUdevice selected = 0;
        CheckCuda("select CUDA device", cuDeviceGet(&selected, device));
        CUcontext context = nullptr;
        if (mode == DeviceContextMode::PrimaryInterop)
            CheckCuda("retain CUDA primary context", cuDevicePrimaryCtxRetain(&context, selected));
        else
            CheckCuda("create CUDA context", cuCtxCreate(&context, nullptr, CU_CTX_SCHED_AUTO, selected));
        return reinterpret_cast<std::uintptr_t>(context);
    }
    void DestroyContext(const int device, const DeviceContextMode mode, const std::uintptr_t context) noexcept override {
        if (context == 0U) return;
        if (mode == DeviceContextMode::Isolated) {
            static_cast<void>(cuCtxDestroy(reinterpret_cast<CUcontext>(context)));
            return;
        }
        CUcontext current = nullptr;
        if (cuCtxGetCurrent(&current) == CUDA_SUCCESS && current == reinterpret_cast<CUcontext>(context))
            static_cast<void>(cuCtxSetCurrent(nullptr));
        CUdevice selected = 0;
        if (cuDeviceGet(&selected, device) == CUDA_SUCCESS) static_cast<void>(cuDevicePrimaryCtxRelease(selected));
    }
    void BindContext(const std::uintptr_t context) override {
        CheckCuda("bind CUDA context", cuCtxSetCurrent(reinterpret_cast<CUcontext>(context)));
    }
    [[nodiscard]] std::uintptr_t CreateStream(const std::uintptr_t context) override {
        BindContext(context);
        CUstream stream = nullptr;
        CheckCuda("create CUDA stream", cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING));
        return reinterpret_cast<std::uintptr_t>(stream);
    }
    void DestroyStream(const std::uintptr_t context, const std::uintptr_t stream) noexcept override {
        if (stream == 0U) return;
        static_cast<void>(cuCtxSetCurrent(reinterpret_cast<CUcontext>(context)));
        static_cast<void>(cuStreamDestroy(reinterpret_cast<CUstream>(stream)));
    }
    [[nodiscard]] std::uintptr_t CreateEvent(const std::uintptr_t context) override {
        BindContext(context);
        CUevent event = nullptr;
        // CLEANUP-IGNORE: CUDA events and streams have distinct driver types, flags, and destruction obligations.
        CheckCuda("create CUDA event", cuEventCreate(&event, CU_EVENT_DISABLE_TIMING));
        return reinterpret_cast<std::uintptr_t>(event);
    }
    void DestroyEvent(const std::uintptr_t context, const std::uintptr_t event) noexcept override {
        if (event == 0U) return;
        static_cast<void>(cuCtxSetCurrent(reinterpret_cast<CUcontext>(context)));
        static_cast<void>(cuEventDestroy(reinterpret_cast<CUevent>(event)));
    }
    [[nodiscard]] ImagePlaneView AllocatePlane(const std::uintptr_t context, const ImagePlaneKind kind, const std::uint32_t width,
                                               const std::uint32_t height) override {
        BindContext(context);
        CUdeviceptr data = 0U;
        std::size_t pitch = 0U;
        CheckCuda("allocate CUDA image plane", cuMemAllocPitch(&data, &pitch, static_cast<std::size_t>(width) * 4U, height, 16U));
        return {
            .data = data,
            .descriptor =
                {
                    .kind = kind,
                    .format = ImageFormat::Rgba8,
                    .width = width,
                    .height = height,
                    .pitch_bytes = pitch,
                },
        };
    }
    void FreePlane(const std::uintptr_t context, const CUdeviceptr data) noexcept override {
        if (data == 0U) return;
        static_cast<void>(cuCtxSetCurrent(reinterpret_cast<CUcontext>(context)));
        static_cast<void>(cuMemFree(data));
    }
    void ClearPlane(const std::uintptr_t context, const std::uintptr_t stream, const ImagePlaneView& plane) override {
        BindContext(context);
        CheckCuda("clear CUDA image plane", cuMemsetD2D8Async(plane.data, plane.descriptor.pitch_bytes, 0U, plane.descriptor.row_bytes(),
                                                              plane.descriptor.height, reinterpret_cast<CUstream>(stream)));
    }
    [[nodiscard]] std::shared_ptr<void> AllocatePinned(const std::uintptr_t receiver_context,
                                                       const mmltk::common::system::ExecutionPlacement* placement,
                                                       const std::size_t bytes) override {
        if (!placement) throw std::invalid_argument("image staging requires receiver placement");
        auto storage = std::make_shared<PinnedHostBuffer>(reinterpret_cast<CUcontext>(receiver_context), *placement, true);
        storage->ensure_bytes(bytes);
        return std::shared_ptr<void>(storage, storage->data());
    }
    [[nodiscard]] bool CanAccessPeer(const int receiver, const int source) override {
        CUdevice receiver_device = 0;
        CUdevice source_device = 0;
        int accessible = 0;
        CheckCuda("select receiver CUDA device", cuDeviceGet(&receiver_device, receiver));
        CheckCuda("select source CUDA device", cuDeviceGet(&source_device, source));
        CheckCuda("query CUDA peer access", cuDeviceCanAccessPeer(&accessible, receiver_device, source_device));
        return accessible != 0;
    }
    void WaitEvent(const std::uintptr_t receiver_context, const std::uintptr_t receiver_stream,
                   const std::uintptr_t source_event) override {
        BindContext(receiver_context);
        CheckCuda("wait for source image",
                  cuStreamWaitEvent(reinterpret_cast<CUstream>(receiver_stream), reinterpret_cast<CUevent>(source_event), 0U));
    }
    void CopySameDevice(const std::uintptr_t context, const std::uintptr_t stream, const ImagePlaneView& destination,
                        const std::uintptr_t source_context, const ImagePlaneView& source) override {
        CopyDevice(context, stream, destination, source_context, source, "copy same-device image");
    }
    void CopyPeer(const std::uintptr_t receiver_context, const std::uintptr_t receiver_stream, const int, const ImagePlaneView& destination,
                  const std::uintptr_t source_context, const int, const ImagePlaneView& source) override {
        CopyDevice(receiver_context, receiver_stream, destination, source_context, source, "copy peer image");
    }
    void CopyDeviceToHost(const std::uintptr_t source_context, const ImagePlaneView& source, void* const destination,
                          const std::size_t destination_pitch) override {
        BindContext(source_context);
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = source.data;
        copy.srcPitch = source.descriptor.pitch_bytes;
        copy.dstMemoryType = CU_MEMORYTYPE_HOST;
        copy.dstHost = destination;
        copy.dstPitch = destination_pitch;
        copy.WidthInBytes = source.descriptor.row_bytes();
        copy.Height = source.descriptor.height;
        CheckCuda("stage source image", cuMemcpy2D(&copy));
    }
    void CopyHostToDevice(const std::uintptr_t receiver_context, const std::uintptr_t receiver_stream, const void* const source,
                          const std::size_t source_pitch, const ImagePlaneView& destination) override {
        BindContext(receiver_context);
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_HOST;
        copy.srcHost = source;
        copy.srcPitch = source_pitch;
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = destination.data;
        copy.dstPitch = destination.descriptor.pitch_bytes;
        copy.WidthInBytes = destination.descriptor.row_bytes();
        copy.Height = destination.descriptor.height;
        CheckCuda("copy staged image", cuMemcpy2DAsync(&copy, reinterpret_cast<CUstream>(receiver_stream)));
    }
    void RecordEvent(const std::uintptr_t context, const std::uintptr_t stream, const std::uintptr_t event) override {
        BindContext(context);
        CheckCuda("record image completion", cuEventRecord(reinterpret_cast<CUevent>(event), reinterpret_cast<CUstream>(stream)));
    }
    void SynchronizeEvent(const std::uintptr_t context, const std::uintptr_t event) override {
        BindContext(context);
        CheckCuda("synchronize image event", cuEventSynchronize(reinterpret_cast<CUevent>(event)));
    }
    StreamSettlement SettleStream(const std::uintptr_t context, const std::uintptr_t stream) noexcept override {
        const auto bound = cuCtxSetCurrent(reinterpret_cast<CUcontext>(context));
        if (bound != CUDA_SUCCESS) return {.failure = CudaFailure("bind CUDA context for stream settlement", bound)};
        const auto settled = cuStreamSynchronize(reinterpret_cast<CUstream>(stream));
        return {
            .completion_reached = CompletionBoundaryReached(settled),
            .failure = CudaFailure("synchronize image stream", settled),
        };
    }

   private:
    void CopyDevice(const std::uintptr_t receiver_context, const std::uintptr_t receiver_stream, const ImagePlaneView& destination,
                    const std::uintptr_t source_context, const ImagePlaneView& source, const char* const operation) {
        BindContext(receiver_context);
        CUDA_MEMCPY3D_PEER copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = source.data;
        copy.srcPitch = source.descriptor.pitch_bytes;
        copy.srcContext = reinterpret_cast<CUcontext>(source_context);
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = destination.data;
        copy.dstPitch = destination.descriptor.pitch_bytes;
        copy.dstContext = reinterpret_cast<CUcontext>(receiver_context);
        copy.WidthInBytes = source.descriptor.row_bytes();
        copy.Height = source.descriptor.height;
        copy.Depth = 1U;
        CheckCuda(operation, cuMemcpy3DPeerAsync(&copy, reinterpret_cast<CUstream>(receiver_stream)));
    }
};

}  // namespace

std::shared_ptr<ImageCopyBackend> cuda_image_copy_backend() {
    static std::shared_ptr<ImageCopyBackend> backend = std::make_shared<NativeImageCopyBackend>();
    return backend;
}

std::optional<DeviceExecution> ImageCopyBackend::ResolveExecution(int device, int numa_node) {
    return resolve_device_execution(device, mmltk::common::system::NumaTopology::Capture(), numa_node);
}
struct DeviceContext::State final {
    State(const int selected, std::shared_ptr<ImageCopyBackend> implementation, const DeviceContextMode selected_mode, int numa_node,
          std::optional<DeviceExecution> selected_execution)
        : device(selected), mode(selected_mode), backend(std::move(implementation)) {
        if (device < 0 || !backend) throw std::invalid_argument("image device context is unavailable");
        execution = selected_execution ? std::move(selected_execution) : backend->ResolveExecution(device, numa_node);
        std::optional<mmltk::common::system::ScopedExecutionPolicy> policy;
        if (execution) {
            if (execution->device != device) throw std::invalid_argument("image context placement device mismatch");
            policy.emplace(mmltk::common::system::ExecutionPolicyRequest{
                execution->placement.cpus, {}, 0, execution->placement.numa_node, -10, false});
        }
        context = backend->CreateContext(device, mode);
        if (context == 0U) throw std::runtime_error("image device context creation returned no context");
    }
    ~State() { backend->DestroyContext(device, mode, context); }
    int device = -1;
    DeviceContextMode mode = DeviceContextMode::Isolated;
    std::shared_ptr<ImageCopyBackend> backend;
    std::uintptr_t context = 0U;
    std::optional<DeviceExecution> execution;
};

DeviceContext::DeviceContext(const int device, std::shared_ptr<ImageCopyBackend> backend, const DeviceContextMode mode, int numa_node,
                             std::optional<DeviceExecution> execution)
    : state_(std::make_shared<State>(device, std::move(backend), mode, numa_node, std::move(execution))) {}
DeviceContext::~DeviceContext() = default;
int DeviceContext::device() const noexcept { return state_->device; }
const DeviceExecution* DeviceContext::execution() const noexcept { return state_->execution ? &*state_->execution : nullptr; }
void DeviceContext::Bind() const { state_->backend->BindContext(state_->context); }
std::uintptr_t DeviceContext::CreateEvent() const {
    const auto event = state_->backend->CreateEvent(state_->context);
    if (event == 0U) throw std::runtime_error("image completion event creation returned no event");
    return event;
}
DeviceContext DeviceContext::OnDevice(int device, std::optional<DeviceExecution> execution) const {
    if (execution && execution->device != device) throw std::invalid_argument("display execution device mismatch");
    if (device == this->device()) return *this;
    return DeviceContext(device, state_->backend, DeviceContextMode::PrimaryInterop, -1, std::move(execution));
}
void DeviceContext::DestroyEvent(std::uintptr_t event) const noexcept {
    if (event != 0U) state_->backend->DestroyEvent(state_->context, event);
}
void ImageStream::Record(std::uintptr_t event) { context_.state_->backend->RecordEvent(context_.state_->context, stream_, event); }
void ImageStream::AwaitEvent(std::uintptr_t event) { context_.state_->backend->WaitEvent(context_.state_->context, stream_, event); }

ImageStream::ImageStream(DeviceContext context) : context_(std::move(context)) {
    stream_ = context_.state_->backend->CreateStream(context_.state_->context);
    if (stream_ == 0U) throw std::runtime_error("image stream creation returned no stream");
}
ImageStream::~ImageStream() {
    if (stream_ != 0U) context_.state_->backend->DestroyStream(context_.state_->context, stream_);
}
ImageStream::ImageStream(ImageStream&& other) noexcept
    : context_(std::move(other.context_)),
      stream_(std::exchange(other.stream_, 0U)),
      settlement_failure_(std::move(other.settlement_failure_)) {}
ImageStream& ImageStream::operator=(ImageStream&& other) noexcept {
    if (this == &other) return *this;
    if (stream_ != 0U) context_.state_->backend->DestroyStream(context_.state_->context, stream_);
    context_ = std::move(other.context_);
    stream_ = std::exchange(other.stream_, 0U);
    settlement_failure_ = std::move(other.settlement_failure_);
    return *this;
}
void ImageStream::Synchronize() {
    const auto settled = Settle();
    if (!settled.completion_reached || settled.failure) ThrowExecutionFailure(settled);
}
ImageCopyBackend::StreamSettlement ImageStream::Settle() noexcept {
    auto settled = context_.state_->backend->SettleStream(context_.state_->context, stream_);
    if ((!settled.completion_reached || settled.failure) && !is_image_execution_failure(settled.failure)) {
        try {
            throw ImageStreamExecutionFailure(settled.failure);
        } catch (...) { settled.failure = std::current_exception(); }
    }
    settlement_failure_ = combine_image_failures(settlement_failure_, settled.failure);
    return {.completion_reached = settled.completion_reached, .failure = settlement_failure_};
}
void ImageStream::RethrowAfterSettlement(std::exception_ptr submission) {
    const auto settled = Settle();
    if (!settled.completion_reached || settled.failure) ThrowExecutionFailure(settled, submission);
    std::rethrow_exception(submission);
}

struct ImageBuffer::State final {
    explicit State(DeviceContext selected) : context(std::move(selected)) {
        completion = context.state_->backend->CreateEvent(context.state_->context);
        if (completion == 0U) throw std::runtime_error("image completion event creation returned no event");
    }
    ~State() {
        std::unique_lock lock(access);
        if (plane.data != 0U && !external_storage) context.state_->backend->FreePlane(context.state_->context, plane.data);
        staging_owner.reset();
        context.state_->backend->DestroyEvent(context.state_->context, completion);
    }
    [[nodiscard]] std::uint64_t BeginWrite() {
        if (unsettled_source) throw std::runtime_error("image receiver retains an unsettled source");
        if (revision == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("image revision exhausted");
        const auto next = revision + 1U;
        revision = 0U;
        return next;
    }
    void EnsurePlane(const ImagePlaneKind kind, const std::uint32_t width, const std::uint32_t height) {
        if (unavailable.load(std::memory_order_acquire)) throw std::runtime_error("image storage is quarantined");
        if (plane.data != 0U && capacity_width >= width && capacity_height >= height && plane.descriptor.kind == kind) {
            plane.descriptor.width = width;
            plane.descriptor.height = height;
            return;
        }
        const std::uint32_t next_width = std::max(capacity_width, width);
        const std::uint32_t next_height = std::max(capacity_height, height);
        const auto identity = next_image_allocation_identity();
        ImagePlaneView candidate = context.state_->backend->AllocatePlane(context.state_->context, kind, next_width, next_height);
        if (!candidate.valid()) throw std::runtime_error("image plane allocation returned an invalid plane");
        candidate.allocation = {identity, next_width, next_height, allocation_owner};
        if (plane.data != 0U && !external_storage) context.state_->backend->FreePlane(context.state_->context, plane.data);
        external_storage.reset();
        external_bytes = 0U;
        plane = candidate;
        capacity_width = next_width;
        capacity_height = next_height;
        plane.descriptor.width = width;
        plane.descriptor.height = height;
    }
    void EnsureStaging(const std::size_t bytes) {
        if (staging_bytes >= bytes) return;
        const auto* execution = context.execution();
        auto candidate =
            context.state_->backend->AllocatePinned(context.state_->context, execution ? &execution->placement : nullptr, bytes);
        if (candidate == nullptr) throw std::runtime_error("pinned staging allocation returned no memory");
        staging_owner.reset();
        staging_owner = std::move(candidate);
        staging = staging_owner.get();
        staging_bytes = bytes;
    }
    DeviceContext context;
    const std::uint64_t allocation_owner = next_image_allocation_identity();
    mutable std::shared_mutex access;
    std::atomic_bool unavailable{false};
    ImagePlaneView plane{};
    std::shared_ptr<void> external_storage;
    std::size_t external_bytes = 0U;
    std::shared_ptr<void> staging_owner;
    void* staging = nullptr;
    std::size_t staging_bytes = 0U;
    std::uint32_t capacity_width = 0U;
    std::uint32_t capacity_height = 0U;
    std::uintptr_t completion = 0U;
    std::uint64_t revision = 0U;
    std::optional<BorrowedImageReadView> unsettled_source;
};

struct BorrowedImageReadView::Lease final {
    explicit Lease(std::shared_ptr<ImageBuffer::State> owner, const std::uintptr_t product_completion = 0U,
                   std::shared_ptr<void> shared_product_lease = {}, const std::uint64_t product_generation = 0U,
                   std::shared_lock<std::shared_mutex>* shared_product_lock = nullptr,
                   std::shared_ptr<const std::function<void()>> availability_callback = {}, bool nonblocking = false)
        : state(std::move(owner)),
          completion(product_completion == 0U ? state->completion : product_completion),
          product_lease(std::move(shared_product_lease)),
          product_lock(shared_product_lock),
          lock(state->access, std::defer_lock),
          revision(product_lease ? product_generation : state->revision),
          availability(std::move(availability_callback)) {
        if (nonblocking)
            static_cast<void>(lock.try_lock());
        else
            lock.lock();
    }
    ~Lease() {
        if (lock.owns_lock()) {
            lock.unlock();
            state.reset();
        }
    }
    std::shared_ptr<ImageBuffer::State> state;
    std::uintptr_t completion = 0U;
    std::shared_ptr<void> product_lease;
    std::shared_lock<std::shared_mutex>* product_lock = nullptr;
    std::shared_lock<std::shared_mutex> lock;
    std::uint64_t revision = 0U;
    std::shared_ptr<const std::function<void()>> availability;
};

BorrowedImageReadView::BorrowedImageReadView() noexcept = default;
BorrowedImageReadView::~BorrowedImageReadView() = default;
BorrowedImageReadView::BorrowedImageReadView(std::unique_ptr<Lease> lease) noexcept : lease_(std::move(lease)) {}
BorrowedImageReadView::BorrowedImageReadView(BorrowedImageReadView&&) noexcept = default;
BorrowedImageReadView& BorrowedImageReadView::operator=(BorrowedImageReadView&&) noexcept = default;
bool BorrowedImageReadView::valid() const noexcept {
    return lease_ && !lease_->state->unavailable.load(std::memory_order_acquire) && lease_->state->plane.valid() && lease_->revision != 0U;
}
int BorrowedImageReadView::device() const noexcept { return valid() ? lease_->state->context.device() : -1; }
std::uint64_t BorrowedImageReadView::revision() const noexcept { return valid() ? lease_->revision : 0U; }
ImagePlaneView BorrowedImageReadView::plane() const noexcept { return valid() ? lease_->state->plane : ImagePlaneView{}; }

void BorrowedImageReadView::Quarantine() noexcept {
    if (!lease_) return;
    lease_->state->unavailable.store(true, std::memory_order_release);
    if (lease_->lock.owns_lock()) lease_->lock.unlock();
    if (lease_->product_lock && lease_->product_lock->owns_lock()) lease_->product_lock->unlock();
    if (lease_->availability) {
        try {
            (*lease_->availability)();
        } catch (...) {}
    }
}

ImageBuffer::ImageBuffer(DeviceContext context) : state_(std::make_shared<State>(std::move(context))) {}
ImageBuffer::~ImageBuffer() {
    if (!state_) return;
    std::unique_lock retirement(state_->access);
}
ImageBuffer::ImageBuffer(ImageBuffer&&) noexcept = default;
ImageBuffer& ImageBuffer::operator=(ImageBuffer&& other) noexcept {
    if (this != &other) {
        auto retired = std::move(state_);
        if (retired) { std::unique_lock retirement(retired->access); }
        state_ = std::move(other.state_);
    }
    return *this;
}

void ImageBuffer::Write(ImageStream& stream, const ImagePlaneKind kind, const std::uint32_t width, const std::uint32_t height,
                        const std::function<void(ImagePlaneView, std::uintptr_t)>& submit) {
    if (width == 0U || height == 0U || !submit) throw std::invalid_argument("image write request is invalid");
    if (stream.context_.state_ != state_->context.state_) throw std::invalid_argument("image stream does not belong to the buffer context");
    std::unique_lock lock(state_->access);
    const auto next_revision = state_->BeginWrite();
    state_->EnsurePlane(kind, width, height);
    submit(state_->plane, stream.stream_);
    state_->context.state_->backend->RecordEvent(state_->context.state_->context, stream.stream_, state_->completion);
    state_->revision = next_revision;
}

ImageCopyPath ImageBuffer::CopyFrom(ImageStream& stream, BorrowedImageReadView source) {
    if (!source.valid()) throw std::invalid_argument("borrowed source image is unavailable");
    if (source.lease_->state == state_) throw std::invalid_argument("an image buffer cannot copy from itself");
    if (stream.context_.state_ != state_->context.state_)
        throw std::invalid_argument("image stream does not belong to the receiver context");
    if (state_->context.state_->backend != source.lease_->state->context.state_->backend)
        throw std::invalid_argument("source and receiver use different image backends");
    std::unique_lock lock(state_->access);
    const auto next_revision = state_->BeginWrite();
    const ImagePlaneView source_plane = source.plane();
    state_->EnsurePlane(source_plane.descriptor.kind, source_plane.descriptor.width, source_plane.descriptor.height);
    auto& backend = *state_->context.state_->backend;
    bool reads_submitted = false;
    bool receiver_completed = false;
    try {
        const int receiver_device = state_->context.device();
        const int source_device = source.device();
        ImageCopyPath path = ImageCopyPath::SameDevice;
        if (receiver_device == source_device) {
            backend.WaitEvent(state_->context.state_->context, stream.stream_, source.lease_->completion);
            reads_submitted = true;
            backend.CopySameDevice(state_->context.state_->context, stream.stream_, state_->plane,
                                   source.lease_->state->context.state_->context, source_plane);
        } else if (backend.CanAccessPeer(receiver_device, source_device)) {
            backend.WaitEvent(state_->context.state_->context, stream.stream_, source.lease_->completion);
            reads_submitted = true;
            backend.CopyPeer(state_->context.state_->context, stream.stream_, receiver_device, state_->plane,
                             source.lease_->state->context.state_->context, source_device, source_plane);
            path = ImageCopyPath::Peer;
        } else {
            const std::size_t row_bytes = source_plane.descriptor.row_bytes();
            if (source_plane.descriptor.height > std::numeric_limits<std::size_t>::max() / row_bytes)
                throw std::overflow_error("staged image byte size exceeds addressable memory");
            state_->EnsureStaging(row_bytes * source_plane.descriptor.height);
            LogStaging(state_->context, source_device, row_bytes * source_plane.descriptor.height);
            source.lease_->state->context.state_->backend->SynchronizeEvent(source.lease_->state->context.state_->context,
                                                                            source.lease_->completion);
            reads_submitted = true;
            backend.CopyDeviceToHost(source.lease_->state->context.state_->context, source_plane, state_->staging, row_bytes);
            backend.CopyHostToDevice(state_->context.state_->context, stream.stream_, state_->staging, row_bytes, state_->plane);
            path = ImageCopyPath::PinnedStaging;
        }
        backend.RecordEvent(state_->context.state_->context, stream.stream_, state_->completion);
        auto settled = stream.Settle();
        receiver_completed = settled.completion_reached;
        if (!settled.completion_reached || settled.failure) ThrowExecutionFailure(settled);
        state_->revision = next_revision;
        return path;
    } catch (...) {
        if (reads_submitted && !receiver_completed) {
            const auto settled = RetainUnsettledRead(stream, source, state_->unsettled_source);
            receiver_completed = settled.completion_reached;
            if (!receiver_completed || settled.failure) {
                LogCopyFailure(state_->context, "scalar", receiver_completed);
                ThrowExecutionFailure(settled, std::current_exception());
            }
        }
        LogCopyFailure(state_->context, "scalar", !reads_submitted || receiver_completed);
        throw;
    }
}

BorrowedImageReadView ImageBuffer::Borrow() const {
    auto lease = std::make_unique<BorrowedImageReadView::Lease>(state_);
    if (lease->state->unavailable.load(std::memory_order_acquire) || !lease->state->plane.valid() || lease->state->revision == 0U)
        return {};
    return BorrowedImageReadView{std::move(lease)};
}
std::uint32_t ImageBuffer::capacity_width() const noexcept {
    std::shared_lock lock(state_->access);
    return state_->capacity_width;
}
std::uint32_t ImageBuffer::capacity_height() const noexcept {
    std::shared_lock lock(state_->access);
    return state_->capacity_height;
}
std::size_t ImageBuffer::staging_capacity_bytes() const noexcept {
    std::shared_lock lock(state_->access);
    return state_->staging_bytes;
}
std::uint64_t ImageBuffer::revision() const noexcept {
    std::shared_lock lock(state_->access);
    return state_->revision;
}

struct ImageProductBuffer::State final {
    State(DeviceContext selected, const ImageProductLayout selected_layout)
        : context_(std::move(selected)),
          layout_(selected_layout),
          plane_count_(selected_layout == ImageProductLayout::CleanAndSemantic ? 2U : 1U) {
        planes_[0U] = std::make_unique<ImageBuffer>(context_);
        if (plane_count_ == 2U) planes_[1U] = std::make_unique<ImageBuffer>(context_);
        completion_ = context_.state_->backend->CreateEvent(context_.state_->context);
        if (completion_ == 0U) throw std::runtime_error("image product completion event creation returned no event");
    }
    ~State() {
        for (auto& plane : planes_)
            plane.reset();
        context_.state_->backend->DestroyEvent(context_.state_->context, completion_);
    }
    [[nodiscard]] std::uint64_t BeginWrite() {
        AwaitReceiverReads();
        if (unsettled_source_) throw std::runtime_error("image product retains an unsettled source");
        if (generation_sequence_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("image product generation exhausted");
        if (!ReservePhysicalWork()) throw std::runtime_error("image product workspace is acquired");
        if (workspace_) {
            workspace_->InvalidateWrite();
            if (workspace_reserved_ != workspace_) workspace_reserved_->InvalidateWrite();
        }
        generation_ = 0U;
        return generation_sequence_ + 1U;
    }
    void AwaitReceiverReads() const noexcept {
        auto readers = receiver_reads_.load(std::memory_order_acquire);
        while (readers != 0U) {
            receiver_reads_.wait(readers, std::memory_order_acquire);
            readers = receiver_reads_.load(std::memory_order_acquire);
        }
    }
    bool ReservePhysicalWork() {
        if (workspace_reserved_) return true;
        if (raw_workspace_ && planes_[0U]->state_->external_storage != raw_workspace_) raw_workspace_.reset();
        const auto first = raw_workspace_ ? raw_workspace_ : workspace_;
        if (!first) return true;
        if (!first->ReserveWrite()) return false;
        if (workspace_ && workspace_ != first && !workspace_->ReserveWrite()) {
            first->CancelWrite();
            return false;
        }
        workspace_reserved_ = first;
        return true;
    }
    [[nodiscard]] std::array<std::unique_lock<std::shared_mutex>, 2U> LockPlanes() {
        std::array<std::unique_lock<std::shared_mutex>, 2U> locks;
        for (std::size_t index = 0U; index != plane_count_; ++index) {
            locks[index] = std::unique_lock{planes_[index]->state_->access};
            if (planes_[index]->state_->unavailable.load(std::memory_order_acquire))
                throw std::runtime_error("image product storage is quarantined");
        }
        return locks;
    }
    DeviceContext context_;
    std::shared_ptr<ImageWorkspace> workspace_;
    std::shared_ptr<ImageWorkspace> workspace_reserved_;
    std::shared_ptr<ImageWorkspace> raw_workspace_;
    ImageWorkspaceFinalize finalize_;
    ImageProductLayout layout_;
    std::array<std::unique_ptr<ImageBuffer>, 2U> planes_{};
    std::size_t plane_count_;
    std::uintptr_t completion_ = 0U;
    mutable std::shared_mutex transaction_;
    std::atomic<std::uint32_t> receiver_reads_{0U};
    std::uint64_t generation_ = 0U;
    std::uint64_t generation_sequence_ = 0U;
    std::optional<BorrowedImageProductReadView> unsettled_source_;
    std::shared_ptr<const std::function<void()>> availability_sink_;
};

struct BorrowedImageProductReadView::Lease final {
    explicit Lease(std::shared_ptr<ImageProductBuffer::State> owner)
        : product(std::move(owner)), lock(product->transaction_), generation(product->generation_) {}
    Lease(std::shared_ptr<ImageProductBuffer::State> owner, std::shared_lock<std::shared_mutex> acquired)
        : product(std::move(owner)), lock(std::move(acquired)), generation(product->generation_) {}
    ~Lease() {
        if (completion_access || !lock.owns_lock()) return;
        const auto availability = product->availability_sink_;
        if (lock.owns_lock()) lock.unlock();
        product.reset();
        if (availability) {
            try {
                (*availability)();
            } catch (...) {}
        }
    }
    // CLEANUP-IGNORE: Product leases lock a transaction; plane leases lock individual buffer storage and retain
    // completion facts.
    std::shared_ptr<ImageProductBuffer::State> product;
    std::shared_lock<std::shared_mutex> lock;
    std::uint64_t generation = 0U;
    bool completion_access = false;
};

void ImageStream::Await(const BorrowedImageProductReadView& source) {
    if (!source.valid()) throw std::invalid_argument("cannot await an invalid image product");
    const auto& product = *source.lease_->product;
    if (context_.state_->backend != product.context_.state_->backend)
        throw std::invalid_argument("source and receiver use different image backends");
    context_.state_->backend->WaitEvent(context_.state_->context, stream_, product.completion_);
}

// CLEANUP-IGNORE: Product read special members retain a shared transaction lease; scalar reads own one plane lease.
BorrowedImageProductReadView::BorrowedImageProductReadView() noexcept = default;
BorrowedImageProductReadView::~BorrowedImageProductReadView() = default;
BorrowedImageProductReadView::BorrowedImageProductReadView(std::shared_ptr<Lease> lease) noexcept : lease_(std::move(lease)) {}
BorrowedImageProductReadView::BorrowedImageProductReadView(BorrowedImageProductReadView&&) noexcept = default;
BorrowedImageProductReadView& BorrowedImageProductReadView::operator=(BorrowedImageProductReadView&&) noexcept = default;
bool BorrowedImageProductReadView::valid() const noexcept {
    if (!lease_ || lease_->completion_access || lease_->generation == 0U || count_ == 0U) return false;
    for (std::size_t index = 0U; index != count_; ++index)
        if (!planes_[index].valid()) return false;
    return true;
}
std::size_t BorrowedImageProductReadView::plane_count() const noexcept { return count_; }
const BorrowedImageReadView& BorrowedImageProductReadView::plane(const std::size_t index) const {
    if (index >= count_) throw std::out_of_range("image product plane index is invalid");
    return planes_[index];
}
BorrowedImageReadView BorrowedImageProductReadView::TakePlane(const std::size_t index) && {
    if (index >= count_) throw std::out_of_range("image product plane index is invalid");
    return std::move(planes_[index]);
}

void BorrowedImageProductReadView::Quarantine() noexcept {
    for (std::size_t index = 0U; index != count_; ++index)
        planes_[index].Quarantine();
}

ImageProductReadCompletion::ImageProductReadCompletion(BorrowedImageProductReadView&& source) : source_(std::move(source)) {
    if (!source_.valid()) throw std::invalid_argument("receiver completion requires an intact product borrow");
    auto& lease = *source_.lease_;
    available_ = lease.product->availability_sink_;
    lease.product->receiver_reads_.fetch_add(1U, std::memory_order_acq_rel);
    lease.completion_access = true;
    pending_.store(true, std::memory_order_release);
    // These shared_mutex locks belong to this thread. The callback releases
    // only the counted access established before unlocking them.
    for (std::size_t index = 0U; index != source_.count_; ++index)
        source_.planes_[index].lease_->lock.unlock();
    lease.lock.unlock();
}
ImageProductReadCompletion::~ImageProductReadCompletion() {
    if (pending()) std::terminate();
}
bool ImageProductReadCompletion::pending() const noexcept { return pending_.load(std::memory_order_acquire); }
void ImageProductReadCompletion::Complete() noexcept {
    if (!pending_.exchange(false, std::memory_order_acq_rel)) return;
    ReleaseAccess();
}
void ImageProductReadCompletion::ReleaseAccess() noexcept {
    auto& lease = *source_.lease_;
    lease.product->receiver_reads_.fetch_sub(1U, std::memory_order_release);
    lease.product->receiver_reads_.notify_all();
    // Registered availability sinks are wake-only notifications. Keep their
    // retained function and every physical GPU owner alive on the caller.
    if (available_) {
        try {
            (*available_)();
        } catch (...) {}
    }
}
void ImageProductReadCompletion::Quarantine() noexcept {
    if (!pending_.exchange(false, std::memory_order_acq_rel)) return;
    source_.Quarantine();
    ReleaseAccess();
}

ImageProductBuffer::ImageProductBuffer(DeviceContext context, const ImageProductLayout layout)
    : state_(std::make_shared<State>(std::move(context), layout)) {}
ImageProductBuffer::~ImageProductBuffer() {
    if (deferred_release_) return;
    std::unique_lock transaction(state_->transaction_);
    state_->AwaitReceiverReads();
}
std::array<ImageAllocation, 2U> ImageProductBuffer::Allocations() const {
    std::shared_lock transaction(state_->transaction_);
    std::array<ImageAllocation, 2U> result{};
    for (std::size_t index = 0U; index != state_->plane_count_; ++index) {
        std::shared_lock access(state_->planes_[index]->state_->access);
        result[index] = state_->planes_[index]->state_->plane.allocation;
    }
    return result;
}
void ImageProductBuffer::Publish(ImageStream& stream, const std::uint32_t width, const std::uint32_t height, ProductSubmit submit) {
    std::uint64_t next_generation = 0U;
    {
        std::shared_lock transaction(state_->transaction_);
        if (state_->generation_sequence_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("image product generation exhausted");
        next_generation = state_->generation_sequence_ + 1U;
    }
    PublishAs(stream, width, height, next_generation, false, std::move(submit));
}
void ImageProductBuffer::PublishAs(ImageStream& stream, const std::uint32_t width, const std::uint32_t height, const std::uint64_t revision,
                                   const bool initialize, ProductSubmit submit) {
    if (width == 0U || height == 0U || !submit) throw std::invalid_argument("image product submit is empty");
    if (revision == 0U) throw std::invalid_argument("image product revision is invalid");
    if (stream.context_.state_ != state_->context_.state_)
        throw std::invalid_argument("image stream does not belong to the product context");
    std::unique_lock transaction(state_->transaction_);
    static_cast<void>(state_->BeginWrite());
    const auto plane_locks = state_->LockPlanes();
    if (state_->workspace_ && state_->workspace_->admitted() && state_->plane_count_ == 1U &&
        state_->workspace_->layout().device == state_->context_.device() && width <= state_->workspace_->layout().width &&
        height <= state_->workspace_->layout().height) {
        auto& raw = *state_->planes_[0U]->state_;
        if (raw.external_storage != state_->workspace_) {
            auto destination = state_->workspace_->plane(width, height);
            // A late alias cutover preserves the old authoritative plane before
            // releasing it. Already borrowed raw pointers cannot reach this lock.
            if (raw.plane.valid() && !initialize && raw.plane.descriptor.width == width && raw.plane.descriptor.height == height) {
                state_->context_.state_->backend->CopySameDevice(state_->context_.state_->context, stream.native_handle(), destination,
                                                                 state_->context_.state_->context, raw.plane);
                stream.Synchronize();
            }
            if (raw.plane.data != 0U && !raw.external_storage)
                state_->context_.state_->backend->FreePlane(state_->context_.state_->context, raw.plane.data);
            raw.external_storage = state_->workspace_;
            state_->raw_workspace_ = state_->workspace_;
            raw.external_bytes = state_->workspace_->allocation_bytes();
            destination.allocation.owner = raw.allocation_owner;
            raw.plane = destination;
            raw.capacity_width = state_->workspace_->layout().width;
            raw.capacity_height = state_->workspace_->layout().height;
        }
    }
    state_->planes_[0U]->state_->EnsurePlane(ImagePlaneKind::Clean, width, height);
    ImagePlaneView semantic;
    if (state_->plane_count_ == 2U) {
        state_->planes_[1U]->state_->EnsurePlane(ImagePlaneKind::Semantic, width, height);
        semantic = state_->planes_[1U]->state_->plane;
    }
    if (initialize) {
        for (std::size_t index = 0U; index != state_->plane_count_; ++index)
            state_->context_.state_->backend->ClearPlane(state_->context_.state_->context, stream.native_handle(),
                                                         state_->planes_[index]->state_->plane);
    }
    submit(state_->planes_[0U]->state_->plane, semantic, stream.native_handle());
    state_->context_.state_->backend->RecordEvent(state_->context_.state_->context, stream.native_handle(), state_->completion_);
    state_->generation_sequence_ = std::max(state_->generation_sequence_, revision);
    state_->generation_ = revision;
}
std::array<ImageCopyPath, 2U> ImageProductBuffer::CopyFrom(ImageStream& stream, BorrowedImageProductReadView source,
                                                           MissingPlaneSubmit initialize_missing, const bool preserve_clean) {
    return CopyFromAs(stream, std::move(source), std::move(initialize_missing), 0U, preserve_clean);
}
std::array<ImageCopyPath, 2U> ImageProductBuffer::CopyFromAs(ImageStream& stream, BorrowedImageProductReadView source,
                                                             MissingPlaneSubmit initialize_missing, const std::uint64_t revision,
                                                             const bool preserve_clean, const ImagePlanePreservation preservation) {
    if (!source.valid() || (source.plane_count() < state_->plane_count_ && !initialize_missing))
        throw std::invalid_argument("source image product lacks a receiver plane");
    if (source.lease_->product == state_) throw std::invalid_argument("an image product cannot copy from itself");
    if (stream.context_.state_ != state_->context_.state_)
        throw std::invalid_argument("image stream does not belong to the product context");
    if (state_->context_.state_->backend != source.lease_->product->context_.state_->backend)
        throw std::invalid_argument("source and receiver use different image backends");
    std::unique_lock transaction(state_->transaction_);
    if (preserve_clean) {
        const auto& retained = state_->planes_[0U]->state_->plane.descriptor;
        const auto& incoming = source.planes_[0U].plane().descriptor;
        if (state_->generation_ == 0U || retained.width != incoming.width || retained.height != incoming.height)
            throw std::invalid_argument("preserved clean input requires completed matching geometry");
    }
    std::uint64_t next_generation = revision;
    if (revision == 0U)
        next_generation = state_->BeginWrite();
    else
        static_cast<void>(state_->BeginWrite());
    std::array<ImageCopyPath, 2U> paths{};
    const auto plane_locks = state_->LockPlanes();
    auto& backend = *state_->context_.state_->backend;
    const auto& source_product = *source.lease_->product;
    const int receiver_device = state_->context_.device();
    const int source_device = source_product.context_.device();
    const bool staged = receiver_device != source_device && !backend.CanAccessPeer(receiver_device, source_device);
    if (staged)
        backend.SynchronizeEvent(source_product.context_.state_->context, source_product.completion_);
    else
        backend.WaitEvent(state_->context_.state_->context, stream.native_handle(), source_product.completion_);
    // Initialize receiver-only storage before enqueueing any borrowed-source
    // reads, so an initializer exception cannot release a source still in use.
    for (std::size_t index = source.plane_count(); index < state_->plane_count_; ++index) {
        auto& receiver = *state_->planes_[index]->state_;
        const auto& extent = source.planes_[0U].plane().descriptor;
        receiver.EnsurePlane(ImagePlaneKind::Semantic, extent.width, extent.height);
        backend.ClearPlane(state_->context_.state_->context, stream.native_handle(), receiver.plane);
        initialize_missing(receiver.plane, stream.native_handle());
    }
    bool reads_submitted = false;
    bool receiver_completed = false;
    try {
        const auto copied_planes =
            preservation == ImagePlanePreservation::Clean ? 1U : std::min(state_->plane_count_, source.plane_count());
        for (std::size_t index = preserve_clean ? 1U : 0U; index < copied_planes; ++index) {
            auto& receiver = *state_->planes_[index]->state_;
            const ImagePlaneView source_plane = source.planes_[index].plane();
            receiver.EnsurePlane(source_plane.descriptor.kind, source_plane.descriptor.width, source_plane.descriptor.height);
            if (receiver_device == source_device) {
                reads_submitted = true;
                backend.CopySameDevice(state_->context_.state_->context, stream.native_handle(), receiver.plane,
                                       source_product.context_.state_->context, source_plane);
                paths[index] = ImageCopyPath::SameDevice;
            } else if (!staged) {
                reads_submitted = true;
                backend.CopyPeer(state_->context_.state_->context, stream.native_handle(), receiver_device, receiver.plane,
                                 source_product.context_.state_->context, source_device, source_plane);
                paths[index] = ImageCopyPath::Peer;
            } else {
                const std::size_t row_bytes = source_plane.descriptor.row_bytes();
                if (source_plane.descriptor.height > std::numeric_limits<std::size_t>::max() / row_bytes)
                    throw std::overflow_error("staged image byte size exceeds addressable memory");
                receiver.EnsureStaging(row_bytes * source_plane.descriptor.height);
                LogStaging(state_->context_, source_device, row_bytes * source_plane.descriptor.height);
                reads_submitted = true;
                backend.CopyDeviceToHost(source_product.context_.state_->context, source_plane, receiver.staging, row_bytes);
                backend.CopyHostToDevice(state_->context_.state_->context, stream.native_handle(), receiver.staging, row_bytes,
                                         receiver.plane);
                paths[index] = ImageCopyPath::PinnedStaging;
            }
        }
        backend.RecordEvent(state_->context_.state_->context, stream.native_handle(), state_->completion_);
        auto settled = stream.Settle();
        receiver_completed = settled.completion_reached;
        if (!settled.completion_reached || settled.failure) ThrowExecutionFailure(settled);
        state_->generation_sequence_ = std::max(state_->generation_sequence_, next_generation);
        state_->generation_ = next_generation;
        return paths;
    } catch (...) {
        if (reads_submitted && !receiver_completed) {
            const auto settled = RetainUnsettledRead(stream, source, state_->unsettled_source_);
            receiver_completed = settled.completion_reached;
            if (!receiver_completed || settled.failure) {
                LogCopyFailure(state_->context_, "product", receiver_completed);
                ThrowExecutionFailure(settled, std::current_exception());
            }
        }
        LogCopyFailure(state_->context_, "product", !reads_submitted || receiver_completed);
        throw;
    }
}
BorrowedImageProductReadView ImageProductBuffer::Borrow() const {
    BorrowedImageProductReadView result{std::make_shared<BorrowedImageProductReadView::Lease>(state_)};
    result.count_ = state_->plane_count_;
    for (std::size_t index = 0U; index != state_->plane_count_; ++index)
        result.planes_[index] = BorrowedImageReadView{
            std::make_unique<BorrowedImageReadView::Lease>(state_->planes_[index]->state_, state_->completion_, result.lease_,
                                                           result.lease_->generation, &result.lease_->lock, state_->availability_sink_)};
    if (!result.valid()) return {};
    return result;
}
void ImageProductBuffer::AdoptExternalPlane(std::shared_ptr<void> custody, std::size_t bytes, ImagePlaneView plane) {
    std::unique_lock transaction(state_->transaction_);
    state_->AwaitReceiverReads();
    const auto locks = state_->LockPlanes();
    auto& raw = *state_->planes_[0U]->state_;
    if (state_->plane_count_ != 1U || raw.plane.valid() || !custody || !plane.valid())
        throw std::invalid_argument("external plane adoption requires empty clean storage");
    plane.allocation.owner = raw.allocation_owner;
    raw.external_storage = std::move(custody);
    raw.external_bytes = bytes;
    raw.plane = plane;
    raw.capacity_width = plane.allocation.width;
    raw.capacity_height = plane.allocation.height;
}
bool ImageProductBuffer::ConfigureWorkspace(std::shared_ptr<ImageWorkspace> workspace, ImageWorkspaceFinalize finalize) {
    if (!workspace || !workspace->admitted() || !finalize) throw std::invalid_argument("workspace configuration is incomplete");
    std::unique_lock transaction(state_->transaction_, std::try_to_lock);
    if (!transaction.owns_lock() || state_->receiver_reads_.load(std::memory_order_acquire) != 0U) return false;
    const bool reserved_replacement = workspace != state_->workspace_ && state_->workspace_reserved_;
    if (reserved_replacement) {
        if (!workspace->ReserveWrite()) return false;
    }
    try {
        workspace->Attach(state_->planes_[0U]->state_->allocation_owner);
    } catch (...) {
        if (reserved_replacement) workspace->CancelWrite();
        throw;
    }
    if (reserved_replacement && state_->workspace_ != state_->workspace_reserved_ && state_->workspace_) state_->workspace_->CancelWrite();
    // Retained raw plane custody is unchanged until the next exclusive write.
    if (state_->workspace_ && state_->workspace_ != workspace) state_->workspace_->Withdraw();
    state_->workspace_ = std::move(workspace);
    state_->workspace_->SetAvailabilitySink(state_->availability_sink_);
    state_->finalize_ = std::move(finalize);
    // Dropping the replaced owner can reveal failed physical cleanup even
    // though the new workspace and source execution are healthy.
    state_->workspace_->CheckOwner();
    return true;
}
void ImageProductBuffer::FinalizeWorkspace(ImageWorkspaceCoverage coverage) {
    {
        std::shared_lock transaction(state_->transaction_);
        if (!state_->workspace_ || state_->workspace_->revision() == state_->generation_) return;
    }
    auto source = Borrow();
    if (!source.valid() || !state_->workspace_ || state_->workspace_->revision() == source.plane(0U).revision()) return;
    const auto extent = source.plane(0U).plane().descriptor;
    if (extent.width > state_->workspace_->layout().width || extent.height > state_->workspace_->layout().height) return;
    state_->workspace_->Finalize(std::move(source), coverage, state_->finalize_);
    CancelWorkspaceWrite();
}
BorrowedImageWorkspace ImageProductBuffer::BorrowWorkspace() const {
    std::shared_lock transaction(state_->transaction_, std::try_to_lock);
    if (!transaction.owns_lock() || state_->generation_ == 0U || !state_->workspace_ ||
        state_->workspace_->revision() != state_->generation_)
        return {};
    BorrowedImageProductReadView source{std::make_shared<BorrowedImageProductReadView::Lease>(state_, std::move(transaction))};
    source.count_ = state_->plane_count_;
    for (std::size_t index = 0U; index != state_->plane_count_; ++index) {
        source.planes_[index] = BorrowedImageReadView{std::make_unique<BorrowedImageReadView::Lease>(
            state_->planes_[index]->state_, state_->completion_, source.lease_, source.lease_->generation, &source.lease_->lock,
            state_->availability_sink_, true)};
        if (!source.planes_[index].lease_->lock.owns_lock()) return {};
    }
    if (!source.valid()) return {};
    return BorrowedImageWorkspace(std::move(source), state_->workspace_);
}
ImageWorkspaceObservation ImageProductBuffer::ObserveWorkspace() const {
    std::shared_lock lock(state_->transaction_, std::try_to_lock);
    if (!lock.owns_lock() || state_->generation_ == 0U) return {};
    return {state_->planes_[0U]->state_->allocation_owner, state_->generation_, state_->workspace_};
}
ImageStreamSettlement ImageProductBuffer::SettleWorkspace() noexcept {
    return state_->workspace_ ? state_->workspace_->Settle() : ImageStreamSettlement{.completion_reached = true};
}
ImageProductLayout ImageProductBuffer::layout() const noexcept { return state_->layout_; }
std::uint32_t ImageProductBuffer::capacity_width() const noexcept { return state_->planes_[0U]->capacity_width(); }
std::uint32_t ImageProductBuffer::capacity_height() const noexcept { return state_->planes_[0U]->capacity_height(); }
std::size_t ImageProductBuffer::staging_capacity_bytes() const noexcept {
    std::size_t result = 0U;
    for (std::size_t index = 0U; index != state_->plane_count_; ++index)
        result = std::max(result, state_->planes_[index]->staging_capacity_bytes());
    return result;
}
ImageStorageFootprint ImageProductBuffer::StorageFootprint() const noexcept {
    ImageStorageFootprint result;
    for (std::size_t index = 0U; index != state_->plane_count_; ++index) {
        const auto& plane = *state_->planes_[index]->state_;
        std::shared_lock lock(plane.access);
        if (plane.external_storage) {
            if (plane.external_storage != state_->workspace_) result.device_bytes += plane.external_bytes;
        } else if (plane.plane.data != 0U)
            result.device_bytes += plane.plane.descriptor.pitch_bytes * plane.capacity_height;
        result.pinned_bytes += plane.staging_bytes;
    }
    if (state_->workspace_) {
        const auto workspace = state_->workspace_->StorageFootprint();
        result.device_bytes += workspace.device_bytes;
        result.pinned_bytes += workspace.pinned_bytes;
    }
    return result;
}
std::uint64_t ImageProductBuffer::revision() const noexcept {
    std::shared_lock transaction(state_->transaction_);
    return state_->generation_;
}
bool ImageProductBuffer::writable() const {
    if (terminal()) throw std::runtime_error("image product storage is quarantined");
    std::unique_lock transaction(state_->transaction_, std::try_to_lock);
    if (!transaction.owns_lock()) return false;
    if (state_->receiver_reads_.load(std::memory_order_acquire) != 0U) return false;
    if (state_->unsettled_source_) throw std::runtime_error("image product retains an unsettled source");
    if (state_->workspace_ && !state_->workspace_reserved_ && !state_->workspace_->WriteAvailable()) return false;
    if (state_->raw_workspace_ && !state_->workspace_reserved_ && !state_->raw_workspace_->WriteAvailable()) return false;
    std::array<std::unique_lock<std::shared_mutex>, 2U> locks;
    for (std::size_t index = 0U; index != state_->plane_count_; ++index) {
        locks[index] = std::unique_lock{state_->planes_[index]->state_->access, std::try_to_lock};
        if (!locks[index].owns_lock()) return false;
        if (state_->planes_[index]->state_->unavailable.load(std::memory_order_acquire))
            throw std::runtime_error("image product storage is quarantined");
    }
    return true;
}
bool ImageProductBuffer::ReserveWorkspaceWrite() {
    std::unique_lock transaction(state_->transaction_, std::try_to_lock);
    if (!transaction.owns_lock()) return false;
    if (state_->workspace_reserved_) return false;
    return state_->ReservePhysicalWork();
}
void ImageProductBuffer::CancelWorkspaceWrite() noexcept {
    std::unique_lock transaction(state_->transaction_);
    if (!state_->workspace_reserved_) return;
    state_->workspace_reserved_->CancelWrite();
    if (state_->workspace_ != state_->workspace_reserved_ && state_->workspace_) state_->workspace_->CancelWrite();
    state_->workspace_reserved_.reset();
}
bool ImageProductBuffer::terminal() const noexcept {
    for (std::size_t index = 0U; index != state_->plane_count_; ++index)
        if (state_->planes_[index]->state_->unavailable.load(std::memory_order_acquire)) return true;
    return false;
}
bool ImageProductBuffer::Owns(const BorrowedImageProductReadView& source) const noexcept {
    return source.lease_ && source.lease_->product == state_;
}
void ImageProductBuffer::SetAvailabilitySink(std::shared_ptr<const std::function<void()>> sink) {
    std::unique_lock transaction(state_->transaction_);
    state_->availability_sink_ = std::move(sink);
    if (state_->workspace_) state_->workspace_->SetAvailabilitySink(state_->availability_sink_);
    if (state_->raw_workspace_ && state_->raw_workspace_ != state_->workspace_)
        state_->raw_workspace_->SetAvailabilitySink(state_->availability_sink_);
}
}  // namespace mmltk::frameworks::gpu
