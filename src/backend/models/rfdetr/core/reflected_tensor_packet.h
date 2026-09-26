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
 static const torch::Tensor& get(const Tensors& values) {
  constexpr auto slot = index<Member>();
  return values[slot];
 }
 template <std::meta::info Member>
 static void set(Tensors& values, const torch::Tensor& tensor) {
  constexpr auto slot = index<Member>();
  if (tensor.defined()) values[slot] = tensor.detach();
 }
 template <std::meta::info Member>
 static torch::Tensor select(const torch::Tensor& values, std::int64_t dimension) {
  constexpr auto slot = static_cast<std::int64_t>(index<Member>());
  return values.select(dimension, slot);
 }
 template <class Visitor>
 static void visit(Visitor&& visitor) {
  template for (constexpr auto member : members) {
   constexpr auto pointer = &[:member:];
   constexpr auto slot = index<member>();
   visitor.template operator()<pointer, slot>();
  }
 }
};
}  // namespace mmltk::backend::models::rfdetr
