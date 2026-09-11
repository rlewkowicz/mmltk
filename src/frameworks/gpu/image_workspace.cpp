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

struct ImageWorkspace::State final {
    State(DeviceContext selected, ImageWorkspaceLayout requested)
        : context(std::move(selected)), layout(std::move(requested)), stream(context) {}
    void Initialize() {
        identity = next_image_allocation_identity();
        if (!layout.valid() || context.device() != layout.device) throw std::invalid_argument("workspace layout is invalid");
        context.Bind();
        CUuuid uuid{};
        if (cuDeviceGetUuid(&uuid, layout.device) != CUDA_SUCCESS ||
            std::memcmp(uuid.bytes, layout.device_uuid.data(), layout.device_uuid.size()) != 0)
            throw std::invalid_argument("workspace physical device UUID mismatch");
        std::string error;
        if (!allocation->allocate(layout.device, layout.width, layout.height, layout.pitch_bytes,
                                 layout.required_allocation_bytes, &error, layout.offset_bytes)) throw std::runtime_error(error);
        if (allocation->allocation_size() % layout.alignment_bytes != 0U)
            throw std::invalid_argument("workspace allocation alignment mismatch");
        completion = context.CreateEvent();
    }
    ~State() {
        context.DestroyEvent(completion);
        // ImageWorkspace's outer owner settles before resource destruction.
    }
    DeviceContext context;
    const ImageWorkspaceLayout layout;
    ImageStream stream;
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

ImageWorkspace::ImageWorkspace(DeviceContext context, ImageWorkspaceLayout layout)
    : state_(std::make_shared<State>(std::move(context), std::move(layout))) {
    try {
        state_->Initialize();
    } catch (...) {
        const auto failure = std::current_exception();
        Release();
        std::rethrow_exception(failure);
    }
}
ImageWorkspace::~ImageWorkspace() noexcept { Release(); }
void ImageWorkspace::Release() noexcept {
    bool safe = Settle().completion_reached;
    if (safe) {
        try {
            state_->context.Bind();
            safe = state_->allocation->Release() == cudaSuccess && state_->allocation->release_failure() == cudaSuccess;
        } catch (...) { safe = false; }
    }
    if (!safe) {
        auto retention = std::move(state_->retention);
        std::move(retention).Install(TerminalCudaCustody::Share(std::move(state_)), cudaErrorUnknown);
    }
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
    if (allocation_identity != identity() || device_incarnation != layout().device_incarnation || state_->admitted)
        throw std::invalid_argument("workspace admission identity mismatch or duplicate");
    state_->admitted = true;
}
ImageStreamSettlement ImageWorkspace::Settle() noexcept {
    if (!state_) return {.completion_reached = true};
    std::scoped_lock lock(state_->access);
    auto settled = state_->stream.Settle();
    settled.completion_reached = settled.completion_reached && !state_->unsettled_source;
    return settled;
}
void ImageWorkspace::Finalize(BorrowedImageProductReadView source, ImageWorkspaceCoverage coverage,
                               const ImageWorkspaceFinalize& finalize) {
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
                auto transfer = std::make_unique<ImageProductBuffer>(state_->context, product_layout);
                if (product_layout == ImageProductLayout::Clean)
                    transfer->AdoptExternalPlane(state_->allocation, allocation_bytes(), destination);
                state_->transfer = std::move(transfer);
            }
            if (state_->transfer->layout() != product_layout) throw std::invalid_argument("workspace raw layout changed");
            static_cast<void>(state_->transfer->CopyFrom(state_->stream, std::move(source)));
            source = state_->transfer->Borrow();
            // Transfer storage is receiver-owned and already settled by CopyFrom.
        } else {
            state_->stream.Await(source);
        }
        const auto input = source.plane(0U).plane();
        if (input.data != destination.data)
            finalize(input, source.plane_count() == 2U ? source.plane(1U).plane() : ImagePlaneView{}, destination, coverage,
                     state_->stream.native_handle());
        state_->stream.Record(state_->completion);
        state_->stream.Synchronize();
        // Logical shrink does not discard initialized high-water contents.
        if (expands_initialized) {
            state_->width = destination.descriptor.width;
            state_->height = destination.descriptor.height;
        }
        state_->revision = revision;
    } catch (...) {
        const auto failure = std::current_exception();
        const auto settled = state_->stream.Settle();
        if (!settled.completion_reached) {
            source.Quarantine();
            state_->unsettled_source.emplace(std::move(source));
        }
        state_->stream.RethrowAfterSettlement(failure);
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
