#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace mmltk::backend::imaging::explore {

using ExploreStorageStatus = std::int32_t;
inline constexpr ExploreStorageStatus kExploreStorageSuccess = 0;

struct ExploreCudaAllocationApi final {
    void* context = nullptr;
    ExploreStorageStatus (*allocate_device)(void*, void**, std::size_t) noexcept = nullptr;
    ExploreStorageStatus (*release_device)(void*, void*) noexcept = nullptr;
    ExploreStorageStatus (*allocate_pinned)(void*, void**, std::size_t) noexcept = nullptr;
    ExploreStorageStatus (*release_pinned)(void*, void*) noexcept = nullptr;

    [[nodiscard]] bool valid() const noexcept {
        return allocate_device != nullptr && release_device != nullptr && allocate_pinned != nullptr && release_pinned != nullptr;
    }
};

enum class ExploreBufferMemory : std::uint8_t {
    PinnedHost,
    Device,
};

class ExploreHighWaterBuffer final {
   public:
    explicit ExploreHighWaterBuffer(ExploreBufferMemory memory = ExploreBufferMemory::Device);
    ~ExploreHighWaterBuffer();
    ExploreHighWaterBuffer(const ExploreHighWaterBuffer&) = delete;
    ExploreHighWaterBuffer& operator=(const ExploreHighWaterBuffer&) = delete;
    ExploreHighWaterBuffer(ExploreHighWaterBuffer&&) = delete;
    ExploreHighWaterBuffer& operator=(ExploreHighWaterBuffer&&) = delete;

    void bind(ExploreCudaAllocationApi api) noexcept;
    [[nodiscard]] bool ensure_bytes(std::size_t required) noexcept;
    [[nodiscard]] ExploreStorageStatus reset() noexcept;
    [[nodiscard]] void* data() noexcept;
    [[nodiscard]] const void* data() const noexcept;
    [[nodiscard]] std::size_t capacity_bytes() const noexcept;
    [[nodiscard]] ExploreStorageStatus last_failure() const noexcept;
    [[nodiscard]] bool owns_allocation() const noexcept;

   private:
    struct Owner;
    std::unique_ptr<Owner> owner_;
};

}  // namespace mmltk::backend::imaging::explore
