#pragma once
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::backend::data {
struct NativePlacementOptions {
    [[= mmltk::frameworks::reflection::Minimum<int>{-1}]] int numa_node = -1;
    bool operator==(const NativePlacementOptions&) const = default;
};
struct DataLoadingOptions : NativePlacementOptions {
    bool h2d_dataloader = true;
    bool operator==(const DataLoadingOptions&) const = default;
};
[[nodiscard]] constexpr DataLoadingOptions data_loading_options(const bool h2d_dataloader, const int numa_node = -1) noexcept {
    DataLoadingOptions options{};
    options.numa_node = numa_node;
    options.h2d_dataloader = h2d_dataloader;
    return options;
}
MMLTK_REFLECT_FIELDS(NativePlacementOptions)
MMLTK_REFLECT_FIELDS(DataLoadingOptions)
}  // namespace mmltk::backend::data
