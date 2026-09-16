#include "src/controller/presentation/annotation_palette.h"
#include "src/backend/imaging/raster/class_palette.h"
namespace mmltk::controller {
std::vector<contracts::AnnotationColor> annotation_class_palette(const std::size_t count) {
    std::vector<contracts::AnnotationColor> palette(count);
    for (std::size_t index = 0U; index < count; ++index)
        mmltk::backend::imaging::raster::color::class_hsv(static_cast<int>(index), static_cast<int>(count), palette[index].hue,
                                                                  palette[index].saturation, palette[index].value);
    return palette;
}
}  // namespace mmltk::controller
