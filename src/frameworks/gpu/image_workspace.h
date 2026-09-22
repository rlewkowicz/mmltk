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
#include <utility>
#include "src/common/io/scoped_fd.h"
#include "src/frameworks/gpu/image_types.h"
#include "src/frameworks/gpu/device_execution.h"
namespace mmltk::frameworks::gpu {
class DeviceContext;
class ImageStream;
class ImageProductBuffer;
class ImageProductRetirement;
class BorrowedImageProductReadView;
class ImageProductReadCompletion;
class ImportedImageBuffer;
namespace test_support {
struct ImageWorkspaceTestAccess;
}
inline constexpr std::uint64_t kWorkspaceAccessEmpty = 0U;
inline constexpr std::uint64_t kWorkspaceAccessWriting = 1U;
inline constexpr std::uint64_t kWorkspaceAccessAvailable = 2U;
inline constexpr std::uint64_t kWorkspaceAccessReading = 3U;
inline constexpr std::uint64_t kWorkspaceAccessMask = 3U;
inline constexpr std::uint64_t kWorkspaceAccessRevoked = std::uint64_t{1U} << 63U;
// The native producer owns this shared, generation-scoped physical access gate.
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
 constexpr bool operator==(const ImageWorkspaceRegion&) const noexcept = default;
};
struct ImageWorkspaceContent final {
 std::uint64_t owner = 0U;
 std::uint64_t revision = 0U;
 constexpr bool operator==(const ImageWorkspaceContent&) const noexcept = default;
 [[nodiscard]] constexpr bool valid() const noexcept { return owner != 0U && revision != 0U; }
};
struct ImageWorkspaceCoverage final {
 // Partial coverage belongs to this exact physical allocation's contents.
 std::uint64_t allocation_identity = 0U;
 std::span<const ImageWorkspaceRegion> regions{};
 bool full_image = true;
 ImageWorkspaceContent baseline{};
};
// Bounded raw-allocation history lets either physical display allocation
// accumulate all changes since its own completed content. An unknown baseline,
// source replacement or history overflow conservatively initializes the image.
class ImageWorkspaceDamage final {
public:
 void Record(ImageWorkspaceContent, ImageWorkspaceCoverage) noexcept;
 [[nodiscard]] ImageWorkspaceCoverage Since(ImageWorkspaceContent, ImageWorkspaceContent, std::uint64_t allocation) noexcept;

private:
 class Regions final {
 public:
  void Insert(ImageWorkspaceRegion) noexcept;
  [[nodiscard]] std::span<const ImageWorkspaceRegion> Coverage() const noexcept;

 private:
  std::array<ImageWorkspaceRegion, 8U> rectangles_{};
  std::size_t count_ = 0U;
  bool coarsened_ = false;
 };
 struct Change final {
  ImageWorkspaceContent before{}, after{};
  Regions regions{};
  bool full = true;
 };
 std::array<Change, 64U> changes_{};
 std::size_t next_ = 0U, count_ = 0U;
 ImageWorkspaceContent newest_{};
 Regions accumulated_{};
};
using ImageWorkspaceFinalize = std::function<void(ImagePlaneView clean, ImagePlaneView semantic, ImagePlaneView destination, ImageWorkspaceCoverage, std::uintptr_t stream)>;
// One physical Vulkan opaque-FD allocation, producer-context mappings, and
// display-device transfer storage when the producer resides on another GPU.
// Admission precedes writes; raw products and browser imports have their own
// custody. No import registry or scheduling policy lives in this owner.
class ImageWorkspace final {
 class Owner;

public:
 struct AccessObservation final {
  std::uint64_t access = 0U;
  std::uint64_t generation = 0U;
  bool display_held = false;
  bool write_reserved = false;
  bool completion_pending = false;
 };
 [[nodiscard]] AccessObservation ObserveAccess() const noexcept;
 class Retirement final {
 public:
  struct Result final {
   // Release has either freed storage or installed terminal custody.
   bool complete = false;
   // Only the observer that claims the result reports an operation failure.
   bool claimed = false;
   ImageStreamSettlement settlement{};
  };
  Retirement() noexcept = default;
  [[nodiscard]] Result TakeResult() const noexcept;
  // Atomically assigns the eventual result to the attached producer's
  // counted retirement owner. Acceptance survives subsequent detachment.
  // On rejection the display must retain this observation until release.
  [[nodiscard]] bool TransferToProducer() const noexcept;
  void SetWake(std::shared_ptr<const std::function<void()>>) const noexcept;

