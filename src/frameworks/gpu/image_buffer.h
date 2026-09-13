#pragma once
#include <optional>
#include "src/frameworks/gpu/device_execution.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <atomic>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "src/frameworks/gpu/image_geometry.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/frameworks/gpu/image_types.h"
#include "src/frameworks/gpu/image_product_retirement.h"
#include "src/frameworks/gpu/image_workspace.h"

namespace mmltk::frameworks::gpu {

class ImageCopyBackend {
   public:
    virtual ~ImageCopyBackend() = default;
    [[nodiscard]] virtual std::optional<DeviceExecution> ResolveExecution(int device, int numa_node);
    [[nodiscard]] virtual std::uintptr_t CreateContext(int device, DeviceContextMode) = 0;
    virtual void DestroyContext(int device, DeviceContextMode, std::uintptr_t context) noexcept = 0;
    virtual void BindContext(std::uintptr_t context) = 0;
    [[nodiscard]] virtual std::uintptr_t CreateStream(std::uintptr_t context) = 0;
    virtual void DestroyStream(std::uintptr_t context, std::uintptr_t stream) noexcept = 0;
    [[nodiscard]] virtual std::uintptr_t CreateEvent(std::uintptr_t context) = 0;
    virtual void DestroyEvent(std::uintptr_t context, std::uintptr_t event) noexcept = 0;
    [[nodiscard]] virtual ImagePlaneView AllocatePlane(std::uintptr_t context, ImagePlaneKind kind, std::uint32_t width,
                                                       std::uint32_t height) = 0;
    virtual void FreePlane(std::uintptr_t context, CUdeviceptr data) noexcept = 0;
    virtual void ClearPlane(std::uintptr_t context, std::uintptr_t stream, const ImagePlaneView&) = 0;
    [[nodiscard]] virtual std::shared_ptr<void> AllocatePinned(std::uintptr_t receiver_context,
                                                               const mmltk::common::system::ExecutionPlacement* receiver_placement,
                                                               std::size_t bytes) = 0;
    [[nodiscard]] virtual bool CanAccessPeer(int receiver, int source) = 0;
    virtual void WaitEvent(std::uintptr_t receiver_context, std::uintptr_t receiver_stream, std::uintptr_t source_event) = 0;
    virtual void CopySameDevice(std::uintptr_t context, std::uintptr_t stream, const ImagePlaneView& destination,
                                std::uintptr_t source_context, const ImagePlaneView& source) = 0;
    virtual void CopyPeer(std::uintptr_t receiver_context, std::uintptr_t receiver_stream, int receiver_device,
                          const ImagePlaneView& destination, std::uintptr_t source_context, int source_device,
                          const ImagePlaneView& source) = 0;
    virtual void CopyDeviceToHost(std::uintptr_t source_context, const ImagePlaneView& source, void* destination,
                                  std::size_t destination_pitch) = 0;
    virtual void CopyHostToDevice(std::uintptr_t receiver_context, std::uintptr_t receiver_stream, const void* source,
                                  std::size_t source_pitch, const ImagePlaneView& destination) = 0;
    virtual void RecordEvent(std::uintptr_t context, std::uintptr_t stream, std::uintptr_t event) = 0;
    virtual void SynchronizeEvent(std::uintptr_t context, std::uintptr_t event) = 0;
    // Notification is wake-only. The owner must settle the following stream
    // boundary before releasing any callback or GPU resource custody.
    virtual void NotifyStream(std::uintptr_t context, std::uintptr_t stream, std::function<void()>) = 0;
    using StreamSettlement = ImageStreamSettlement;
    [[nodiscard]] virtual StreamSettlement SettleStream(std::uintptr_t context, std::uintptr_t stream) noexcept = 0;
};

[[nodiscard]] std::shared_ptr<ImageCopyBackend> cuda_image_copy_backend();

class DeviceContext final {
   public:
    DeviceContext(int device, std::shared_ptr<ImageCopyBackend> backend, DeviceContextMode mode = DeviceContextMode::Isolated,
                  int numa_node = -1, std::optional<DeviceExecution> execution = {});
    ~DeviceContext();
    DeviceContext(const DeviceContext&) noexcept = default;
    DeviceContext& operator=(const DeviceContext&) noexcept = default;
    DeviceContext(DeviceContext&&) noexcept = default;
    DeviceContext& operator=(DeviceContext&&) noexcept = default;
    [[nodiscard]] int device() const noexcept;
    [[nodiscard]] bool operator==(const DeviceContext&) const noexcept = default;
    [[nodiscard]] const DeviceExecution* execution() const noexcept;
    void Bind() const;
    [[nodiscard]] DeviceContext OnDevice(int device, std::optional<DeviceExecution> execution = {}) const;
    [[nodiscard]] std::uintptr_t CreateEvent() const;
    void DestroyEvent(std::uintptr_t) const noexcept;

