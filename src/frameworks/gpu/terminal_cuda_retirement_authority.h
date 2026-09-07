#pragma once

#include <cuda_runtime_api.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace mmltk::frameworks::gpu {

struct TerminalCudaRetirementFact final {
    bool terminal = false;
    std::size_t occupancy = 0U;
    std::size_t reservations = 0U;
    cudaError_t first_failure = cudaSuccess;
};

class TerminalCudaCustody final {
   public:
    TerminalCudaCustody() noexcept = default;
    TerminalCudaCustody(const TerminalCudaCustody&) = delete;
    TerminalCudaCustody& operator=(const TerminalCudaCustody&) = delete;

    TerminalCudaCustody(TerminalCudaCustody&& other) noexcept { Take(other); }

    TerminalCudaCustody& operator=(TerminalCudaCustody&& other) noexcept {
        if (this != &other) {
            Reset();
            Take(other);
        }
        return *this;
    }

    ~TerminalCudaCustody() noexcept { Reset(); }

    template <class T>
    [[nodiscard]] static TerminalCudaCustody Share(std::shared_ptr<T>&& owner) noexcept {
        using SharedOwner = std::shared_ptr<T>;
        static_assert(sizeof(SharedOwner) <= kStorageBytes);
        static_assert(alignof(SharedOwner) <= kStorageAlignment);
        if (!owner) std::terminate();

        TerminalCudaCustody custody;
        std::construct_at(reinterpret_cast<SharedOwner*>(custody.storage_.data()), std::move(owner));
        custody.move_ = [](void* destination, void* source) noexcept {
            auto* const typed_source = std::launder(reinterpret_cast<SharedOwner*>(source));
            std::construct_at(reinterpret_cast<SharedOwner*>(destination), std::move(*typed_source));
            std::destroy_at(typed_source);
        };
        custody.destroy_ = [](void* storage) noexcept { std::destroy_at(std::launder(reinterpret_cast<SharedOwner*>(storage))); };
        return custody;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return destroy_ != nullptr; }

   private:
    static constexpr std::size_t kStorageBytes = sizeof(std::shared_ptr<void>);
    static constexpr std::size_t kStorageAlignment = alignof(std::shared_ptr<void>);
    using MoveOperation = void (*)(void*, void*) noexcept;
    using DestroyOperation = void (*)(void*) noexcept;

    void Take(TerminalCudaCustody& other) noexcept {
        if (!other) return;
        const MoveOperation move = other.move_;
        const DestroyOperation destroy = other.destroy_;
        move(storage_.data(), other.storage_.data());
        move_ = move;
        destroy_ = destroy;
        other.move_ = nullptr;
        other.destroy_ = nullptr;
    }

    void Reset() noexcept {
        if (destroy_ != nullptr) destroy_(storage_.data());
        move_ = nullptr;
        destroy_ = nullptr;
    }

    alignas(kStorageAlignment) std::array<std::byte, kStorageBytes> storage_{};
    MoveOperation move_ = nullptr;
    DestroyOperation destroy_ = nullptr;
};

class TerminalCudaRetirementAuthority;

class TerminalCudaRetirementLease final {
   public:
    TerminalCudaRetirementLease() noexcept = default;
    TerminalCudaRetirementLease(const TerminalCudaRetirementLease&) = delete;
    TerminalCudaRetirementLease& operator=(const TerminalCudaRetirementLease&) = delete;
    TerminalCudaRetirementLease(TerminalCudaRetirementLease&&) noexcept;
    TerminalCudaRetirementLease& operator=(TerminalCudaRetirementLease&&) noexcept;
    ~TerminalCudaRetirementLease() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
    void Install(TerminalCudaCustody&& custody, cudaError_t failure) && noexcept;

   private:
    TerminalCudaRetirementLease(TerminalCudaRetirementAuthority& owner, std::size_t slot, std::uint64_t generation) noexcept
        : owner_(&owner), slot_(slot), generation_(generation) {}
    void release() noexcept;

    TerminalCudaRetirementAuthority* owner_ = nullptr;
    std::size_t slot_ = 0U;
    std::uint64_t generation_ = 0U;
    friend class TerminalCudaRetirementAuthority;
};

// Narrow construction-time authority seen by physical resource owners.
// Concrete storage retains a caller-supplied fixed capacity.
class TerminalCudaRetirementAuthority {
   public:
    TerminalCudaRetirementAuthority() = default;
    virtual ~TerminalCudaRetirementAuthority() = default;
    TerminalCudaRetirementAuthority(const TerminalCudaRetirementAuthority&) = delete;
    TerminalCudaRetirementAuthority& operator=(const TerminalCudaRetirementAuthority&) = delete;

    [[nodiscard]] virtual bool admission_open() const noexcept = 0;
    [[nodiscard]] virtual std::optional<TerminalCudaRetirementLease> Reserve() noexcept = 0;
    [[nodiscard]] virtual TerminalCudaRetirementFact fact() const noexcept = 0;

   protected:
    [[nodiscard]] static TerminalCudaRetirementLease MakeLease(TerminalCudaRetirementAuthority& owner, const std::size_t slot,
                                                               const std::uint64_t generation) noexcept {
        return TerminalCudaRetirementLease{owner, slot, generation};
    }
    virtual void Release(std::size_t, std::uint64_t) noexcept = 0;
    virtual void Install(std::size_t, std::uint64_t, TerminalCudaCustody&&, cudaError_t) noexcept = 0;

    friend class TerminalCudaRetirementLease;
};

inline TerminalCudaRetirementLease::TerminalCudaRetirementLease(TerminalCudaRetirementLease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), slot_(other.slot_), generation_(other.generation_) {}

inline TerminalCudaRetirementLease& TerminalCudaRetirementLease::operator=(TerminalCudaRetirementLease&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        slot_ = other.slot_;
        generation_ = other.generation_;
    }
    return *this;
}

inline TerminalCudaRetirementLease::~TerminalCudaRetirementLease() noexcept { release(); }

inline void TerminalCudaRetirementLease::release() noexcept {
    if (owner_ == nullptr) return;
    owner_->Release(slot_, generation_);
    owner_ = nullptr;
}

inline void TerminalCudaRetirementLease::Install(TerminalCudaCustody&& custody, const cudaError_t failure) && noexcept {
    if (owner_ == nullptr || !custody) std::terminate();
    TerminalCudaRetirementAuthority* const owner = std::exchange(owner_, nullptr);
    owner->Install(slot_, generation_, std::move(custody), failure);
}

[[nodiscard]] inline TerminalCudaRetirementLease ReserveTerminalCudaLease(TerminalCudaRetirementAuthority& owner) {
    auto lease = owner.Reserve();
    if (!lease.has_value()) throw std::runtime_error("terminal CUDA custody reservation refused before resource allocation");
    return std::move(*lease);
}

}  // namespace mmltk::frameworks::gpu
