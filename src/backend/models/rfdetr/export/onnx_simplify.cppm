module;
#include <filesystem>
export module mmltk.backend.models.rfdetr.model_export:onnx_simplify;
export namespace mmltk::backend::models::rfdetr {
void simplify_onnx_model_file(const std::filesystem::path& model_path);
}
