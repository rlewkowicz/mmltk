#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <cuda_runtime_api.h>
#include "src/frameworks/gpu/image_buffer.h"
#include "src/backend/imaging/explore/detail/explore_render_cuda_abi.h"
#include <cstring>
#include <span>
#include <type_traits>
namespace mmltk::controller::explore_detail {
struct StorageSpan final {
    std::size_t offset = 0U;
    std::size_t count = 0U;
};
[[nodiscard]] constexpr std::size_t align_up(const std::size_t value, const std::size_t alignment) noexcept {
    return (value + alignment - 1U) / alignment * alignment;
}
template <class Value>
[[nodiscard]] Value load_payload(const void* const payload, const std::size_t offset) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    Value value{};
    std::memcpy(&value, static_cast<const std::byte*>(payload) + offset, sizeof(Value));
    return value;
}
template <class Value>
void store_payload(void* const payload, const std::size_t offset, const Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    std::memcpy(static_cast<std::byte*>(payload) + offset, &value, sizeof(Value));
}
template <class Value, std::size_t Extent>
void store_payload(void* const payload, const std::size_t offset, const std::span<Value, Extent> values) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    if (!values.empty()) std::memcpy(static_cast<std::byte*>(payload) + offset, values.data(), values.size_bytes());
}
inline void ensure_gallery_cuda(cudaError_t status, const char* detail) {
    if (status != cudaSuccess) throw std::runtime_error(detail);
}
[[nodiscard]] inline mmltk::backend::imaging::explore::detail::ExploreRenderTargetViewAbi gallery_target(mmltk::frameworks::gpu::ImagePlaneView target) {
    return {.data = reinterpret_cast<std::uint8_t*>(target.data),
            .pitch_bytes = target.descriptor.pitch_bytes,
            .width = target.descriptor.width,
            .height = target.descriptor.height};
}
}  // namespace mmltk::controller::explore_detail
