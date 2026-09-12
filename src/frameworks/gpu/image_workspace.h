#pragma once

#include <cuda_runtime_api.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

#include "src/common/io/scoped_fd.h"
#include "src/frameworks/gpu/image_types.h"
#include "src/frameworks/gpu/device_execution.h"

namespace mmltk::frameworks::gpu {

class DeviceContext;
class ImageStream;
class ImageProductBuffer;
class BorrowedImageProductReadView;
class ImageProductReadCompletion;
class SystemImageRuntime;
class ExportedImageBuffer;
namespace test_support {
struct ImageWorkspaceTestAccess;
}

inline constexpr std::uint64_t kWorkspaceAccessEmpty = 0U;
inline constexpr std::uint64_t kWorkspaceAccessWriting = 1U;
inline constexpr std::uint64_t kWorkspaceAccessAvailable = 2U;
inline constexpr std::uint64_t kWorkspaceAccessReading = 3U;
inline constexpr std::uint64_t kWorkspaceAccessMask = 3U;
inline constexpr std::uint64_t kWorkspaceAccessRevoked = std::uint64_t{1U} << 63U;

// The exporter owns this shared, generation-scoped physical access gate.
// Availability advertises completed pixels; it grants no reader custody.
struct alignas(64) ImageWorkspaceAccessSignal final {
    std::uint64_t access = kWorkspaceAccessEmpty;
    std::uint64_t generation = 0U;
    std::uint64_t allocation_identity = 0U;
    // Exact revoked Reading access token, published only after terminal GPU
    // retirement. Revocation forbids another browser read of this token;
    // reserving a subsequent native write clears it and advances the epoch.
    std::uint64_t terminal_read_complete = 0U;
};
static_assert(sizeof(ImageWorkspaceAccessSignal) == 64U);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);

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
    bool direct_sampling = false;
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
    ~ImageWorkspace() noexcept;
    ImageWorkspace(const ImageWorkspace&) = delete;
    ImageWorkspace& operator=(const ImageWorkspace&) = delete;
    [[nodiscard]] const ImageWorkspaceLayout& layout() const noexcept;
    [[nodiscard]] std::uint64_t identity() const noexcept;
    [[nodiscard]] std::size_t allocation_bytes() const noexcept;
    [[nodiscard]] ImageStorageFootprint StorageFootprint() const noexcept;
    [[nodiscard]] mmltk::common::io::ScopedFd ExportDescriptor() const;
    [[nodiscard]] mmltk::common::io::ScopedFd ExportAccessDescriptor() const;
    [[nodiscard]] bool WriteAvailable() const noexcept;
    [[nodiscard]] bool ReserveWrite();
    void InvalidateWrite() noexcept;
    void CancelWrite() noexcept;
    [[nodiscard]] bool Acquired(std::uint64_t generation) const noexcept;
    [[nodiscard]] bool TerminalReadComplete(std::uint64_t generation) const noexcept;
    void CompleteRead(std::uint64_t generation);
    // Called only after the importing device completed initial ownership setup.
    void Admit(std::uint64_t allocation_identity, std::uint64_t device_incarnation);
    [[nodiscard]] bool admitted() const noexcept;
    [[nodiscard]] bool retired() const noexcept;
    void Withdraw() noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] ImageStreamSettlement Settle() noexcept;

   private:
    // Shared by every candidate in one runtime, including candidates that
    // never reach a product slot and owners released after replacement.
    class Owner final {
       public:
        void Check() const;
        void Failed(std::exception_ptr) noexcept;
        [[nodiscard]] std::exception_ptr failure() const noexcept;
        [[nodiscard]] ImageStreamSettlement Retire() noexcept;
        [[nodiscard]] bool has_live_workspaces() const noexcept;
        void SetRetirementSink(std::shared_ptr<const std::function<void()>>) noexcept;

       private:
        mutable std::mutex mutex_;
        std::exception_ptr failure_;
        std::size_t live_ = 0U;
        bool closed_ = false;
        std::shared_ptr<const std::function<void()>> retirement_sink_;
        void Notify() const noexcept;
        friend class ImageWorkspace;
    };
    struct Operations final {
        void (*initialize)(ExportedImageBuffer&, const ImageWorkspaceLayout&);
        cudaError_t (*release)(ExportedImageBuffer&) noexcept;
    };
    struct State;
    std::shared_ptr<State> state_;
    ImageWorkspace(std::shared_ptr<Owner>, DeviceContext, ImageWorkspaceLayout, std::optional<DeviceExecution>,
                   const Operations* = nullptr);
    [[nodiscard]] ImagePlaneView plane(std::uint32_t width, std::uint32_t height) const;
    [[nodiscard]] std::exception_ptr Release(std::exception_ptr = {}) noexcept;
    void CheckOwner(const std::shared_ptr<Owner>& = {}) const;
    void Attach(std::uint64_t product_owner);
    void SetAvailabilitySink(std::shared_ptr<const std::function<void()>>) noexcept;
    void Finalize(BorrowedImageProductReadView, ImageWorkspaceCoverage, const ImageWorkspaceFinalize&);
    friend class ImageProductBuffer;
    friend class BorrowedImageWorkspace;
    friend class ImageStream;
    friend class SystemImageRuntime;
    friend struct test_support::ImageWorkspaceTestAccess;
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

struct ImageWorkspaceObservation final {
    std::uint64_t product_owner = 0U;
    std::uint64_t product_revision = 0U;
    std::shared_ptr<ImageWorkspace> workspace{};
};

}  // namespace mmltk::frameworks::gpu
