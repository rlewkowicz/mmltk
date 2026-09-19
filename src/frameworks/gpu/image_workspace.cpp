#include "src/frameworks/gpu/image_workspace.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "src/frameworks/gpu/imported_image_buffer.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/image_product_retirement.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "src/common/io/noexcept_io.h"
namespace mmltk::frameworks::gpu {
namespace {
void merge_workspace_damage(ImageWorkspaceRegion& result, ImageWorkspaceRegion region) noexcept {
    if (region.x1 >= region.x2 || region.y1 >= region.y2) return;
    if (result.x1 >= result.x2 || result.y1 >= result.y2) {
        result = region;
        return;
    }
    result.x1 = std::min(result.x1, region.x1);
    result.y1 = std::min(result.y1, region.y1);
    result.x2 = std::max(result.x2, region.x2);
    result.y2 = std::max(result.y2, region.y2);
}
}  // namespace
void ImageWorkspaceDamage::Record(ImageWorkspaceContent current, ImageWorkspaceCoverage coverage) noexcept {
    if (!current.valid() || current == newest_) return;
    auto& change = changes_[next_];
    change = {.before = coverage.baseline, .after = current, .full = coverage.full_image};
    for (const auto region : coverage.regions) merge_workspace_damage(change.bounds, region);
    next_ = (next_ + 1U) % changes_.size();
    count_ = std::min(count_ + 1U, changes_.size());
    newest_ = current;
}
ImageWorkspaceCoverage ImageWorkspaceDamage::Since(ImageWorkspaceContent baseline, ImageWorkspaceContent current, std::uint64_t allocation) noexcept {
    accumulated_ = {};
    if (!baseline.valid() || baseline.owner != current.owner) return {};
    auto cursor = baseline;
    for (std::size_t index = 0U; index < count_ && cursor != current; ++index) {
        const auto& change = changes_[(next_ + changes_.size() - count_ + index) % changes_.size()];
        if (change.before != cursor) continue;
        if (change.full) return {};
        merge_workspace_damage(accumulated_, change.bounds);
        cursor = change.after;
    }
    if (cursor != current) return {};
    return {.allocation_identity = allocation, .regions = {&accumulated_, 1U}, .full_image = false, .baseline = baseline};
}
bool ImageWorkspaceLayout::valid() const noexcept {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    return format == ImageFormat::Rgba8 && device >= 0 && device_incarnation != 0U && width != 0U && height != 0U &&
           width <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) &&
           height <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) &&
           std::ranges::any_of(device_uuid, [](auto byte) { return byte != 0U; }) && pitch_bytes >= static_cast<std::size_t>(width) * 4U &&
           alignment_bytes != 0U && (alignment_bytes & (alignment_bytes - 1U)) == 0U && height <= (maximum - offset_bytes) / pitch_bytes &&
           required_allocation_bytes >= offset_bytes + pitch_bytes * height &&
           required_allocation_bytes <= static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
}
namespace {
[[nodiscard]] std::exception_ptr workspace_release_failure(const char* message) noexcept {
    try {
        throw ImageStreamExecutionFailure(std::make_exception_ptr(std::runtime_error(message)));
    } catch (...) { return std::current_exception(); }
}
void initialize_workspace_allocation(ImportedImageBuffer& allocation, DeviceContext context, const ImageWorkspaceLayout& layout,
                                     mmltk::common::io::ScopedFd memory, std::uint64_t identity) {
    std::string error;
    if (!allocation.Import(std::move(context), std::move(memory), layout, identity, &error)) throw std::runtime_error(error);
}
}  // namespace
void ImageWorkspace::Owner::Check() const {
    std::scoped_lock lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
    if (closed_) throw std::runtime_error("workspace owner is retired");
}
void ImageWorkspace::Owner::Failed(std::exception_ptr failure) noexcept {
    {
        std::scoped_lock lock(mutex_);
        closed_ = true;
        failure_ = combine_image_failures(failure_, std::move(failure));
    }
}
std::exception_ptr ImageWorkspace::Owner::failure() const noexcept {
    std::scoped_lock lock(mutex_);
    return failure_;
}
void ImageWorkspace::Owner::Released(ImageStreamSettlement result) noexcept {
    std::shared_ptr<ImageProductRetirement> producer;
    {
        std::scoped_lock lock(mutex_);
        release_result_ = result;
        release_complete_ = true;
        producer = std::move(responsible_producer_);
    }
    // This is the accepted allocation's direct settlement bookkeeping. Its
    // notification only wakes the producer; no product operation runs here.
    if (producer) producer->Released(std::move(result));
    Wake();
}
ImageWorkspace::Retirement::Result ImageWorkspace::Owner::TakeResult() noexcept {
    std::scoped_lock lock(mutex_);
    if (!release_complete_) return {};
    return {.complete = true, .claimed = !std::exchange(release_claimed_, true), .settlement = release_result_};
}
bool ImageWorkspace::Owner::TransferToProducer() noexcept {
    std::scoped_lock lock(mutex_);
    if (release_complete_) return false;
    if (responsible_producer_) return true;
    if (release_claimed_ || !attached_producer_) return false;
    attached_producer_->Acquire();
    responsible_producer_ = attached_producer_;
    release_claimed_ = true;
    return true;
}
void ImageWorkspace::Owner::SetWake(std::shared_ptr<const std::function<void()>> wake) noexcept {
    {
        std::scoped_lock lock(mutex_);
        wake_ = std::move(wake);
    }
    Wake();
}
void ImageWorkspace::Owner::Wake() const noexcept {
    std::shared_ptr<const std::function<void()>> wake;
    {
        std::scoped_lock lock(mutex_);
        if (release_complete_ || product_owner.load(std::memory_order_acquire) != 0U) wake = wake_;
    }
    if (wake) {
        try {
            (*wake)();
        } catch (...) {}
    }
}
ImageWorkspace::Retirement::Result ImageWorkspace::Retirement::TakeResult() const noexcept { return owner_ ? owner_->TakeResult() : Result{.complete = true}; }
bool ImageWorkspace::Retirement::TransferToProducer() const noexcept { return owner_ && owner_->TransferToProducer(); }
void ImageWorkspace::Retirement::SetWake(std::shared_ptr<const std::function<void()>> wake) const noexcept {
    if (owner_) owner_->SetWake(std::move(wake));
}
struct ImageWorkspace::State final {
    State(std::shared_ptr<Owner> lifetime, ImageWorkspaceLayout requested, const Operations* injected)
        : owner(std::move(lifetime)),
          layout(std::move(requested)),
          operations(injected ? *injected
                              : Operations{&initialize_workspace_allocation, [](ImportedImageBuffer& buffer) noexcept { return buffer.Release(); },
                                           [](const ImportedImageBuffer& buffer, DeviceContext import_context) {
                                               return buffer.ImportAlias(std::move(import_context));
                                           }}) {}
    void Initialize(DeviceContext source, std::optional<DeviceExecution> execution) {
        identity = next_image_allocation_identity();
        if (!layout.valid()) throw std::invalid_argument("workspace layout is invalid");
        access_descriptor.reset(::memfd_create("mmltk-workspace-access", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        if (access_descriptor.get() < 0 || ::ftruncate(access_descriptor.get(), sizeof(ImageWorkspaceAccessSignal)) != 0)
            throw std::runtime_error("workspace access allocation failed");
        const auto mapping = ::mmap(nullptr, sizeof(ImageWorkspaceAccessSignal), PROT_READ | PROT_WRITE, MAP_SHARED, access_descriptor.get(), 0);
        if (mapping == MAP_FAILED) throw std::runtime_error("workspace access mapping failed");
        access_signal = static_cast<ImageWorkspaceAccessSignal*>(mapping);
        *access_signal = {.allocation_identity = identity};
        if (::fcntl(access_descriptor.get(), F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0)
            throw std::runtime_error("workspace access sealing failed");
        context.emplace(source.OnDevice(layout.device, std::move(execution)));
        stream.emplace(*context);
        context->Bind();
        completion = context->CreateEvent();
    }
    ~State() {
        if (context) context->DestroyEvent(completion);
        if (producer_context) producer_context->DestroyEvent(producer_completion);
        if (access_signal) static_cast<void>(::munmap(access_signal, sizeof(ImageWorkspaceAccessSignal)));
    }
    bool ReservePhysicalWrite() {
        auto gate = std::atomic_ref{access_signal->access};
        auto expected = gate.load(std::memory_order_acquire);
        const auto role = expected & kWorkspaceAccessMask;
        if (role != kWorkspaceAccessEmpty && role != kWorkspaceAccessAvailable) return false;
        const auto epoch = expected & ~kWorkspaceAccessMask;
        if ((epoch & ~kWorkspaceAccessRevoked) > kWorkspaceAccessRevoked - 2U * (kWorkspaceAccessMask + 1U))
            throw std::overflow_error("workspace physical access epoch exhausted");
        if (!gate.compare_exchange_strong(expected, epoch + kWorkspaceAccessMask + 1U + kWorkspaceAccessWriting, std::memory_order_acq_rel)) return false;
        std::atomic_ref{access_signal->terminal_read_complete}.store(0U, std::memory_order_release);
        write_reserved = true;
        write_invalidated = false;
        return true;
    }
    void PublishAccess(std::uint64_t role) noexcept {
        if (role == kWorkspaceAccessAvailable && withdrawn.load(std::memory_order_acquire)) role = kWorkspaceAccessEmpty;
        auto gate = std::atomic_ref{access_signal->access};
        const auto epoch = gate.load(std::memory_order_relaxed) & ~kWorkspaceAccessMask;
        gate.store(epoch | role, std::memory_order_release);
    }
    static void InvokeWake(const std::atomic<std::shared_ptr<const std::function<void()>>>& selected) noexcept {
        if (const auto sink = selected.load(std::memory_order_acquire)) {
            try {
                (*sink)();
            } catch (...) {}
        }
    }
    void NotifyDisplayAvailable() const noexcept {
        if (display_held.load(std::memory_order_acquire)) return;
        const auto role = std::atomic_ref{access_signal->access}.load(std::memory_order_acquire) & kWorkspaceAccessMask;
        if (role != kWorkspaceAccessEmpty && role != kWorkspaceAccessAvailable) return;
        InvokeWake(display_availability_sink);
    }
    void Withdraw() noexcept {
        std::scoped_lock lock(access);
        auto gate = std::atomic_ref{access_signal->access};
        auto expected = gate.fetch_or(kWorkspaceAccessRevoked, std::memory_order_acq_rel) | kWorkspaceAccessRevoked;
        if ((expected & kWorkspaceAccessMask) == kWorkspaceAccessAvailable)
            // A winning external reader retains custody through settlement.
            static_cast<void>(gate.compare_exchange_strong(expected, (expected & ~kWorkspaceAccessMask) | kWorkspaceAccessEmpty, std::memory_order_acq_rel));
        pending_memory.reset();
        withdrawn.store(true, std::memory_order_release);
    }
    std::shared_ptr<Owner> owner;
    std::optional<DeviceContext> context;
    const ImageWorkspaceLayout layout;
    const Operations operations;
    std::optional<ImageStream> stream;
    std::shared_ptr<ImportedImageBuffer> allocation = std::make_shared<ImportedImageBuffer>();
    std::optional<DeviceContext> producer_context;
    std::shared_ptr<ImportedImageBuffer> producer_allocation;
    std::optional<ImageStream> producer_stream;
    std::uintptr_t producer_completion = 0U;
    std::array<std::unique_ptr<ImageProductBuffer>, 2U> transfers;
    std::optional<BorrowedImageProductReadView> unsettled_source;
    std::unique_ptr<ImageProductReadCompletion> pending_source;
    ImageStream* pending_execution = nullptr;
    ImageWorkspaceContent pending_content{};
    std::uint32_t pending_width = 0U, pending_height = 0U;
    std::atomic_bool completion_notified{false};
    void FinishPending(const ImageStreamSettlement& settled, bool publish) noexcept {
        if (!pending_source) return;
        if (!settled.completion_reached) {
            pending_source->Quarantine();
            owner->Failed(settled.failure ? settled.failure : workspace_release_failure("workspace completion was not established"));
            return;
        }
        if (publish && !settled.failure) {
            width = pending_width;
            height = pending_height;
            content = pending_content;
            content_valid = true;
            revision = content.revision;
            std::atomic_ref{access_signal->generation}.store(content.revision, std::memory_order_relaxed);
        } else {
            content = {};
            content_valid = false;
            revision = 0U;
            width = height = 0U;
        }
        write_invalidated = false;
        write_reserved = false;
        PublishAccess(content_valid ? kWorkspaceAccessAvailable : kWorkspaceAccessEmpty);
        pending_execution = nullptr;
        pending_source->Complete();
        pending_source.reset();
    }
    std::uintptr_t completion = 0U;
    std::uintptr_t completed_event = 0U;
    std::uint64_t identity = 0U;
    mmltk::common::io::ScopedFd access_descriptor;
    mmltk::common::io::ScopedFd pending_memory;
    ImageWorkspaceAccessSignal* access_signal = nullptr;
    std::atomic_bool display_held{false};
    bool write_reserved = false;
    bool write_invalidated = false;
    std::atomic<std::shared_ptr<const std::function<void()>>> availability_sink;
    std::atomic<std::shared_ptr<const std::function<void()>>> display_availability_sink;
    std::atomic<std::uint64_t> revision{0U};
    ImageWorkspaceContent content{};
    bool content_valid = false;
    mutable std::mutex access;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::atomic_bool admitted{false};
    std::atomic_bool withdrawn{false};
    TerminalCudaRetirementOwner terminal{1U};
    TerminalCudaRetirementLease retention = ReserveTerminalCudaLease(terminal);
};
ImageWorkspace::ImageWorkspace(DeviceContext source, ImageWorkspaceLayout layout, std::optional<DeviceExecution> execution, const Operations* operations)
    : state_(std::make_shared<State>(std::make_shared<Owner>(), std::move(layout), operations)) {
    try {
        state_->Initialize(std::move(source), std::move(execution));
    } catch (...) { std::rethrow_exception(Release(std::current_exception())); }
}
ImageWorkspace::~ImageWorkspace() noexcept { static_cast<void>(Release()); }
std::shared_ptr<ImageWorkspace> ImageWorkspace::Create(DeviceContext context, ImageWorkspaceLayout layout, std::optional<DeviceExecution> execution) {
    return Create(std::move(context), std::move(layout), std::move(execution), nullptr);
}
std::shared_ptr<ImageWorkspace> ImageWorkspace::Create(DeviceContext context, ImageWorkspaceLayout layout, std::optional<DeviceExecution> execution,
                                                       const Operations* operations) {
    auto result = std::shared_ptr<ImageWorkspace>(new ImageWorkspace(std::move(context), std::move(layout), std::move(execution), operations));
    return result;
}
std::exception_ptr ImageWorkspace::Release(std::exception_ptr initiating) noexcept {
    if (!state_) return initiating;
    auto settled = Settle();
    auto failure = combine_image_failures(initiating, settled.failure);
    bool safe = settled.completion_reached;
    if (safe) {
        try {
            if (state_->context) state_->context->Bind();
            for (auto& transfer : state_->transfers) transfer.reset();
            if (state_->producer_allocation) {
                const auto released = state_->operations.release(*state_->producer_allocation);
                if (released != cudaSuccess || state_->producer_allocation->release_failure() != cudaSuccess)
                    throw std::runtime_error("workspace producer mapping release failed");
                state_->producer_allocation.reset();
            }
            state_->producer_stream.reset();
            if (state_->producer_context) state_->producer_context->DestroyEvent(std::exchange(state_->producer_completion, 0U));
            const auto released = state_->allocation.use_count() == 1U ? state_->operations.release(*state_->allocation) : cudaSuccess;
            safe = released == cudaSuccess && state_->allocation->release_failure() == cudaSuccess;
            if (!safe) failure = combine_image_failures(failure, workspace_release_failure("workspace allocation release failed"));
        } catch (...) {
            safe = false;
            failure = combine_image_failures(failure, std::current_exception());
        }
    }
    auto owner = state_->owner;
    if (!safe) {
        if (!failure) failure = workspace_release_failure("workspace completion was not established");
        // Retain display context, producer mappings, streams, transfers and reads.
        owner->Failed(failure);
        auto retention = std::move(state_->retention);
        std::move(retention).Install(TerminalCudaCustody::Share(std::move(state_)), cudaErrorUnknown);
    } else {
        state_.reset();
    }
    owner->Released({.completion_reached = safe, .failure = failure});
    return failure;
}
void ImageWorkspace::CheckOwner() const { state_->owner->Check(); }
ImageWorkspace::Retirement ImageWorkspace::ObserveRetirement() const noexcept { return Retirement{state_->owner}; }
const ImageWorkspaceLayout& ImageWorkspace::layout() const noexcept { return state_->layout; }
std::uint64_t ImageWorkspace::identity() const noexcept { return state_->identity; }
std::size_t ImageWorkspace::allocation_bytes() const noexcept { return layout().required_allocation_bytes; }
ImageStorageFootprint ImageWorkspace::StorageFootprint() const noexcept {
    std::scoped_lock lock(state_->access);
    ImageStorageFootprint result;
    for (const auto& transfer : state_->transfers) {
        if (!transfer) continue;
        const auto storage = transfer->StorageFootprint();
        result.device_bytes += storage.device_bytes;
        result.pinned_bytes += storage.pinned_bytes;
    }
    if (!state_->transfers[0U]) result.device_bytes += state_->allocation->allocation_size();
    return result;
}
void ImageWorkspace::Attach(std::uint64_t product_owner, std::shared_ptr<ImageProductRetirement> producer) {
    CheckOwner();
    bool changed;
    {
        std::scoped_lock lock(state_->access);
        std::scoped_lock owner_lock(state_->owner->mutex_);
        if (product_owner == 0U || (state_->owner->product_owner != 0U && state_->owner->product_owner != product_owner))
            throw std::invalid_argument("workspace allocation already belongs to another product slot");
        changed = state_->owner->product_owner.exchange(product_owner, std::memory_order_acq_rel) != product_owner;
        state_->owner->attached_producer_ = std::move(producer);
    }
    if (changed) state_->owner->Wake();
}
bool ImageWorkspace::admitted() const noexcept { return state_->admitted.load(std::memory_order_acquire); }
bool ImageWorkspace::retired() const noexcept {
    if (state_->withdrawn.load(std::memory_order_acquire)) return true;
    {
        std::scoped_lock lock(state_->owner->mutex_);
        if (!state_->owner->closed_) return false;
    }
    // Failure retirement resolves the same physical race as replacement.
    state_->Withdraw();
    return true;
}
void ImageWorkspace::Withdraw() noexcept { state_->Withdraw(); }
std::uint64_t ImageWorkspace::revision() const noexcept { return state_->revision.load(std::memory_order_acquire); }
bool ImageWorkspace::Contains(ImageWorkspaceContent content) const noexcept {
    std::scoped_lock lock(state_->access);
    return content.valid() && state_->content_valid && state_->content == content;
}
ImageWorkspaceContent ImageWorkspace::Content() const noexcept {
    std::scoped_lock lock(state_->access);
    return state_->content;
}
ImagePlaneView ImageWorkspace::plane(std::uint32_t width, std::uint32_t height) const {
    if (!admitted() || state_->allocation->empty() || width == 0U || height == 0U || width > layout().width || height > layout().height)
        throw std::invalid_argument("workspace logical extent exceeds capacity");
    return {state_->allocation->data(),
            {ImagePlaneKind::Clean, ImageFormat::Rgba8, width, height, layout().pitch_bytes},
            {identity(), layout().width, layout().height, identity()}};
}
mmltk::common::io::ScopedFd ImageWorkspace::ExportAccessDescriptor() const {
    mmltk::common::io::ScopedFd descriptor(::fcntl(state_->access_descriptor.get(), F_DUPFD_CLOEXEC, 0));
    if (descriptor.get() < 0) throw std::runtime_error("workspace access descriptor duplication failed");
    return descriptor;
}
ImagePlaneView ImageWorkspace::ProducerPlane(const DeviceContext& context, std::uint32_t width, std::uint32_t height) {
    std::scoped_lock lock(state_->access);
    return ProducerPlaneLocked(context, width, height);
}
ImagePlaneView ImageWorkspace::ProducerPlaneLocked(const DeviceContext& context, std::uint32_t width, std::uint32_t height) {
    auto result = plane(width, height);
    if (context == *state_->context) return result;
    if (context.device() != layout().device) throw std::invalid_argument("workspace producer belongs to another device");
    if (!state_->producer_context || *state_->producer_context != context || !state_->producer_stream || state_->producer_completion == 0U) {
        std::exception_ptr completed_failure;
        if (state_->producer_allocation) {
            const auto settled = state_->producer_stream ? state_->producer_stream->Settle() : ImageStreamSettlement{.completion_reached = true};
            if (!settled.completion_reached) {
                const auto failure = combine_image_failures(workspace_release_failure("workspace producer mapping is unsettled"), settled.failure);
                state_->owner->Failed(failure);
                std::rethrow_exception(failure);
            }
            completed_failure = settled.failure;
            const auto released = state_->operations.release(*state_->producer_allocation);
            if (released != cudaSuccess || state_->producer_allocation->release_failure() != cudaSuccess) {
                const auto failure = combine_image_failures(completed_failure, workspace_release_failure("workspace producer mapping release failed"));
                state_->owner->Failed(failure);
                std::rethrow_exception(failure);
            }
            // CLEANUP-IGNORE: Producer rebinding and final detach release under different settlement/failure policies and context lifetimes.
            state_->producer_allocation.reset();
        }
        state_->producer_stream.reset();
        if (state_->producer_context) state_->producer_context->DestroyEvent(std::exchange(state_->producer_completion, 0U));
        state_->producer_context.reset();
        if (completed_failure) std::rethrow_exception(completed_failure);
        state_->producer_allocation = state_->operations.alias(*state_->allocation, context);
        state_->producer_context = context;
        state_->producer_stream.emplace(context);
        state_->producer_completion = context.CreateEvent();
    }
    result.data = state_->producer_allocation->data();
    return result;
}
bool ImageWorkspace::WriteAvailable() const noexcept {
    if (!admitted() || state_->display_held.load(std::memory_order_acquire)) return false;
    const auto access = std::atomic_ref{state_->access_signal->access}.load(std::memory_order_acquire);
    return (access & kWorkspaceAccessMask) == kWorkspaceAccessEmpty || (access & kWorkspaceAccessMask) == kWorkspaceAccessAvailable;
}
bool ImageWorkspace::FinalizationPending() const noexcept {
    std::scoped_lock lock(state_->access);
    return bool(state_->pending_source);
}
ImageWorkspace::AccessObservation ImageWorkspace::ObserveAccess() const noexcept {
    std::scoped_lock lock(state_->access);
    return {
        .access = std::atomic_ref{state_->access_signal->access}.load(std::memory_order_acquire),
        .generation = std::atomic_ref{state_->access_signal->generation}.load(std::memory_order_relaxed),
        .display_held = state_->display_held.load(std::memory_order_acquire),
        .write_reserved = state_->write_reserved,
        .completion_pending = bool(state_->pending_source),
    };
}
bool ImageWorkspace::ReserveWrite() {
    if (!admitted()) return false;
    std::unique_lock lock(state_->access, std::try_to_lock);
    if (!lock.owns_lock() || state_->display_held.load(std::memory_order_acquire)) return false;
    return state_->ReservePhysicalWrite();
}
void ImageWorkspace::InvalidateWrite() noexcept {
    std::scoped_lock lock(state_->access);
    if (!state_->write_reserved) return;
    state_->write_invalidated = true;
    state_->content_valid = false;
    state_->revision = 0U;
    std::atomic_ref{state_->access_signal->generation}.store(0U, std::memory_order_relaxed);
}
void ImageWorkspace::CancelWrite() noexcept {
    {
        std::scoped_lock lock(state_->access);
        if (!state_->write_reserved || state_->pending_source) return;
        if (state_->write_invalidated) {
            state_->revision = 0U;
            state_->content = {};
            state_->content_valid = false;
            state_->width = state_->height = 0U;
        }
        state_->write_reserved = false;
        state_->PublishAccess(state_->revision != 0U ? kWorkspaceAccessAvailable : kWorkspaceAccessEmpty);
    }
    State::InvokeWake(state_->availability_sink);
    state_->NotifyDisplayAvailable();
}
bool ImageWorkspace::ReserveDisplayWrite() {
    std::scoped_lock lock(state_->access);
    if (!admitted() || state_->display_held.load(std::memory_order_acquire)) return false;
    if (!state_->ReservePhysicalWrite()) return false;
    state_->display_held.store(true, std::memory_order_release);
    return true;
}
void ImageWorkspace::CancelDisplayWrite() noexcept {
    CancelWrite();
    state_->display_held.store(false, std::memory_order_release);
    State::InvokeWake(state_->availability_sink);
    state_->NotifyDisplayAvailable();
}
std::uint64_t ImageWorkspace::product_owner() const noexcept { return state_->owner->product_owner.load(std::memory_order_acquire); }
void ImageWorkspace::SetDisplayAvailabilitySink(std::shared_ptr<const std::function<void()>> sink) noexcept {
    state_->display_availability_sink.store(std::move(sink), std::memory_order_release);
}
void ImageWorkspace::Detach(std::uint64_t product_owner) {
    {
        std::scoped_lock lock(state_->access);
        std::scoped_lock owner_lock(state_->owner->mutex_);
        if (state_->owner->product_owner == product_owner) {
            state_->owner->product_owner = 0U;
            state_->owner->attached_producer_.reset();
        } else if (state_->owner->product_owner != 0U)
            throw std::invalid_argument("workspace detachment owner mismatch");
    }
    State::InvokeWake(state_->display_availability_sink);
}
bool ImageWorkspace::Acquired(std::uint64_t generation) const noexcept {
    return generation != 0U &&
           (std::atomic_ref{state_->access_signal->access}.load(std::memory_order_acquire) & kWorkspaceAccessMask) == kWorkspaceAccessReading &&
           std::atomic_ref{state_->access_signal->generation}.load(std::memory_order_relaxed) == generation;
}
bool ImageWorkspace::TerminalReadComplete(std::uint64_t generation) const noexcept {
    const auto complete = std::atomic_ref{state_->access_signal->terminal_read_complete}.load(std::memory_order_acquire);
    return (complete & kWorkspaceAccessRevoked) != 0U && Acquired(generation) &&
           complete == std::atomic_ref{state_->access_signal->access}.load(std::memory_order_acquire);
}
void ImageWorkspace::CompleteRead(std::uint64_t generation) {
    {
        std::scoped_lock lock(state_->access);
        if (!Acquired(generation)) throw std::runtime_error("workspace read settlement generation mismatch");
        auto access = std::atomic_ref{state_->access_signal->access};
        auto expected = access.load(std::memory_order_acquire);
        const auto role = (expected & kWorkspaceAccessRevoked) != 0U ? kWorkspaceAccessEmpty : kWorkspaceAccessAvailable;
        if ((expected & kWorkspaceAccessMask) != kWorkspaceAccessReading ||
            !access.compare_exchange_strong(expected, (expected & ~kWorkspaceAccessMask) | role, std::memory_order_release))
            throw std::runtime_error("workspace read settlement lost physical custody");
        state_->display_held.store(false, std::memory_order_release);
    }
    State::InvokeWake(state_->availability_sink);
}
void ImageWorkspace::SetAvailabilitySink(std::shared_ptr<const std::function<void()>> sink) noexcept {
    state_->availability_sink.store(std::move(sink), std::memory_order_release);
}
bool ImageWorkspace::QueueAllocation(mmltk::common::io::ScopedFd memory) {
    std::scoped_lock lock(state_->access);
    std::scoped_lock owner_lock(state_->owner->mutex_);
    if (state_->owner->closed_ || state_->withdrawn.load(std::memory_order_acquire)) return false;
    if (memory.get() < 0 || state_->pending_memory.get() >= 0 || admitted())
        throw std::invalid_argument("workspace allocation descriptor is unavailable or duplicated");
    state_->pending_memory = std::move(memory);
    return true;
}
void ImageWorkspace::Admit(std::uint64_t allocation_identity, std::uint64_t device_incarnation) {
    std::unique_lock lock(state_->access);
    std::unique_lock owner_lock(state_->owner->mutex_);
    if (state_->owner->failure_) std::rethrow_exception(state_->owner->failure_);
    if (state_->owner->closed_ || state_->withdrawn.load(std::memory_order_acquire)) throw std::runtime_error("workspace owner is retired");
    if (allocation_identity != identity() || device_incarnation != layout().device_incarnation || state_->admitted)
        throw std::invalid_argument("workspace admission identity mismatch or duplicate");
    try {
        state_->context->Bind();
        state_->operations.initialize(*state_->allocation, *state_->context, layout(), std::move(state_->pending_memory), identity());
        state_->admitted = true;
    } catch (...) {
        auto failure = std::current_exception();
        const auto released = state_->operations.release(*state_->allocation);
        state_->withdrawn.store(true, std::memory_order_release);
        owner_lock.unlock();
        lock.unlock();
        if (released != cudaSuccess || state_->allocation->release_failure() != cudaSuccess) {
            failure = combine_image_failures(failure, workspace_release_failure("workspace import cleanup failed"));
            state_->owner->Failed(failure);
        }
        std::rethrow_exception(failure);
    }
}
ImageStreamSettlement ImageWorkspace::Settle() noexcept {
    if (!state_) return {.completion_reached = true};
    std::unique_lock lock(state_->access);
    const bool pending = bool(state_->pending_source);
    auto settled = state_->stream ? state_->stream->Settle() : ImageStreamSettlement{.completion_reached = true};
    if (state_->producer_stream) {
        const auto producer = state_->producer_stream->Settle();
        settled.completion_reached = settled.completion_reached && producer.completion_reached;
        settled.failure = combine_image_failures(settled.failure, producer.failure);
    }
    state_->FinishPending(settled, true);
    if (const auto failure = state_->owner->failure()) {
        settled.completion_reached = false;
        settled.failure = combine_image_failures(settled.failure, failure);
    }
    settled.completion_reached = settled.completion_reached && !state_->unsettled_source;
    if (!settled.completion_reached) {
        if (!settled.failure) settled.failure = workspace_release_failure("workspace completion was not established");
        state_->owner->Failed(settled.failure);
    }
    const bool available = pending && !state_->pending_source;
    lock.unlock();
    if (available) state_->NotifyDisplayAvailable();
    return settled;
}
void ImageWorkspace::Complete() {
    CheckOwner();
    std::unique_lock lock(state_->access);
    if (!state_->pending_source || !state_->completion_notified.load(std::memory_order_acquire)) return;
    // The notification executes before CUDA returns from the host callback.
    // Settlement on the execution owner proves that physical return as well.
    const auto settled = state_->pending_execution->Settle();
    state_->FinishPending(settled, true);
    const bool available = !state_->pending_source;
    lock.unlock();
    // CUDA's callback wakes the producer to settle its work. Presentation
    // independently waits for the resulting writable, completed workspace.
    if (available) state_->NotifyDisplayAvailable();
    if (settled.failure) std::rethrow_exception(settled.failure);
    if (!settled.completion_reached) CheckOwner();
}
void ImageWorkspace::Finalize(BorrowedImageProductReadView source, ImageWorkspaceCoverage coverage, const ImageWorkspaceFinalize& finalize) {
    CheckOwner();
    std::unique_lock lock(state_->access);
    if (state_->pending_source) return;
    if (!admitted() || !source.valid() || state_->unsettled_source) throw std::invalid_argument("workspace source is unavailable");
    const auto clean = source.plane(0U).plane();
    const ImageWorkspaceContent content{clean.allocation.owner, source.plane(0U).revision()};
    const bool expands_initialized = clean.descriptor.width > state_->width || clean.descriptor.height > state_->height;
    if (!coverage.baseline.valid() || coverage.baseline != state_->content || coverage.baseline.owner != content.owner || expands_initialized ||
        coverage.allocation_identity != identity())
        coverage.full_image = true;
    if (!state_->write_reserved && !state_->ReservePhysicalWrite()) throw std::runtime_error("workspace physical generation is acquired");
    state_->write_invalidated = true;
    state_->content_valid = false;
    state_->revision = 0U;
    std::atomic_ref{state_->access_signal->generation}.store(0U, std::memory_order_relaxed);
    ImageStream* execution = &*state_->stream;
    std::uintptr_t completion = state_->completion;
    try {
        auto destination = plane(clean.descriptor.width, clean.descriptor.height);
        const bool aliases_destination = source.plane_count() == 1U && clean.allocation.identity == identity();
        if (source.plane(0U).device() == layout().device) {
            const auto context = source.plane(0U).context();
            destination = ProducerPlaneLocked(context, clean.descriptor.width, clean.descriptor.height);
            if (state_->producer_context && *state_->producer_context == context) {
                execution = &*state_->producer_stream;
                completion = state_->producer_completion;
            }
            context.Bind();
            execution->Await(source);
        } else {
            const auto index = source.plane_count() - 1U;
            auto& transfer = state_->transfers[index];
            if (!transfer) {
                auto storage =
                    std::make_unique<ImageProductBuffer>(*state_->context, index == 0U ? ImageProductLayout::Clean : ImageProductLayout::CleanAndSemantic);
                if (index == 0U) storage->AdoptExternalPlane(state_->allocation, allocation_bytes(), destination);
                transfer = std::move(storage);
            }
            source = transfer->CopyDisplayFrom(*execution, std::move(source), coverage);
            state_->context->Bind();
        }
        const auto input = source.plane(0U).plane();
        if (!aliases_destination && input.data != destination.data) {
            if (!finalize) throw std::invalid_argument("workspace finalization policy is unavailable");
            finalize(input, source.plane_count() == 2U ? source.plane(1U).plane() : ImagePlaneView{}, destination, coverage, execution->native_handle());
        }
        state_->pending_source = std::make_unique<ImageProductReadCompletion>(std::move(source));
        state_->pending_execution = execution;
        state_->pending_content = content;
        state_->pending_width = clean.descriptor.width;
        state_->pending_height = clean.descriptor.height;
        state_->completion_notified.store(false, std::memory_order_release);
        execution->Notify([state = state_.get()] {
            state->completion_notified.store(true, std::memory_order_release);
            State::InvokeWake(state->availability_sink);
        });
        execution->Record(completion);
        state_->completed_event = completion;
    } catch (...) {
        const auto failure = std::current_exception();
        state_->content = {};
        state_->width = state_->height = 0U;
        const auto settled = execution->Settle();
        state_->FinishPending(settled, false);
        if (!settled.completion_reached) {
            if (source.valid()) {
                source.Quarantine();
                state_->unsettled_source.emplace(std::move(source));
            }
            state_->owner->Failed(combine_image_failures(failure, settled.failure));
        } else {
            state_->write_reserved = false;
            state_->PublishAccess(kWorkspaceAccessEmpty);
        }
        execution->RethrowAfterSettlement(failure);
    }
    lock.unlock();
    Complete();
}
struct BorrowedImageWorkspace::Lease final {
    BorrowedImageProductReadView source;
    std::shared_ptr<ImageWorkspace> workspace;
    ImagePlaneView plane;
    std::uint64_t revision = 0U;
};
BorrowedImageWorkspace::BorrowedImageWorkspace() noexcept = default;
BorrowedImageWorkspace::~BorrowedImageWorkspace() = default;
BorrowedImageWorkspace::BorrowedImageWorkspace(BorrowedImageWorkspace&&) noexcept = default;
BorrowedImageWorkspace& BorrowedImageWorkspace::operator=(BorrowedImageWorkspace&&) noexcept = default;
BorrowedImageWorkspace::BorrowedImageWorkspace(BorrowedImageProductReadView source, std::shared_ptr<ImageWorkspace> workspace) {
    if (!source.valid() || !workspace || !workspace->Contains({source.plane(0U).plane().allocation.owner, source.plane(0U).revision()})) return;
    const auto descriptor = source.plane(0U).plane().descriptor;
    const auto plane = workspace->plane(descriptor.width, descriptor.height);
    const auto revision = workspace->revision();
    lease_ = std::make_unique<Lease>(std::move(source), std::move(workspace), plane, revision);
}
bool BorrowedImageWorkspace::valid() const noexcept { return lease_ && lease_->source.valid(); }
ImagePlaneView BorrowedImageWorkspace::plane() const noexcept { return valid() ? lease_->plane : ImagePlaneView{}; }
const ImageWorkspaceLayout& BorrowedImageWorkspace::layout() const {
    if (!valid()) throw std::invalid_argument("workspace borrow is unavailable");
    return lease_->workspace->layout();
}
std::uint64_t BorrowedImageWorkspace::identity() const noexcept { return valid() ? lease_->workspace->identity() : 0U; }
std::uint64_t BorrowedImageWorkspace::revision() const noexcept { return valid() ? lease_->revision : 0U; }
std::size_t BorrowedImageWorkspace::allocation_bytes() const noexcept { return valid() ? lease_->workspace->allocation_bytes() : 0U; }
std::unique_ptr<ImageProductReadCompletion> BorrowedImageWorkspace::TakeCompletion() && {
    if (!valid()) throw std::invalid_argument("workspace borrow is unavailable");
    return std::make_unique<ImageProductReadCompletion>(std::move(lease_->source));
}
void ImageStream::Await(const BorrowedImageWorkspace& source) {
    if (!source.valid()) throw std::invalid_argument("workspace borrow is unavailable");
    AwaitEvent(source.lease_->workspace->state_->completed_event);
}
}  // namespace mmltk::frameworks::gpu
