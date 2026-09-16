module;
#include <cstdint>
export module mmltk.backend.imaging.annotation.semantic_scene_descriptor;
export namespace mmltk::backend::imaging::annotation {
enum class SemanticRenderer : std::uint8_t {
    Iced = 0U,
    NativeImageView = 1U,
    NativeTileAtlas = 2U,
};
[[nodiscard]] constexpr bool semantic_renderer_valid(const SemanticRenderer renderer) noexcept {
    return renderer == SemanticRenderer::Iced || renderer == SemanticRenderer::NativeImageView || renderer == SemanticRenderer::NativeTileAtlas;
}
}  // namespace mmltk::backend::imaging::annotation
