#pragma once
#include "src/frameworks/gpu/image_product_retirement.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

#include "src/frameworks/gpu/image_buffer.h"

namespace mmltk::frameworks::gpu {

class ImageProductPool final {
    struct Admission;
    struct Slot;

   public:
    // Retains only the admission event, never image storage or a runtime.
    class Availability final {
       public:
        Availability() noexcept = default;
        [[nodiscard]] bool Wait(std::stop_token = {}) const;
        void Notify() const noexcept;

       private:
        explicit Availability(std::shared_ptr<Admission>) noexcept;
        std::shared_ptr<Admission> admission_;
        std::uint64_t epoch_ = 0U;
        friend class ImageProductPool;
    };
    struct Facts final {
        std::uint64_t revision = 0U;
        std::uint32_t capacity_width = 0U;
        std::uint32_t capacity_height = 0U;
        std::size_t staging_capacity_bytes = 0U;
    };
    class Product final {
       public:
        Product() noexcept;
        ~Product();
        Product(const Product&);
        Product& operator=(const Product&);
        Product(Product&&) noexcept;
        Product& operator=(Product&&) noexcept;
        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] BorrowedImageProductReadView Borrow() const;
        [[nodiscard]] BorrowedImageWorkspace BorrowWorkspace() const;
        [[nodiscard]] ImageWorkspaceObservation ObserveWorkspace() const;

       private:
        Product(std::shared_ptr<Slot>, std::uint64_t) noexcept;
        void Retain();
        void Release() noexcept;
        [[nodiscard]] std::unique_lock<std::mutex> LockWorkspace() const;
        std::shared_ptr<Slot> slot_;
        std::uint64_t revision_ = 0U;
        friend class ImageProductPool;
    };
    // CLEANUP-OFF: Candidate is a move-only reservation with rollback custody; Product is a copyable retained
    // completion handle. Their conventional special-member surface must not be unified behind a false base.
    class Candidate final {
       public:
        Candidate() noexcept = default;
        ~Candidate();
        Candidate(const Candidate&) = delete;
        Candidate& operator=(const Candidate&) = delete;
        Candidate(Candidate&&) noexcept;
        Candidate& operator=(Candidate&&) noexcept;
        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] std::array<ImageAllocation, 2U> allocations() const;
        [[nodiscard]] ImageWorkspaceObservation ObserveWorkspace() const;

       private:
        Candidate(std::shared_ptr<Slot>, Product, ImagePlanePreservation) noexcept;
        void Release() noexcept;
        std::shared_ptr<Slot> slot_;
        Product baseline_;
        ImagePlanePreservation preservation_ = ImagePlanePreservation::All;
        std::uint64_t revision_ = 0U;
        friend class ImageProductPool;
    };
    // CLEANUP-ON
    ImageProductPool(DeviceContext, ImageProductLayout, std::size_t, std::shared_ptr<ImageProductRetirement> = {});
    ~ImageProductPool();
    ImageProductPool(const ImageProductPool&) = delete;
    ImageProductPool& operator=(const ImageProductPool&) = delete;
    // Moving the sole completed handle permits bounded in-place replacement
    // when the other display role is held. Unselected writable storage is
    // preferred, and candidates retain their exact baseline.
    // Clean preservation requires the publication callback to replace semantics.
    [[nodiscard]] Candidate Acquire(std::stop_token = {}, Product baseline = {}, ImagePlanePreservation = ImagePlanePreservation::All);
    // Transfer the baseline only on success. A failed try retains custody without
    // emitting a spurious availability notification from a temporary Product.
    [[nodiscard]] Candidate TryAcquire(Product& baseline, ImagePlanePreservation = ImagePlanePreservation::All);
    void Publish(ImageStream&, Candidate&, std::uint32_t, std::uint32_t, std::uint64_t, ImageProductBuffer::ProductSubmit);
    // Writes only this candidate's existing storage. The callback receives
    // exact post-growth allocation facts and initializes newly acquired regions.
    void PublishRetained(ImageStream&, Candidate&, std::uint32_t, std::uint32_t, std::uint64_t, ImageProductBuffer::ProductSubmit);
    [[nodiscard]] std::array<ImageCopyPath, 2U> CopyFrom(ImageStream&, BorrowedImageProductReadView, std::uint64_t);
    Product Commit(Candidate&&);
    [[nodiscard]] bool PrepareDisplay(ImageStream&, std::uint64_t, const std::shared_ptr<ImageWorkspace>&, ImageWorkspaceFinalize);
    [[nodiscard]] bool DetachDisplay(ImageStream&, const std::shared_ptr<ImageWorkspace>&);
    // Late admission fills an unpublished display allocation from retained raw
    // pixels; the completed product revision and raw plane addresses stay intact.
    [[nodiscard]] BorrowedImageWorkspace BorrowWorkspace() const;
    void FinalizeWorkspace(Candidate&, ImageWorkspaceCoverage);
    [[nodiscard]] ImageStreamSettlement SettleWorkspaces() noexcept;
    // Existing shared product/plane leases own delayed release. Counted
    // completion objects retain those leases beyond their GPU callback.
    void ReleaseForRetirement() noexcept;
    void Select(const Product&);
    [[nodiscard]] Product Selected() const;
    [[nodiscard]] Availability ObserveAvailability() const noexcept;
    [[nodiscard]] Facts SelectedFacts() const;
    [[nodiscard]] ImageStorageFootprint StorageFootprint() const noexcept;
    [[nodiscard]] BorrowedImageProductReadView Borrow() const;
    [[nodiscard]] ImageWorkspaceObservation ObserveWorkspace() const;
    // Wake-only; receiver completion may notify from a GPU host callback.
    void SetAvailabilitySink(std::function<void()>);
    [[nodiscard]] std::size_t size() const noexcept;

   private:
    [[nodiscard]] bool PrepareWorkspace(const Product&, std::shared_ptr<ImageWorkspace>, ImageWorkspaceFinalize);
    void ValidateBaseline(const Product&) const;
    std::shared_ptr<Admission> admission_;
    std::vector<std::shared_ptr<Slot>> slots_;
};

}  // namespace mmltk::frameworks::gpu
