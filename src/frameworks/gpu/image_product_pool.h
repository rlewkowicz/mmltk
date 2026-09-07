#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

       private:
        Product(std::shared_ptr<Slot>, std::uint64_t) noexcept;
        void Retain();
        void Release() noexcept;
        std::shared_ptr<Slot> slot_;
        std::uint64_t revision_ = 0U;
        friend class ImageProductPool;
    };
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

       private:
        Candidate(std::shared_ptr<Slot>, Product) noexcept;
        void Release() noexcept;
        std::shared_ptr<Slot> slot_;
        Product baseline_;
        std::uint64_t revision_ = 0U;
        friend class ImageProductPool;
    };
    ImageProductPool(DeviceContext, ImageProductLayout, std::size_t);
    ~ImageProductPool();
    ImageProductPool(const ImageProductPool&) = delete;
    ImageProductPool& operator=(const ImageProductPool&) = delete;
    // Moving the sole completed handle permits bounded in-place reuse in a
    // one-slot pool. Multi-slot candidates retain their exact baseline.
    [[nodiscard]] Candidate Acquire(std::stop_token = {}, Product baseline = {});
    void Publish(ImageStream&, Candidate&, std::uint32_t, std::uint32_t, std::uint64_t, ImageProductBuffer::ProductSubmit);
    [[nodiscard]] std::array<ImageCopyPath, 2U> CopyFrom(ImageStream&, BorrowedImageProductReadView, std::uint64_t);
    Product Commit(Candidate&&);
    void Select(const Product&);
    [[nodiscard]] Product Selected() const;
    [[nodiscard]] Availability ObserveAvailability() const noexcept;
    [[nodiscard]] Facts SelectedFacts() const;
    [[nodiscard]] BorrowedImageProductReadView Borrow() const;
    void SetAvailabilitySink(std::function<void()>);
    [[nodiscard]] std::size_t size() const noexcept;

   private:
    std::shared_ptr<Admission> admission_;
    std::vector<std::shared_ptr<Slot>> slots_;
};

}  // namespace mmltk::frameworks::gpu
