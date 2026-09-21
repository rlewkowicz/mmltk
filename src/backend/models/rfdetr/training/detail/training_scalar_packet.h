#pragma once
#include <array>
#include <cmath>
#include <meta>
#include <limits>
#include <torch/types.h>
#include "src/backend/models/rfdetr/contract/training_metrics.h"
namespace mmltk::backend::models::rfdetr {
namespace scalar_packet {
inline constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^TrainingScalars, std::meta::access_context::current()));
inline constexpr std::size_t size = members.size();
using Tensors = std::array<torch::Tensor, size>;
template <std::meta::info Member>
consteval std::size_t index() {
 for (std::size_t i = 0; i < size; ++i)
  if (members[i] == Member) return i;
 throw "unknown training scalar";
}
template <std::meta::info Member>
void set(Tensors& values, const torch::Tensor& tensor) {
 if (tensor.defined()) values[index<Member>()] = tensor.detach();
}
inline TrainingScalars project(const float* values, double count) {
 TrainingScalars result;
 mmltk::frameworks::reflection::visit_materialized_members<TrainingScalars>([&]<class Declaration>(const auto&) {
  constexpr auto position =
   mmltk::frameworks::reflection::member_index<Declaration::pointer>(mmltk::frameworks::reflection::field_declarations<TrainingScalars>());
  const auto value = static_cast<double>(values[position]);
  if (count > 0 && std::isfinite(value)) result.*Declaration::pointer = value / count;
 });
 return result;
}
}  // namespace scalar_packet
}  // namespace mmltk::backend::models::rfdetr
