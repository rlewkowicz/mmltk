#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "src/common/io/scoped_fd.h"
#include "src/frameworks/gpu/image_types.h"

namespace mmltk::frameworks::gpu {

class DeviceContext;
class ImageStream;
class ImageProductBuffer;
class BorrowedImageProductReadView;
class ImageProductReadCompletion;

struct ImageWorkspaceLayout final {
    std::uint64_t device_incarnation = 0U;
    std::array<std::uint8_t, 16U> device_uuid{};
    int device = -1;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::size_t pitch_bytes = 0U;
    std::size_t offset_bytes = 0U;
    std::size_t required_allocation_bytes = 0U;
    std::size_t alignment_bytes = 0U;
    bool dedicated = false;
    ImageFormat format = ImageFormat::Rgba8;
    [[nodiscard]] bool valid() const noexcept;
    constexpr bool operator==(const ImageWorkspaceLayout&) const noexcept = default;
};

struct ImageWorkspaceRegion final {
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;
    std::int32_t x2 = 0;
    std::int32_t y2 = 0;
};
struct ImageWorkspaceCoverage final {
    // Partial coverage belongs to this exact physical allocation's contents.
    std::uint64_t allocation_identity = 0U;
    std::span<const ImageWorkspaceRegion> regions{};
    bool full_image = true;
};
using ImageWorkspaceFinalize = std::function<void(ImagePlaneView clean, ImagePlaneView semantic, ImagePlaneView destination,
                                                 ImageWorkspaceCoverage, std::uintptr_t stream)>;

// One physical CUDA opaque-FD allocation and its display-device execution.
// Admission precedes writes; raw products and browser imports have their own
// custody. No import registry or scheduling policy lives in this owner.
class ImageWorkspace final {
   public:
    ImageWorkspace(DeviceContext, ImageWorkspaceLayout);
    ~ImageWorkspace() noexcept;
    ImageWorkspace(const ImageWorkspace&) = delete;
    ImageWorkspace& operator=(const ImageWorkspace&) = delete;
    [[nodiscard]] const ImageWorkspaceLayout& layout() const noexcept;
    [[nodiscard]] std::uint64_t identity() const noexcept;
    [[nodiscard]] std::size_t allocation_bytes() const noexcept;
    [[nodiscard]] ImageStorageFootprint StorageFootprint() const noexcept;
    [[nodiscard]] mmltk::common::io::ScopedFd ExportDescriptor() const;
    // Called only after the importing device completed initial ownership setup.
    void Admit(std::uint64_t allocation_identity, std::uint64_t device_incarnation);
    [[nodiscard]] bool admitted() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] ImageStreamSettlement Settle() noexcept;

   private:
    struct State;
    std::shared_ptr<State> state_;
    [[nodiscard]] ImagePlaneView plane(std::uint32_t width, std::uint32_t height) const;
    void Release() noexcept;
    void Attach(std::uint64_t product_owner);
    void Finalize(BorrowedImageProductReadView, ImageWorkspaceCoverage, const ImageWorkspaceFinalize&);
    friend class ImageProductBuffer;
    friend class BorrowedImageWorkspace;
    friend class ImageStream;
};

class BorrowedImageWorkspace final {
   public:
    BorrowedImageWorkspace() noexcept;
    ~BorrowedImageWorkspace();
    BorrowedImageWorkspace(BorrowedImageWorkspace&&) noexcept;
    BorrowedImageWorkspace& operator=(BorrowedImageWorkspace&&) noexcept;
    BorrowedImageWorkspace(const BorrowedImageWorkspace&) = delete;
    BorrowedImageWorkspace& operator=(const BorrowedImageWorkspace&) = delete;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ImagePlaneView plane() const noexcept;
    [[nodiscard]] const ImageWorkspaceLayout& layout() const;
    [[nodiscard]] std::uint64_t identity() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::size_t allocation_bytes() const noexcept;
    [[nodiscard]] mmltk::common::io::ScopedFd ExportDescriptor() const;
    // Retains allocation and contexts with the existing completion owner. The
    // GPU callback drops only counted access; destroy after callback settlement.
    [[nodiscard]] std::unique_ptr<ImageProductReadCompletion> TakeCompletion() &&;

   private:
    struct Lease;
    std::unique_ptr<Lease> lease_;
    BorrowedImageWorkspace(BorrowedImageProductReadView, std::shared_ptr<ImageWorkspace>);
    friend class ImageProductBuffer;
    friend class ImageStream;
};

}  // namespace mmltk::frameworks::gpu