 private:
  explicit Retirement(std::shared_ptr<Owner> owner) noexcept : owner_(std::move(owner)) {}
  std::shared_ptr<Owner> owner_;
  friend class ImageWorkspace;
 };
 [[nodiscard]] static std::shared_ptr<ImageWorkspace> Create(DeviceContext, ImageWorkspaceLayout, std::optional<DeviceExecution> = {});
 ~ImageWorkspace() noexcept;
 ImageWorkspace(const ImageWorkspace&) = delete;
 ImageWorkspace& operator=(const ImageWorkspace&) = delete;
 [[nodiscard]] const ImageWorkspaceLayout& layout() const noexcept;
 [[nodiscard]] std::uint64_t identity() const noexcept;
 [[nodiscard]] Retirement ObserveRetirement() const noexcept;
 [[nodiscard]] std::size_t allocation_bytes() const noexcept;
 [[nodiscard]] ImageStorageFootprint StorageFootprint() const noexcept;
 [[nodiscard]] mmltk::common::io::ScopedFd ExportAccessDescriptor() const;
 [[nodiscard]] bool WriteAvailable() const noexcept;
 [[nodiscard]] bool FinalizationPending() const noexcept;
 [[nodiscard]] bool ReserveWrite();
 [[nodiscard]] bool ReserveDisplayWrite();
 void CancelDisplayWrite() noexcept;
 void Detach(std::uint64_t product_owner);
 [[nodiscard]] std::uint64_t product_owner() const noexcept;
 void SetDisplayAvailabilitySink(std::shared_ptr<const std::function<void()>>) noexcept;
 void InvalidateWrite() noexcept;
 void CancelWrite() noexcept;
 [[nodiscard]] bool Acquired(std::uint64_t generation) const noexcept;
 [[nodiscard]] bool TerminalReadComplete(std::uint64_t generation) const noexcept;
 void CompleteRead(std::uint64_t generation);
 // Queue initialized backing without running GPU work. Withdrawal closes an
 // unconsumed descriptor; only the producer execution owner admits its mapping.
 [[nodiscard]] bool QueueAllocation(mmltk::common::io::ScopedFd);
 void Admit(std::uint64_t allocation_identity, std::uint64_t device_incarnation);
 [[nodiscard]] bool admitted() const noexcept;
 [[nodiscard]] bool retired() const noexcept;
 void Withdraw() noexcept;
 [[nodiscard]] std::uint64_t revision() const noexcept;
 [[nodiscard]] bool Contains(ImageWorkspaceContent) const noexcept;
 [[nodiscard]] ImageWorkspaceContent Content() const noexcept;
 [[nodiscard]] ImageStreamSettlement Settle() noexcept;
 [[nodiscard]] ImagePlaneView plane(std::uint32_t width, std::uint32_t height) const;
 void Finalize(BorrowedImageProductReadView, ImageWorkspaceCoverage, const ImageWorkspaceFinalize&);
 // Owner-worker completion drain. A notification permits settlement, never
 // publication or resource destruction from inside the host callback.
 void Complete();

private:
 // Failure latch belongs only to this independent physical allocation.
 class Owner final {
 public:
  void Check() const;
  void Failed(std::exception_ptr) noexcept;
  [[nodiscard]] std::exception_ptr failure() const noexcept;
  void Released(ImageStreamSettlement) noexcept;
  [[nodiscard]] Retirement::Result TakeResult() noexcept;
  [[nodiscard]] bool TransferToProducer() noexcept;
  void SetWake(std::shared_ptr<const std::function<void()>>) noexcept;
  std::atomic<std::uint64_t> product_owner{0U};

 private:
  mutable std::mutex mutex_;
  std::exception_ptr failure_;
  bool closed_ = false;
  bool release_complete_ = false;
  bool release_claimed_ = false;
  ImageStreamSettlement release_result_;
  std::shared_ptr<ImageProductRetirement> attached_producer_;
  std::shared_ptr<ImageProductRetirement> responsible_producer_;
  std::shared_ptr<const std::function<void()>> wake_;
  void Wake() const noexcept;
  friend class ImageWorkspace;
 };
 struct Operations final {
  void (*initialize)(ImportedImageBuffer&, DeviceContext, const ImageWorkspaceLayout&, mmltk::common::io::ScopedFd, std::uint64_t);
  cudaError_t (*release)(ImportedImageBuffer&) noexcept;
  std::shared_ptr<ImportedImageBuffer> (*alias)(const ImportedImageBuffer&, DeviceContext);
 };
 struct State;
 std::shared_ptr<State> state_;
 ImageWorkspace(DeviceContext, ImageWorkspaceLayout, std::optional<DeviceExecution>, const Operations*);
 [[nodiscard]] static std::shared_ptr<ImageWorkspace> Create(DeviceContext, ImageWorkspaceLayout, std::optional<DeviceExecution>, const Operations*);
 [[nodiscard]] std::exception_ptr Release(std::exception_ptr = {}) noexcept;
 void CheckOwner() const;
 void Attach(std::uint64_t product_owner, std::shared_ptr<ImageProductRetirement>);
 [[nodiscard]] ImagePlaneView ProducerPlane(const DeviceContext&, std::uint32_t width, std::uint32_t height);
 [[nodiscard]] ImagePlaneView ProducerPlaneLocked(const DeviceContext&, std::uint32_t width, std::uint32_t height);
 void SetAvailabilitySink(std::shared_ptr<const std::function<void()>>) noexcept;
 friend class ImageProductBuffer;
 friend class BorrowedImageWorkspace;
 friend class ImageStream;
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