   private:
    struct State;
    std::shared_ptr<State> state_;
    friend class ImageBuffer;
    friend class ImageProductBuffer;
    friend class ImageStream;
};

class BorrowedImageProductReadView;
class ImageProductReadCompletion;

class ImageStream final {
   public:
    explicit ImageStream(DeviceContext);
    ~ImageStream();
    ImageStream(const ImageStream&) = delete;
    ImageStream& operator=(const ImageStream&) = delete;
    ImageStream(ImageStream&&) noexcept;
    ImageStream& operator=(ImageStream&&) noexcept;
    void Synchronize();
    // Enqueue the producer dependency; the caller retains the view until its
    // receiver reads settle, including partial submission and exceptions.
    void Await(const BorrowedImageProductReadView&);
    void Await(const BorrowedImageWorkspace&);
    [[nodiscard]] ImageCopyBackend::StreamSettlement Settle() noexcept;
    // Completion event belongs to this stream's retained device context.
    void Record(std::uintptr_t event);
    void AwaitEvent(std::uintptr_t event);
    void Notify(std::function<void()>);
    [[noreturn]] void RethrowAfterSettlement(std::exception_ptr);
    [[nodiscard]] std::uintptr_t native_handle() const noexcept { return stream_; }

   private:
    DeviceContext context_;
    std::uintptr_t stream_ = 0U;
    std::exception_ptr settlement_failure_;
    friend class ImageBuffer;
    friend class ImageProductBuffer;
};

class BorrowedImageReadView final {
   public:
    BorrowedImageReadView() noexcept;
    ~BorrowedImageReadView();
    BorrowedImageReadView(const BorrowedImageReadView&) = delete;
    BorrowedImageReadView& operator=(const BorrowedImageReadView&) = delete;
    BorrowedImageReadView(BorrowedImageReadView&&) noexcept;
    BorrowedImageReadView& operator=(BorrowedImageReadView&&) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int device() const noexcept;
    [[nodiscard]] bool UsesContext(const DeviceContext&) const noexcept;
    [[nodiscard]] DeviceContext context() const;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] ImagePlaneView plane() const noexcept;
    // Terminal receiver custody: retain physical storage, reject future source
    // writes, and release read locks so source shutdown remains finite.
    void Quarantine() noexcept;

   private:
    struct Lease;
    explicit BorrowedImageReadView(std::unique_ptr<Lease>) noexcept;
    std::unique_ptr<Lease> lease_;
    friend class ImageBuffer;
    friend class ImageProductBuffer;
    friend class ImageProductReadCompletion;
};

class ImageBuffer final {
   public:
    struct State;

    explicit ImageBuffer(DeviceContext);
    ~ImageBuffer();
    ImageBuffer(const ImageBuffer&) = delete;
    ImageBuffer& operator=(const ImageBuffer&) = delete;
    ImageBuffer(ImageBuffer&&) noexcept;
    ImageBuffer& operator=(ImageBuffer&&) noexcept;
    void Write(ImageStream&, ImagePlaneKind, std::uint32_t width, std::uint32_t height,
               const std::function<void(ImagePlaneView, std::uintptr_t)>&);
    [[nodiscard]] ImageCopyPath CopyFrom(ImageStream&, BorrowedImageReadView);
    [[nodiscard]] BorrowedImageReadView Borrow() const;
    [[nodiscard]] std::uint32_t capacity_width() const noexcept;
    [[nodiscard]] std::uint32_t capacity_height() const noexcept;
    [[nodiscard]] std::size_t staging_capacity_bytes() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;

   private:
    std::shared_ptr<State> state_;
    friend class ImageProductBuffer;
};

enum class ImageProductLayout : std::uint8_t {
    Clean,
    CleanAndSemantic,
};  // CLEANUP-IGNORE: Single-plane and product borrows are separate move-only resource views with different lease
    // ownership.

class BorrowedImageProductReadView final {
   public:
    BorrowedImageProductReadView() noexcept;
    ~BorrowedImageProductReadView();
    BorrowedImageProductReadView(const BorrowedImageProductReadView&) = delete;
    BorrowedImageProductReadView& operator=(const BorrowedImageProductReadView&) = delete;
    BorrowedImageProductReadView(BorrowedImageProductReadView&&) noexcept;
    BorrowedImageProductReadView& operator=(BorrowedImageProductReadView&&) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t plane_count() const noexcept;
    [[nodiscard]] const BorrowedImageReadView& plane(std::size_t) const;
    [[nodiscard]] BorrowedImageReadView TakePlane(std::size_t) &&;
    void Quarantine() noexcept;

   private:
    struct Lease;
    explicit BorrowedImageProductReadView(std::shared_ptr<Lease>) noexcept;
    std::shared_ptr<Lease> lease_;
    std::array<BorrowedImageReadView, 2U> planes_{};
    std::size_t count_ = 0U;
    friend class ImageProductBuffer;
    friend class ImageStream;
    friend class ImageProductReadCompletion;
};

