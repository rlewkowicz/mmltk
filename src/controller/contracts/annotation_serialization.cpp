#include <cstddef>
#include <meta>
#include <vector>
#include "src/controller/contracts/annotation.h"
#include "src/frameworks/serialization/serialization.h"
namespace mmltk::controller::contracts {
bool encode_annotation_persistence(const AnnotationUiState& state, std::vector<std::byte>& destination) noexcept {
    constexpr mmltk::frameworks::serialization::wire::Limits limits{.max_bytes = kAnnotationUiStateByteBudget, .max_items = kAnnotationUiStateByteBudget};
    return mmltk::frameworks::serialization::encode(state, destination, limits).has_value();
}
}  // namespace mmltk::controller::contracts
