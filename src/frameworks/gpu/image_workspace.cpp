#include "src/frameworks/gpu/image_workspace.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "src/frameworks/gpu/exported_image_buffer.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"

namespace mmltk::frameworks::gpu {

bool ImageWorkspaceLayout::valid() const noexcept {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    return format == ImageFormat::Rgba8 && device >= 0 && device_incarnation != 0U && width != 0U && height != 0U &&
           width <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) &&
           height <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) &&
           std::ranges::any_of(device_uuid, [](auto byte) { return byte != 0U; }) &&
           pitch_bytes >= static_cast<std::size_t>(width) * 4U && alignment_bytes != 0U &&
           (alignment_bytes & (alignment_bytes - 1U)) == 0U &&
           height <= (maximum - offset_bytes) / pitch_bytes &&
           required_allocation_bytes >= offset_bytes + pitch_bytes * height;
}

namespace {
[[nodiscard]] std::exception_ptr workspace_release_failure(const char* message) noexcept {
    try { throw ImageStreamExecutionFailure(std::make_exception_ptr(std::runtime_error(message))); }
    catch (...) { return std::current_exception(); }
}

void initialize_workspace_allocation(ExportedImageBuffer& allocation, const ImageWorkspaceLayout& layout) {
    CUuuid uuid{};
    if (cuDeviceGetUuid(&uuid, layout.device) != CUDA_SUCCESS ||
        std::memcmp(uuid.bytes, layout.device_uuid.data(), layout.device_uuid.size()) != 0)
        throw std::invalid_argument("workspace physical device UUID mismatch");
    std::string error;
    if (!allocation.allocate(layout.device, layout.width, layout.height, layout.pitch_bytes,
                             layout.required_allocation_bytes, &error, layout.offset_bytes)) throw std::runtime_error(error);
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
        try { (*sink)(); } catch (...) {}
    }
}

struct ImageWorkspace::State final {
    State(std::shared_ptr<Owner> family, ImageWorkspaceLayout requested, const Operations* injected)
        : owner(std::move(family)), layout(std::move(requested)),
          operations(injected ? *injected : Operations{&initialize_workspace_allocation,
                     [](ExportedImageBuffer& buffer) noexcept { return buffer.Release(); }}) {}
    void Initialize(DeviceContext source, std::optional<DeviceExecution> execution) {
        identity = next_image_allocation_identity();
        if (!layout.valid()) throw std::invalid_argument("workspace layout is invalid");
        context.emplace(source.OnDevice(layout.device, std::move(execution)));
        stream.emplace(*context);
        context->Bind();
        operations.initialize(*allocation, layout);
        if (allocation->allocation_size() % layout.alignment_bytes != 0U)
            throw std::invalid_argument("workspace allocation alignment mismatch");
        completion = context->CreateEvent();
    }
    ~State() {
        if (context) context->DestroyEvent(completion);
    }
    std::shared_ptr<Owner> owner;
    std::optional<DeviceContext> context;
    const ImageWorkspaceLayout layout;
    const Operations operations;
    std::optional<ImageStream> stream;
    std::shared_ptr<ExportedImageBuffer> allocation = std::make_shared<ExportedImageBuffer>();
    std::unique_ptr<ImageProductBuffer> transfer;
    std::optional<BorrowedImageProductReadView> unsettled_source;
    std::uintptr_t completion = 0U;
    std::uint64_t identity = 0U;
    std::atomic<std::uint64_t> revision{0U};
    mutable std::mutex access;
    std::uint64_t product_owner = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::atomic_bool admitted{false};
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
    } catch (...) {
        std::rethrow_exception(Release(std::current_exception()));
    }
}
ImageWorkspace::~ImageWorkspace() noexcept { static_cast<void>(Release()); }
std::exception_ptr ImageWorkspace::Release(std::exception_ptr initiating) noexcept {
    if (!state_) return initiating;
    auto settled = Settle();
    auto failure = combine_image_failures(initiating, settled.failure);
    bool safe = settled.completion_reached;
    if (safe) {
        try {
            if (state_->context) state_->context->Bind();
            const auto released = state_->operations.release(*state_->allocation);
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
std::size_t ImageWorkspace::allocation_bytes() const noexcept { return state_->allocation->allocation_size(); }
ImageStorageFootprint ImageWorkspace::StorageFootprint() const noexcept {
    std::scoped_lock lock(state_->access);
    auto result = state_->transfer ? state_->transfer->StorageFootprint() : ImageStorageFootprint{};
    // Clean cross-device transfer already owns the final allocation directly.
    if (!state_->transfer || state_->transfer->layout() != ImageProductLayout::Clean) result.device_bytes += allocation_bytes();
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
std::uint64_t ImageWorkspace::revision() const noexcept { return state_->revision.load(std::memory_order_acquire); }
ImagePlaneView ImageWorkspace::plane(std::uint32_t width, std::uint32_t height) const {
    if (width == 0U || height == 0U || width > layout().width || height > layout().height)
        throw std::invalid_argument("workspace logical extent exceeds capacity");
    return {state_->allocation->data(), {ImagePlaneKind::Clean, ImageFormat::Rgba8, width, height, layout().pitch_bytes},
            {identity(), layout().width, layout().height, identity()}};
}
mmltk::common::io::ScopedFd ImageWorkspace::ExportDescriptor() const {
    std::string error;
    mmltk::common::io::ScopedFd descriptor(state_->allocation->export_descriptor(&error));
    if (descriptor.get() < 0) throw std::runtime_error(error);
    return descriptor;
}
void ImageWorkspace::Admit(std::uint64_t allocation_identity, std::uint64_t device_incarnation) {
    std::scoped_lock lock(state_->access);
    std::scoped_lock owner_lock(state_->owner->mutex_);
    if (state_->owner->failure_) std::rethrow_exception(state_->owner->failure_);
    if (state_->owner->closed_) throw std::runtime_error("workspace owner is retired");
    if (allocation_identity != identity() || device_incarnation != layout().device_incarnation || state_->admitted)
        throw std::invalid_argument("workspace admission identity mismatch or duplicate");
    state_->admitted = true;
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
    state_->revision = 0U;
    try {
        if (source.plane(0U).device() != layout().device) {
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
        if (input.data != destination.data)
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
    } catch (...) {
        const auto failure = std::current_exception();
        const auto settled = state_->stream->Settle();
        if (!settled.completion_reached) {
            source.Quarantine();
            state_->unsettled_source.emplace(std::move(source));
            state_->owner->Failed(combine_image_failures(failure, settled.failure));
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
std::size_t BorrowedImageWorkspace::allocation_bytes() const noexcept {
    return valid() ? lease_->workspace->allocation_bytes() : 0U;
}
mmltk::common::io::ScopedFd BorrowedImageWorkspace::ExportDescriptor() const {
    if (!valid()) throw std::invalid_argument("workspace borrow is unavailable");
    return lease_->workspace->ExportDescriptor();
}
std::unique_ptr<ImageProductReadCompletion> BorrowedImageWorkspace::TakeCompletion() && {
    if (!valid()) throw std::invalid_argument("workspace borrow is unavailable");
    return std::make_unique<ImageProductReadCompletion>(std::move(lease_->source));
}
void ImageStream::Await(const BorrowedImageWorkspace& source) {
    if (!source.valid()) throw std::invalid_argument("workspace borrow is unavailable");
    AwaitEvent(source.lease_->workspace->state_->completion);
}

}  // namespace mmltk::frameworks::gpu
