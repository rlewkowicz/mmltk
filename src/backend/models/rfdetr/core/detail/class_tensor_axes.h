#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
namespace mmltk::backend::models::rfdetr::detail {
enum class ClassTensorCoordinates { OutputSlots, Foreground, ForegroundWithBoxes };
struct ClassTensorAxis final {
 std::string name;
 std::int64_t dimension;
 ClassTensorCoordinates coordinates;
};
struct ClassTensorShape final {
 std::int64_t dimension, rank;
 ClassTensorCoordinates coordinates;
};
// These declarations name the actual registered modules. Both the owners and
// archive admission consume them; inspection never constructs a tensor owner.
class ClassTensorFamily final {
public:
 constexpr ClassTensorFamily(std::string_view owner, std::string_view module, std::string_view tensor, bool indexed, std::int64_t dimension,
                             ClassTensorCoordinates coordinates)
     : owner_(owner), module_(module), tensor_(tensor), indexed_(indexed), dimension_(dimension), coordinates_(coordinates) {}
 [[nodiscard]] constexpr std::string_view module_name() const noexcept { return module_; }
 [[nodiscard]] std::string prefix() const { return (owner_.empty() ? std::string{} : std::string(owner_) + ".") + std::string(module_) + "."; }
 [[nodiscard]] constexpr std::optional<ClassTensorShape> Match(std::string_view name) const noexcept {
  const auto remove = [&](std::string_view component) {
   if (!name.starts_with(component)) return false;
   name.remove_prefix(component.size());
   if (name.empty() || name.front() != '.') return false;
   name.remove_prefix(1);
   return true;
  };
  if ((!owner_.empty() && !remove(owner_)) || !remove(module_)) return std::nullopt;
  if (indexed_) {
   const auto end = name.find('.');
   if (end == std::string_view::npos || end == 0) return std::nullopt;
   for (std::size_t index = 0; index < end; ++index)
    if (name[index] < '0' || name[index] > '9') return std::nullopt;
   name.remove_prefix(end + 1);
  }
  if (!tensor_.empty()) {
   if (name != tensor_) return std::nullopt;
  } else if (name != "weight" && name != "bias")
   return std::nullopt;
  return ClassTensorShape{dimension_, name == "bias" ? 1 : 2, coordinates_};
 }

private:
 std::string_view owner_, module_, tensor_;
 bool indexed_;
 std::int64_t dimension_;
 ClassTensorCoordinates coordinates_;
};
inline constexpr std::string_view kTransformerModule = "transformer", kTrainingSupervisionModule = "training_supervision";
inline constexpr ClassTensorFamily kDecoderClassAxis{"", "class_embed", "", false, 0, ClassTensorCoordinates::OutputSlots};
inline constexpr ClassTensorFamily kEncoderClassAxis{kTransformerModule, "enc_out_class_embed", "", true, 0, ClassTensorCoordinates::OutputSlots};
inline constexpr ClassTensorFamily kDenoisingClassAxis{kTrainingSupervisionModule,        "denoising_label_embedding", "weight", false, 0,
                                                       ClassTensorCoordinates::Foreground};
inline constexpr std::string_view kProbeInputWeight = "linear1.weight";
inline constexpr std::string_view kProbeInputLayer = kProbeInputWeight.substr(0, kProbeInputWeight.find('.'));
inline constexpr ClassTensorFamily kMatchFreeClassAxis{
 kTrainingSupervisionModule, "ground_truth_mlp", kProbeInputWeight, false, 1, ClassTensorCoordinates::ForegroundWithBoxes};
[[nodiscard]] constexpr std::optional<ClassTensorShape> class_tensor_shape(std::string_view name) noexcept {
 for (const auto* family : std::array{&kDecoderClassAxis, &kEncoderClassAxis, &kDenoisingClassAxis, &kMatchFreeClassAxis})
  if (const auto axis = family->Match(name)) return axis;
 return std::nullopt;
}
}  // namespace mmltk::backend::models::rfdetr::detail
