#pragma once
#include <cstdint>
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::backend::data {
enum class BenchmarkDatasetVariant : std::uint8_t { CocoCustom = 0, Coconut = 1 };
enum class CoconutValidation : std::uint8_t { Coconut = 0, Stock = 1, CoconutStock = 2 };
MMLTK_REFLECT_ENUM(BenchmarkDatasetVariant)
MMLTK_REFLECT_ENUM(CoconutValidation)
struct BenchmarkDatasetSelection {
    BenchmarkDatasetVariant dataset = BenchmarkDatasetVariant::CocoCustom;
    CoconutValidation validation = CoconutValidation::Coconut;
    bool operator==(const BenchmarkDatasetSelection&) const = default;
};
MMLTK_REFLECT_FIELDS(BenchmarkDatasetSelection)
[[nodiscard]] constexpr bool valid_benchmark_selection(BenchmarkDatasetSelection selection) noexcept {
    return mmltk::frameworks::reflection::enum_contains(selection.dataset) &&
           mmltk::frameworks::reflection::enum_contains(selection.validation);
}
}  // namespace mmltk::backend::data
