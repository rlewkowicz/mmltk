#pragma once
#include <torch/types.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
namespace mmltk::backend::models::rfdetr {
enum class CriterionDomain : std::uint8_t { Detection, Masks, Keypoints, Statistic };
struct CriterionTermPolicy {
 std::string_view external_name;
 std::string_view coefficient;
 CriterionDomain domain = CriterionDomain::Detection;
};
struct CriterionTermDeclaration final {
 const char* external_name;
 const char* coefficient;
 CriterionDomain domain;
 consteval CriterionTermDeclaration(std::string_view name, std::string_view coefficient_name, CriterionDomain term_domain = CriterionDomain::Detection)
     : external_name(std::define_static_string(name)), coefficient(std::define_static_string(coefficient_name)), domain(term_domain) {}
};
// Declaration order is the stock coefficient order. Identifiers are the native
// metric vocabulary; upstream spellings and coefficient aliases live here alone.
enum class CriterionTerm : std::uint8_t {
 classification[[= CriterionTermDeclaration{"loss_ce", "cls_loss_coef"}]],
 l1[[= CriterionTermDeclaration{"loss_bbox", "bbox_loss_coef"}]],
 giou[[= CriterionTermDeclaration{"loss_giou", "giou_loss_coef"}]],
 mask_ce[[= CriterionTermDeclaration{"loss_mask_ce", "mask_ce_loss_coef", CriterionDomain::Masks}]],
 mask_dice[[= CriterionTermDeclaration{"loss_mask_dice", "mask_dice_loss_coef", CriterionDomain::Masks}]],
 keypoint_l1[[= CriterionTermDeclaration{"loss_keypoints_l1", "keypoint_l1_loss_coef", CriterionDomain::Keypoints}]],
 keypoint_findable[[= CriterionTermDeclaration{"loss_keypoints_findable", "keypoint_findable_loss_coef", CriterionDomain::Keypoints}]],
 keypoint_visible[[= CriterionTermDeclaration{"loss_keypoints_visible", "keypoint_visible_loss_coef", CriterionDomain::Keypoints}]],
 keypoint_nll[[= CriterionTermDeclaration{"loss_keypoints_nll", "keypoint_nll_loss_coef", CriterionDomain::Keypoints}]],
 class_error[[= CriterionTermDeclaration{"class_error", "", CriterionDomain::Statistic}]],
 cardinality_error[[= CriterionTermDeclaration{"cardinality_error", "", CriterionDomain::Statistic}]],
};
struct CriterionSchema final {
 inline static constexpr auto members = std::define_static_array(std::meta::enumerators_of(^^CriterionTerm));
 inline static constexpr std::size_t size = members.size();
 template <std::meta::info Member>
 static consteval CriterionTermPolicy policy() {
  constexpr auto annotations = std::define_static_array(std::meta::annotations_of(Member));
  static_assert(annotations.size() == 1);
  constexpr auto declaration = std::meta::extract<CriterionTermDeclaration>(annotations[0]);
  constexpr CriterionTermPolicy result{declaration.external_name, declaration.coefficient, declaration.domain};
  static_assert(!result.external_name.empty());
  static_assert(result.coefficient.empty() == (result.domain == CriterionDomain::Statistic));
  return result;
 }
 template <class Owner>
 static consteval std::meta::info named_member(std::string_view name) {
  for (const auto member : std::meta::nonstatic_data_members_of(^^Owner, std::meta::access_context::current()))
   if (std::meta::identifier_of(member) == name) return member;
  throw "criterion term has no matching destination member";
 }
 template <class Visitor>
 static void visit(Visitor&& visitor) {
  template for (constexpr auto member : members) {
   constexpr auto term = std::meta::extract<CriterionTerm>(member);
   visitor.template operator()<term>(policy<member>());
  }
 }
 template <class Coefficients, class Visitor>
 static void visit_coefficients(Visitor&& visitor) {
  template for (constexpr auto member : members) {
   constexpr auto facts = policy<member>();
   if constexpr (facts.domain != CriterionDomain::Statistic) {
    constexpr auto coefficient = named_member<Coefficients>(facts.coefficient);
    static_assert(std::is_same_v<typename[:std::meta::type_of(coefficient):], double>);
    visitor.template operator()<std::meta::extract<CriterionTerm>(member), coefficient>(facts.domain);
   }
  }
 }
 template <class Metrics, class Visitor>
 static void visit_metrics(Visitor&& visitor) {
  template for (constexpr auto member : members) {
   constexpr auto metric = named_member<Metrics>(std::meta::identifier_of(member));
   static_assert(std::is_same_v<typename[:std::meta::type_of(metric):], std::optional<double>>);
   visitor.template operator()<std::meta::extract<CriterionTerm>(member), metric>();
  }
 }
};
static_assert([]<class Schema>() consteval {
 std::size_t index = 0;
 template for (constexpr auto member : Schema::members) {
  if (static_cast<std::size_t>(std::meta::extract<CriterionTerm>(member)) != index++) return false;
  constexpr auto facts = Schema::template policy<member>();
  template for (constexpr auto other : Schema::members) {
   if constexpr (member != other) {
    constexpr auto other_name = Schema::template policy<other>().external_name;
    if (facts.external_name == other_name) return false;
    // A main spelling must not collide with another term's external layer
    // projection. This is declaration validation, never runtime dispatch.
    if (other_name.starts_with(facts.external_name)) {
     const auto suffix = other_name.substr(facts.external_name.size());
     if (suffix == "_enc") return false;
     if (suffix.size() > 1 && suffix.front() == '_') {
      bool numeric = true;
      for (const char value : suffix.substr(1)) numeric = numeric && value >= '0' && value <= '9';
      if (numeric) return false;
     }
    }
   }
  }
 }
 return true;
}.template operator()<CriterionSchema>());
class CriterionLayer final {
public:
 enum class Kind : std::uint8_t { Main, Auxiliary, Encoder };
 constexpr CriterionLayer() = default;
 [[nodiscard]] static constexpr CriterionLayer auxiliary(std::size_t index) { return {Kind::Auxiliary, index}; }
 [[nodiscard]] static constexpr CriterionLayer encoder() { return {Kind::Encoder, 0}; }
 [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
 [[nodiscard]] constexpr std::size_t index() const noexcept { return index_; }
 bool operator==(const CriterionLayer&) const = default;

private:
 constexpr CriterionLayer(Kind kind, std::size_t index) : kind_(kind), index_(index) {}
 Kind kind_ = Kind::Main;
 std::size_t index_ = 0;
};
struct CriterionKey final {
 CriterionTerm term;
 CriterionLayer layer{};
 bool operator==(const CriterionKey&) const = default;
};
struct CriterionWeight final {
 CriterionKey key;
 double coefficient = 0;
};
// Unlike the detached telemetry packet, these slots retain the original tensor
// handles and their autograd graphs. Undefined slots mean absent terms.
class CriterionTensors final {
public:
 template <class Self>
 auto& operator[](this Self&& self, CriterionTerm term) {
  return self.values_.at(static_cast<std::size_t>(term));
 }
 template <class Self, class Visitor>
 void for_each(this Self&& self, Visitor&& visitor) {
  CriterionSchema::visit([&]<CriterionTerm Term>(CriterionTermPolicy) {
   auto& value = self[Term];
   if (value.defined()) visitor(Term, value);
  });
 }

private:
 std::array<torch::Tensor, CriterionSchema::size> values_;
};
struct CriterionResult final {
 CriterionTensors main;
 std::vector<CriterionTensors> auxiliary;
 std::optional<CriterionTensors> encoder;
 template <class Self, class Visitor>
 void for_each(this Self&& self, Visitor&& visitor) {
  const auto visit_layer = [&](auto& terms, CriterionLayer layer) { terms.for_each([&](CriterionTerm term, auto& value) { visitor(CriterionKey{term, layer}, value); }); };
  visit_layer(self.main, {});
  for (std::size_t i = 0; i < self.auxiliary.size(); ++i) visit_layer(self.auxiliary[i], CriterionLayer::auxiliary(i));
  if (self.encoder) visit_layer(*self.encoder, CriterionLayer::encoder());
 }
 [[nodiscard]] const torch::Tensor* find(CriterionKey key) const {
  const CriterionTensors* terms = nullptr;
  switch (key.layer.kind()) {
   case CriterionLayer::Kind::Main: terms = &main; break;
   case CriterionLayer::Kind::Auxiliary:
    if (key.layer.index() < auxiliary.size()) terms = &auxiliary[key.layer.index()];
    break;
   case CriterionLayer::Kind::Encoder:
    if (encoder) terms = &*encoder;
    break;
  }
  if (!terms) return nullptr;
  const auto& value = (*terms)[key.term];
  return value.defined() ? &value : nullptr;
 }
 [[nodiscard]] bool contains(CriterionKey key) const { return find(key) != nullptr; }
 [[nodiscard]] const torch::Tensor& at(CriterionKey key) const {
  if (const auto* value = find(key)) return *value;
  throw std::out_of_range("absent RF-DETR criterion term");
 }
 [[nodiscard]] std::size_t size() const {
  std::size_t count = 0;
  for_each([&](CriterionKey, const torch::Tensor&) { ++count; });
  return count;
 }
};
// Names are an external projection only; numerical order and schema comparison
// use typed keys. No runtime registry or suffix parser participates in dispatch.
[[nodiscard]] inline std::string criterion_external_name(CriterionKey key) {
 std::string name;
 CriterionSchema::visit([&]<CriterionTerm Term>(CriterionTermPolicy policy) {
  if (key.term == Term) name = policy.external_name;
 });
 if (name.empty()) throw std::invalid_argument("unknown RF-DETR criterion term");
 switch (key.layer.kind()) {
  case CriterionLayer::Kind::Main: break;
  case CriterionLayer::Kind::Auxiliary: name += "_" + std::to_string(key.layer.index()); break;
  case CriterionLayer::Kind::Encoder: name += "_enc"; break;
 }
 return name;
}
}  // namespace mmltk::backend::models::rfdetr
