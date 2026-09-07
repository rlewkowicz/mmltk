module;
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

export module mmltk.backend.imaging.annotation.renderer;

export import mmltk.backend.imaging.annotation.core;

export namespace mmltk::backend::imaging::annotation {

[[nodiscard]] AnnotationProjectedScene build_annotation_projected_scene(const AnnotationFrame& frame, const AnnotationSceneView& document,
                                                                        std::optional<std::size_t> selected_object_index);

[[nodiscard]] AnnotationProjectedScene refresh_annotation_projected_scene_selection(const AnnotationFrame& frame,
                                                                                    const AnnotationSceneView& document,
                                                                                    AnnotationProjectedScene scene,
                                                                                    std::optional<std::size_t> selected_object_index);

[[nodiscard]] AnnotationProjectedBox project_annotation_capture_box(const AnnotationFrame& frame, const AnnotationBox& capture_box);

}  // namespace mmltk::backend::imaging::annotation
