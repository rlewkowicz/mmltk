#pragma once

#include <cuda.h>

#include <cstddef>
#include <cstdint>
#include <exception>

namespace mmltk::frameworks::gpu {

struct ImageStreamSettlement final {
    bool completion_reached = false;
    std::exception_ptr failure{};
};

struct ImageStorageFootprint final {
    std::size_t device_bytes = 0U;
    std::size_t pinned_bytes = 0U;
};

[[nodiscard]] std::uint64_t next_image_allocation_identity();

enum class ImageFormat : std::uint8_t { Rgba8 };
enum class ImagePlaneKind : std::uint8_t { Clean, Semantic };
enum class ImagePlanePreservation : std::uint8_t { All, Clean };

struct ImagePlaneDescriptor final {
    ImagePlaneKind kind = ImagePlaneKind::Clean;
    ImageFormat format = ImageFormat::Rgba8;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::size_t pitch_bytes = 0U;

    [[nodiscard]] constexpr std::size_t row_bytes() const noexcept { return static_cast<std::size_t>(width) * 4U; }
    [[nodiscard]] constexpr bool valid() const noexcept {
        return (kind == ImagePlaneKind::Clean || kind == ImagePlaneKind::Semantic) && format == ImageFormat::Rgba8 && width != 0U &&
               height != 0U && pitch_bytes >= row_bytes();
    }
    constexpr bool operator==(const ImagePlaneDescriptor&) const noexcept = default;
};

struct ImageAllocation final {
    std::uint64_t identity = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint64_t owner = 0U;
    constexpr bool operator==(const ImageAllocation&) const noexcept = default;
};

struct ImagePlaneView final {
    CUdeviceptr data = 0U;
    ImagePlaneDescriptor descriptor{};
    // Physical storage identity survives logical extent/revision changes and
    // changes even when an allocator reuses the same device address.
    ImageAllocation allocation{};
    [[nodiscard]] constexpr bool valid() const noexcept { return data != 0U && descriptor.valid(); }
};

enum class ImageCopyPath : std::uint8_t {
    SameDevice,
    Peer,
    PinnedStaging,
};

enum class DeviceContextMode : std::uint8_t {
    Isolated,
    PrimaryInterop,
};

}  // namespace mmltk::frameworks::gpu
