#pragma once
#include <cuda_runtime.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include "src/common/math/deterministic_sampling.h"
namespace mmltk::backend::models::rfdetr {
[[nodiscard]] __host__ __device__ __forceinline__ std::uint64_t training_augmentation_image_key(const std::uint64_t seed, const int epoch, const int rank,
                                                                          const std::uint64_t sequence, const std::size_t image) noexcept {
    return mmltk::common::math::deterministic_mix64(seed ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(epoch)) << 32U) ^
                 (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rank)) * 0xd2b74407b1ce6e93ULL) ^ (sequence * 0xca5a826395121157ULL) ^
                 (static_cast<std::uint64_t>(image) * 0x9e3779b97f4a7c15ULL));
}

[[nodiscard]] inline std::uint64_t augmentation_preview_image_key(const std::uint64_t dataset_identity, const std::uint64_t preview_seed,
                                                                  const std::uint32_t compiled_index) noexcept {
    return mmltk::common::math::deterministic_mix64(dataset_identity ^ mmltk::common::math::deterministic_mix64(preview_seed) ^ (static_cast<std::uint64_t>(compiled_index) * 0xd2b74407b1ce6e93ULL));
}
[[nodiscard]] inline std::uint32_t select_augmentation_preview_donor_image(const std::span<const std::uint32_t> annotated_indices,
                                                                           const std::uint32_t source_index, const std::uint64_t image_key) noexcept {
    if (annotated_indices.empty()) { return source_index; }
    std::size_t position = static_cast<std::size_t>(mmltk::common::math::deterministic_mix64(image_key ^ 0x51ed2705ULL) % annotated_indices.size());
    if (annotated_indices[position] == source_index && annotated_indices.size() > 1U) { position = (position + 1U) % annotated_indices.size(); }
    return annotated_indices[position];
}
[[nodiscard]] inline std::size_t select_augmentation_preview_donor_instance(const std::size_t instance_count, const std::uint64_t image_key) noexcept {
    return instance_count == 0U ? 0U : static_cast<std::size_t>(mmltk::common::math::deterministic_mix64(image_key ^ 0xa0761d6478bd642fULL) % instance_count);
}
}
