#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <torch/types.h>
namespace mmltk::backend::models::rfdetr {
// Only the value schema is reflected. Tensor storage, reduction and lifetime
// remain with the ordinary caller; no runtime field inventory is needed.
template <class Values>
struct ReflectedTensorPacket {
 inline static constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^Values, std::meta::access_context::current()));
 inline static constexpr std::size_t size = members.size();
 using Tensors = std::array<torch::Tensor, size>;
 template <std::meta::info Member>
 static consteval std::size_t index() {
  for (std::size_t i = 0; i < size; ++i)
   if (members[i] == Member) return i;
  throw "unknown tensor packet member";
 }
 template <std::meta::info Member>
 static const torch::Tensor& get(const Tensors& values) { return values[index<Member>()]; }
 template <std::meta::info Member>
 static void set(Tensors& values, const torch::Tensor& tensor) {
  if (tensor.defined()) values[index<Member>()] = tensor.detach();
 }
 template <std::meta::info Member>
 static torch::Tensor select(const torch::Tensor& values, std::int64_t dimension) { return values.select(dimension, static_cast<std::int64_t>(index<Member>())); }
};
}  // namespace mmltk::backend::models::rfdetr
