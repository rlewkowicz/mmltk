#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include "src/backend/models/rfdetr/contract/artifacts.h"
#include "src/backend/models/rfdetr/contract/class_layout.h"
#include "src/backend/models/rfdetr/core/class_artifact.h"
#include <stop_token>
namespace mmltk::backend::models::rfdetr::model_state_detail {
template <class T>
inline constexpr bool is_optional = false;
template <class T>
inline constexpr bool is_optional<std::optional<T>> = true;
}  // namespace mmltk::backend::models::rfdetr::model_state_detail
namespace mmltk::backend::models::rfdetr {
inline constexpr const char* kNativeCheckpointFormat = "mmltk.rfdetr.native_checkpoint";
inline constexpr int64_t kNativeCheckpointFormatVersion = 3;
struct NativeCheckpointMetadata {
    ModelClassLayout class_layout;
    std::string preset_name;
    std::string source_kind;
    std::string source_path;
    int64_t num_classes = 0;
    int64_t num_queries = 0;
    int64_t num_select = 0;
    std::optional<bool> sum_group_losses;
    std::optional<bool> use_varifocal_loss;
    std::optional<bool> use_position_supervised_loss;
    std::optional<bool> ia_bce_loss;
    std::optional<bool> aux_loss;
    std::optional<int64_t> mask_point_sample_ratio;
    std::optional<double> focal_alpha;
    std::optional<double> cls_loss_coef;
    std::optional<double> bbox_loss_coef;
    std::optional<double> giou_loss_coef;
    std::optional<double> mask_ce_loss_coef;
    std::optional<double> mask_dice_loss_coef;
    std::optional<double> set_cost_class;
    std::optional<double> set_cost_bbox;
    std::optional<double> set_cost_giou;
    template <class Self, class Visitor>
    void for_each_detection_field(this Self&& self, Visitor&& visitor) {
        template for (constexpr auto member :
                      std::define_static_array(std::meta::nonstatic_data_members_of(^^NativeCheckpointMetadata, std::meta::access_context::current()))) {
            using Field = std::remove_cvref_t<decltype(self.[:member:])>;
            if constexpr (model_state_detail::is_optional<Field>) {
                constexpr auto name = std::define_static_string(std::meta::identifier_of(member));
                visitor(name, self.[:member:]);
            }
        }
    }
};
class DecodedNativeModelState {
   public:
    DecodedNativeModelState();
    ~DecodedNativeModelState();
    DecodedNativeModelState(DecodedNativeModelState&&) noexcept;
    DecodedNativeModelState& operator=(DecodedNativeModelState&&) noexcept;
    DecodedNativeModelState(const DecodedNativeModelState&) = delete;
    DecodedNativeModelState& operator=(const DecodedNativeModelState&) = delete;
    NativeCheckpointMetadata metadata;
    std::shared_ptr<const ClassArtifactAdmission> class_artifact;
    [[nodiscard]] std::size_t tensor_count() const noexcept;
    [[nodiscard]] void* technical_handle() noexcept;
    [[nodiscard]] const void* technical_handle() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct ResolvedModelState {
    ResolvedModelArtifacts artifacts;
    DecodedNativeModelState model_state;
};
void validate_decoded_model_state(const DecodedNativeModelState& state);
[[nodiscard]] bool is_native_checkpoint_file(const std::filesystem::path& checkpoint_path);
[[nodiscard]] DecodedNativeModelState decode_native_model_state(const std::filesystem::path& checkpoint_path);
[[nodiscard]] DecodedNativeModelState decode_model_state(const std::filesystem::path& checkpoint_path,
                                                         std::shared_ptr<const ClassArtifactAdmission> admission = {},
                                                         const std::filesystem::path& class_layout_path = {}, std::stop_token stop = {});
void write_upstream_model_state(const std::filesystem::path& checkpoint_path, const DecodedNativeModelState& model_state);
[[nodiscard]] ResolvedModelState resolve_model_state(const std::filesystem::path& weights_path, std::string_view preset_name, int resolution,
                                                     const std::filesystem::path& class_layout_path = {},
                                                     std::shared_ptr<const ClassArtifactAdmission> admission = {}, std::stop_token stop = {});
[[nodiscard]] NativeCheckpointMetadata make_native_checkpoint_metadata(const ResolvedModelArtifacts& artifacts, int64_t num_classes);
}  // namespace mmltk::backend::models::rfdetr