// Converts thread-affine CPU read locks into a receiver-completion access
// count. Construct and destroy on the borrowing thread. Complete may run in
// a GPU host callback: it releases access and notifies availability, but never
// destroys a GPU resource. Retain this owner until receiver work is settled.
class ImageProductReadCompletion final {
   public:
    explicit ImageProductReadCompletion(BorrowedImageProductReadView&&);
    ~ImageProductReadCompletion();
    ImageProductReadCompletion(const ImageProductReadCompletion&) = delete;
    ImageProductReadCompletion& operator=(const ImageProductReadCompletion&) = delete;
    void Complete() noexcept;
    void Quarantine() noexcept;
    [[nodiscard]] bool pending() const noexcept;

   private:
    void ReleaseAccess() noexcept;
    BorrowedImageProductReadView source_;
    std::atomic_bool pending_{false};
    std::shared_ptr<const std::function<void()>> available_;
};

class ImageProductBuffer final {
   public:
    ImageProductBuffer(DeviceContext, ImageProductLayout, std::shared_ptr<ImageProductRetirement> = {});
    ~ImageProductBuffer();
    ImageProductBuffer(const ImageProductBuffer&) = delete;
    ImageProductBuffer& operator=(const ImageProductBuffer&) = delete;
    ImageProductBuffer(ImageProductBuffer&&) = delete;
    ImageProductBuffer& operator=(ImageProductBuffer&&) = delete;
    using ProductSubmit = std::function<void(ImagePlaneView clean, ImagePlaneView semantic, std::uintptr_t stream)>;
    void Publish(ImageStream&, std::uint32_t, std::uint32_t, ProductSubmit);
    using MissingPlaneSubmit = std::function<void(ImagePlaneView, std::uintptr_t stream)>;
    // Optional receiver initialization for planes absent from the source.
    // The initializer joins the same completion boundary as the copied planes.
    [[nodiscard]] std::array<ImageCopyPath, 2U> CopyFrom(ImageStream&, BorrowedImageProductReadView, MissingPlaneSubmit = {},
                                                         bool preserve_clean = false);
    [[nodiscard]] BorrowedImageProductReadView Borrow() const;
    [[nodiscard]] BorrowedImageWorkspace BorrowWorkspace() const;
    [[nodiscard]] ImageWorkspaceObservation ObserveWorkspace() const;
    [[nodiscard]] ImageStreamSettlement SettleWorkspace() noexcept;
    [[nodiscard]] ImageProductLayout layout() const noexcept;
    [[nodiscard]] std::uint32_t capacity_width() const noexcept;
    [[nodiscard]] std::uint32_t capacity_height() const noexcept;
    [[nodiscard]] std::size_t staging_capacity_bytes() const noexcept;
    // Physical high-water ownership, including every plane, irrespective of
    // the selected logical extent or product revision.
    [[nodiscard]] ImageStorageFootprint StorageFootprint() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;

   private:
    friend class ImageWorkspace;
    void AdoptExternalPlane(std::shared_ptr<void> custody, std::size_t bytes, ImagePlaneView);
    friend class ImageProductPool;
    [[nodiscard]] bool ConfigureWorkspace(std::shared_ptr<ImageWorkspace>, ImageWorkspaceFinalize);
    [[nodiscard]] bool DetachWorkspace(ImageStream&, const std::shared_ptr<ImageWorkspace>&);
    void FinalizeWorkspace(ImageWorkspaceCoverage = {});
    void CompleteWorkspace();
    friend class BorrowedImageProductReadView;
    friend class ImageStream;
    void PublishAs(ImageStream&, std::uint32_t, std::uint32_t, std::uint64_t, bool, ProductSubmit);
    [[nodiscard]] std::array<ImageAllocation, 2U> Allocations() const;
    [[nodiscard]] std::array<ImageCopyPath, 2U> CopyFromAs(ImageStream&, BorrowedImageProductReadView, MissingPlaneSubmit, std::uint64_t,
                                                           bool preserve_clean = false,
                                                           ImagePlanePreservation = ImagePlanePreservation::All);
    [[nodiscard]] bool writable() const;
    [[nodiscard]] bool ReserveWorkspaceWrite();
    void CancelWorkspaceWrite() noexcept;
    [[nodiscard]] bool terminal() const noexcept;
    [[nodiscard]] bool Owns(const BorrowedImageProductReadView&) const noexcept;
    // Wake-only notification: may run at receiver completion on a GPU callback
    // thread. It must not execute CUDA or synchronously perform product work.
    void SetAvailabilitySink(std::shared_ptr<const std::function<void()>>);
    void DeferReleaseToReaders() noexcept { deferred_release_ = true; }
    struct State;
    std::shared_ptr<State> state_;
    bool deferred_release_ = false;
};

}  // namespace mmltk::frameworks::gpu
