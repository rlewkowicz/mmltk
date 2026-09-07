#include <cstddef>
#include <meta>
#include <vector>
#include "src/backend/imaging/raster/detail/raster_color.h"

#include "src/controller/contracts/annotation.h"
#include "src/frameworks/serialization/serialization.h"

namespace mmltk::controller::contracts {
std::vector<AnnotationColor> annotation_class_palette(const std::size_t count) {
    std::vector<AnnotationColor> palette(count);
    for (std::size_t index = 0U; index < count; ++index)
        mmltk::backend::imaging::raster::detail::color::class_hsv(static_cast<int>(index), static_cast<int>(count), palette[index].hue,
                                                                  palette[index].saturation, palette[index].value);
    return palette;
}

bool encode_annotation_persistence(const AnnotationUiState& state, std::vector<std::byte>& destination) noexcept {
    constexpr mmltk::frameworks::serialization::wire::Limits limits{.max_bytes = kAnnotationUiStateByteBudget,
                                                                    .max_items = kAnnotationUiStateByteBudget};
    return mmltk::frameworks::serialization::encode(state, destination, limits).has_value();
}

}  // namespace mmltk::controller::contracts
