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
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "src/common/io/noexcept_io.h"

namespace mmltk::frameworks::gpu {

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
    Notify();
}
std::exception_ptr ImageWorkspace::Owner::failure() const noexcept {
    std::scoped_lock lock(mutex_);
    return failure_;
}
ImageStreamSettlement ImageWorkspace::Owner::Retire() noexcept {
    std::scoped_lock lock(mutex_);
    closed_ = true;
    return {.completion_reached = live_ == 0U && !failure_, .failure = failure_};
}
bool ImageWorkspace::Owner::has_live_workspaces() const noexcept {
    std::scoped_lock lock(mutex_);
    return live_ != 0U;
}
void ImageWorkspace::Owner::SetRetirementSink(std::shared_ptr<const std::function<void()>> sink) noexcept {
    {
        std::scoped_lock lock(mutex_);
        retirement_sink_ = std::move(sink);
    }
    Notify();
}
void ImageWorkspace::Owner::Notify() const noexcept {
    std::shared_ptr<const std::function<void()>> sink;
    {
        std::scoped_lock lock(mutex_);
        if (!closed_ || (live_ != 0U && !failure_)) return;
        sink = retirement_sink_;
    }
    if (sink) {
        try {
            (*sink)();
        } catch (...) {}
    }
}

struct ImageWorkspace::State final {
    State(std::shared_ptr<Owner> family, ImageWorkspaceLayout requested, const Operations* injected)
        : owner(std::move(family)),
          layout(std::move(requested)),
          operations(injected ? *injected : Operations{&initialize_workspace_allocation, [](ImportedImageBuffer& buffer) noexcept {
                                                           return buffer.Release();
                                                       }}) {}
    void Initialize(DeviceContext source, std::optional<DeviceExecution> execution) {
        identity = next_image_allocation_identity();
        if (!layout.valid()) throw std::invalid_argument("workspace layout is invalid");
        access_descriptor.reset(::memfd_create("mmltk-workspace-access", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        if (access_descriptor.get() < 0 || ::ftruncate(access_descriptor.get(), sizeof(ImageWorkspaceAccessSignal)) != 0)
            throw std::runtime_error("workspace access allocation failed");
        const auto mapping =
            ::mmap(nullptr, sizeof(ImageWorkspaceAccessSignal), PROT_READ | PROT_WRITE, MAP_SHARED, access_descriptor.get(), 0);
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
        if (!gate.compare_exchange_strong(expected, epoch + kWorkspaceAccessMask + 1U + kWorkspaceAccessWriting, std::memory_order_acq_rel))
            return false;
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
    void Withdraw() noexcept {
        std::scoped_lock lock(access);
        auto gate = std::atomic_ref{access_signal->access};
        auto expected = gate.fetch_or(kWorkspaceAccessRevoked, std::memory_order_acq_rel) | kWorkspaceAccessRevoked;
        if ((expected & kWorkspaceAccessMask) == kWorkspaceAccessAvailable)
            // A winning external reader retains custody through settlement.
            static_cast<void>(gate.compare_exchange_strong(expected, (expected & ~kWorkspaceAccessMask) | kWorkspaceAccessEmpty,
                                                           std::memory_order_acq_rel));
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
    std::unique_ptr<ImageProductBuffer> transfer;
    std::optional<BorrowedImageProductReadView> unsettled_source;
    std::uintptr_t completion = 0U;
    std::uint64_t identity = 0U;
    mmltk::common::io::ScopedFd access_descriptor;
    mmltk::common::io::ScopedFd pending_memory;
    ImageWorkspaceAccessSignal* access_signal = nullptr;
    std::atomic_bool display_owned{false};
    std::atomic_bool display_held{false};
    bool write_reserved = false;
    bool write_invalidated = false;
    std::atomic<std::shared_ptr<const std::function<void()>>> availability_sink;
    std::atomic<std::shared_ptr<const std::function<void()>>> display_availability_sink;
    std::atomic<std::uint64_t> revision{0U};
    mutable std::mutex access;
    std::atomic<std::uint64_t> product_owner{0U};
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::atomic_bool admitted{false};
    std::atomic_bool withdrawn{false};
    TerminalCudaRetirementOwner terminal{1U};
    TerminalCudaRetirementLease retention = ReserveTerminalCudaLease(terminal);
};

ImageWorkspace::ImageWorkspace(std::shared_ptr<Owner> owner, DeviceContext source, ImageWorkspaceLayout layout,
                               std::optional<DeviceExecution> execution, const Operations* operations) {
    // Serialize admission with failure publication before any display context
    // or storage is constructed. A reservation already in progress may finish;
    // all subsequent attempts use this same closed family.
    {
        std::scoped_lock lock(owner->mutex_);
        if (owner->failure_) std::rethrow_exception(owner->failure_);
        if (owner->closed_) throw std::runtime_error("workspace owner is retired");
        state_ = std::make_shared<State>(owner, std::move(layout), operations);
        ++owner->live_;
    }
    try {
        state_->Initialize(std::move(source), std::move(execution));
    } catch (...) { std::rethrow_exception(Release(std::current_exception())); }
}
ImageWorkspace::~ImageWorkspace() noexcept { static_cast<void>(Release()); }
std::shared_ptr<ImageWorkspace> ImageWorkspace::Create(DeviceContext context, ImageWorkspaceLayout layout,
                                                       std::optional<DeviceExecution> execution) {
    auto result = std::shared_ptr<ImageWorkspace>(
        new ImageWorkspace(std::make_shared<Owner>(), std::move(context), std::move(layout), std::move(execution)));
    result->state_->display_owned = true;
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
            state_->transfer.reset();
            if (state_->producer_allocation) {
                const auto released = state_->producer_allocation->Release();
                if (released != cudaSuccess || state_->producer_allocation->release_failure() != cudaSuccess)
                    throw std::runtime_error("workspace producer mapping release failed");
                state_->producer_allocation.reset();
            }
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
        // Publish the family latch before installing terminal storage. The
        // complete state includes display context, stream, transfer and reads.
        owner->Failed(failure);
        auto retention = std::move(state_->retention);
        std::move(retention).Install(TerminalCudaCustody::Share(std::move(state_)), cudaErrorUnknown);
    } else {
        state_.reset();
    }
    {
        std::scoped_lock lock(owner->mutex_);
        --owner->live_;
    }
    owner->Notify();
    return failure;
}
void ImageWorkspace::CheckOwner(const std::shared_ptr<Owner>& owner) const {
    if (owner && owner != state_->owner) throw std::invalid_argument("workspace belongs to another runtime");
    state_->owner->Check();
}
const ImageWorkspaceLayout& ImageWorkspace::layout() const noexcept { return state_->layout; }
std::uint64_t ImageWorkspace::identity() const noexcept { return state_->identity; }
std::size_t ImageWorkspace::allocation_bytes() const noexcept { return layout().required_allocation_bytes; }
ImageStorageFootprint ImageWorkspace::StorageFootprint() const noexcept {
    std::scoped_lock lock(state_->access);
    auto result = state_->transfer ? state_->transfer->StorageFootprint() : ImageStorageFootprint{};
    // Clean cross-device transfer already owns the final allocation directly.
    if (!state_->transfer || state_->transfer->layout() != ImageProductLayout::Clean)
        result.device_bytes += state_->allocation->allocation_size();
    return result;
}
void ImageWorkspace::Attach(std::uint64_t product_owner) {
    CheckOwner();
    std::scoped_lock lock(state_->access);
    if (product_owner == 0U || (state_->product_owner != 0U && state_->product_owner != product_owner))
        throw std::invalid_argument("workspace allocation already belongs to another product slot");
    state_->product_owner = product_owner;
}
bool ImageWorkspace::admitted() const noexcept { return state_->admitted.load(std::memory_order_acquire); }
bool ImageWorkspace::retired() const noexcept {
    if (state_->withdrawn.load(std::memory_order_acquire)) return true;
    {
        std::scoped_lock lock(state_->owner->mutex_);
        if (!state_->owner->closed_) return false;
    }
    // Family retirement must resolve the same physical race as replacement.
    // Release the family lock before taking the workspace's access lock.
    state_->Withdraw();
    return true;
}
void ImageWorkspace::Withdraw() noexcept { state_->Withdraw(); }
std::uint64_t ImageWorkspace::revision() const noexcept { return state_->revision.load(std::memory_order_acquire); }
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
    auto result = plane(width, height);
    if (context == *state_->context) return result;
    if (context.device() != layout().device) throw std::invalid_argument("workspace producer belongs to another device");
    if (!state_->producer_context || *state_->producer_context != context) {
        if (state_->producer_allocation) {
            const auto released = state_->producer_allocation->Release();
            if (released != cudaSuccess || state_->producer_allocation->release_failure() != cudaSuccess) {
                const auto failure = workspace_release_failure("workspace producer mapping release failed");
                state_->owner->Failed(failure);
                std::rethrow_exception(failure);
            }
        }
        state_->producer_context.reset();
        state_->producer_allocation = state_->allocation->ImportAlias(context);
        state_->producer_context = context;
    }
    result.data = state_->producer_allocation->data();
    return result;
}
bool ImageWorkspace::WriteAvailable() const noexcept {
    if (!admitted() || state_->display_held.load(std::memory_order_acquire)) return false;
    const auto access = std::atomic_ref{state_->access_signal->access}.load(std::memory_order_acquire);
    return (access & kWorkspaceAccessMask) == kWorkspaceAccessEmpty || (access & kWorkspaceAccessMask) == kWorkspaceAccessAvailable;
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
    std::atomic_ref{state_->access_signal->generation}.store(0U, std::memory_order_relaxed);
}
void ImageWorkspace::CancelWrite() noexcept {
    {
        std::scoped_lock lock(state_->access);
        if (!state_->write_reserved) return;
        if (state_->write_invalidated) state_->revision = 0U;
        state_->write_reserved = false;
        state_->PublishAccess(state_->revision != 0U ? kWorkspaceAccessAvailable : kWorkspaceAccessEmpty);
    }
    if (const auto sink = state_->availability_sink.load(std::memory_order_acquire)) {
        try {
            (*sink)();
        } catch (...) {}
    }
}
bool ImageWorkspace::display_owned() const noexcept { return state_->display_owned.load(std::memory_order_acquire); }
bool ImageWorkspace::ReserveDisplayWrite() {
    std::scoped_lock lock(state_->access);
    if (!admitted() || state_->display_held.load(std::memory_order_acquire)) return false;
    if (!state_->ReservePhysicalWrite()) return false;
    state_->display_owned = true;
    state_->display_held.store(true, std::memory_order_release);
    return true;
}
void ImageWorkspace::CancelDisplayWrite() noexcept {
    CancelWrite();
    state_->display_held.store(false, std::memory_order_release);
    if (const auto sink = state_->availability_sink.load(std::memory_order_acquire)) {
        try { (*sink)(); } catch (...) {}
    }
}
std::uint64_t ImageWorkspace::product_owner() const noexcept { return state_->product_owner.load(std::memory_order_acquire); }
void ImageWorkspace::SetDisplayAvailabilitySink(std::shared_ptr<const std::function<void()>> sink) noexcept {
    state_->display_availability_sink.store(std::move(sink), std::memory_order_release);
}
void ImageWorkspace::Detach(std::uint64_t product_owner) {
    {
        std::scoped_lock lock(state_->access);
        if (state_->product_owner == product_owner) state_->product_owner = 0U;
        else if (state_->product_owner != 0U) throw std::invalid_argument("workspace detachment owner mismatch");
    }
    if (const auto sink = state_->display_availability_sink.load(std::memory_order_acquire)) {
        try { (*sink)(); } catch (...) {}
    }
}
bool ImageWorkspace::Acquired(std::uint64_t generation) const noexcept {
    return generation != 0U &&
           (std::atomic_ref{state_->access_signal->access}.load(std::memory_order_acquire) & kWorkspaceAccessMask) ==
               kWorkspaceAccessReading &&
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
    if (const auto sink = state_->availability_sink.load(std::memory_order_acquire)) {
        try {
            (*sink)();
        } catch (...) {}
    }
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
    std::scoped_lock lock(state_->access);
    auto settled = state_->stream ? state_->stream->Settle() : ImageStreamSettlement{.completion_reached = true};
    settled.completion_reached = settled.completion_reached && !state_->unsettled_source;
    if (!settled.completion_reached) {
        if (!settled.failure) settled.failure = workspace_release_failure("workspace completion was not established");
        state_->owner->Failed(settled.failure);
    }
    return settled;
}
void ImageWorkspace::Finalize(BorrowedImageProductReadView source, ImageWorkspaceCoverage coverage,
                              const ImageWorkspaceFinalize& finalize) {
    CheckOwner();
    std::scoped_lock lock(state_->access);
    if (!admitted() || !source.valid() || state_->unsettled_source) throw std::invalid_argument("workspace source is unavailable");
    const auto clean = source.plane(0U).plane();
    const auto revision = source.plane(0U).revision();
    const auto destination = plane(clean.descriptor.width, clean.descriptor.height);
    if (!finalize && clean.data != destination.data) throw std::invalid_argument("workspace finalization policy is unavailable");
    const bool expands_initialized = clean.descriptor.width > state_->width || clean.descriptor.height > state_->height;
    if (state_->revision == 0U || expands_initialized) coverage.full_image = true;
    if (!coverage.full_image && coverage.allocation_identity != identity())
        throw std::invalid_argument("workspace coverage belongs to another physical allocation");
    if (!state_->write_reserved) {
        if (!state_->ReservePhysicalWrite()) throw std::runtime_error("workspace physical generation is acquired");
    }
    state_->write_invalidated = true;
    state_->revision = 0U;
    std::atomic_ref{state_->access_signal->generation}.store(0U, std::memory_order_relaxed);
    try {
        const bool aliases_destination = source.plane_count() == 1U && clean.allocation.identity == identity();
        if (!aliases_destination && !source.plane(0U).UsesContext(*state_->context)) {
            const auto product_layout = source.plane_count() == 1U ? ImageProductLayout::Clean : ImageProductLayout::CleanAndSemantic;
            if (!state_->transfer) {
                auto transfer = std::make_unique<ImageProductBuffer>(*state_->context, product_layout);
                if (product_layout == ImageProductLayout::Clean)
                    transfer->AdoptExternalPlane(state_->allocation, allocation_bytes(), destination);
                state_->transfer = std::move(transfer);
            }
            if (state_->transfer->layout() != product_layout) throw std::invalid_argument("workspace raw layout changed");
            static_cast<void>(state_->transfer->CopyFrom(*state_->stream, std::move(source)));
            source = state_->transfer->Borrow();
            // Transfer storage is receiver-owned and already settled by CopyFrom.
        } else {
            state_->stream->Await(source);
        }
        const auto input = source.plane(0U).plane();
        if (!aliases_destination && input.data != destination.data)
            finalize(input, source.plane_count() == 2U ? source.plane(1U).plane() : ImagePlaneView{}, destination, coverage,
                     state_->stream->native_handle());
        state_->stream->Record(state_->completion);
        state_->stream->Synchronize();
        // Logical shrink does not discard initialized high-water contents.
        if (expands_initialized) {
            state_->width = destination.descriptor.width;
            state_->height = destination.descriptor.height;
        }
        state_->revision = revision;
        state_->write_invalidated = false;
        state_->write_reserved = false;
        std::atomic_ref{state_->access_signal->generation}.store(revision, std::memory_order_relaxed);
        state_->PublishAccess(kWorkspaceAccessAvailable);
    } catch (...) {
        const auto failure = std::current_exception();
        const auto settled = state_->stream->Settle();
        if (!settled.completion_reached) {
            source.Quarantine();
            state_->unsettled_source.emplace(std::move(source));
            state_->owner->Failed(combine_image_failures(failure, settled.failure));
        } else {
            state_->write_reserved = false;
            state_->PublishAccess(kWorkspaceAccessEmpty);
        }
        state_->stream->RethrowAfterSettlement(failure);
    }
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
    if (!source.valid() || !workspace || workspace->revision() != source.plane(0U).revision()) return;
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
    AwaitEvent(source.lease_->workspace->state_->completion);
}

}  // namespace mmltk::frameworks::gpu
