module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include "src/common/math/checked_arithmetic.h"
export module mmltk.backend.models.rfdetr.core.dataset_limit_resolution;
export namespace mmltk::backend::models::rfdetr {
struct ResolvedDatasetLimit {
    int as_int = 0;
    std::size_t as_size = 0;
    bool automatic = false;
};
[[nodiscard]] inline ResolvedDatasetLimit resolve_dataset_limit(const std::uint32_t max_instances_per_image, const std::size_t requested) {
    const bool automatic = requested == 0U;
    const std::uint64_t value = automatic ? static_cast<std::uint64_t>(max_instances_per_image) * 2U + 1U
                                          : mmltk::common::math::checked_cast<std::uint64_t>(requested, "RF-DETR dataset limit exceeds uint64_t");
    return ResolvedDatasetLimit{
        mmltk::common::math::checked_cast<int>(value, "RF-DETR dataset limit exceeds int"),
        mmltk::common::math::checked_cast<std::size_t>(value, "RF-DETR dataset limit exceeds size_t"),
        automatic,
    };
}
[[nodiscard]] inline ResolvedDatasetLimit resolve_dataset_query_limit(const std::uint32_t max_instances_per_image, const std::size_t requested,
                                                                      const std::size_t automatic_num_queries_cap) {
    if (requested != 0U) { return resolve_dataset_limit(max_instances_per_image, requested); }
    if (automatic_num_queries_cap == 0U) { throw std::invalid_argument("RF-DETR automatic query resolution requires a positive model query cap"); }
    const ResolvedDatasetLimit derived = resolve_dataset_limit(max_instances_per_image, 0U);
    const std::size_t capped = std::min(derived.as_size, automatic_num_queries_cap);
    return ResolvedDatasetLimit{
        mmltk::common::math::checked_cast<int>(capped, "RF-DETR automatic query limit exceeds int"),
        capped,
        true,
    };
}
}  // namespace mmltk::backend::models::rfdetr
