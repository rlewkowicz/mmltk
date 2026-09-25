#pragma once
#include <cstdint>
#include <stdexcept>
#include <torch/types.h>
namespace mmltk::backend::models::rfdetr {
// Counter samples depend on semantic row identity and coordinate, never on tensor
// packing, padding, a generator cursor, device rank or physical worker identity.
[[nodiscard]] inline torch::Tensor semantic_uniform(const torch::Tensor& identities, std::int64_t columns, std::uint64_t purpose) {
 if (identities.scalar_type() != torch::kInt64 || columns < 0) throw std::invalid_argument("invalid semantic sample shape");
 constexpr std::int64_t mask = 0x7fffffff;
 auto rows = torch::bitwise_xor(torch::bitwise_and(identities, mask), torch::bitwise_and(torch::bitwise_right_shift(identities, 32), mask));
 auto counters = torch::arange(columns, identities.options());
 auto value = torch::bitwise_xor(rows.unsqueeze(-1), torch::bitwise_and(counters * 48271 + static_cast<std::int64_t>(purpose & mask), mask));
 for (int round = 0; round < 3; ++round) {
  value = torch::bitwise_xor(value, torch::bitwise_right_shift(value, 16));
  value = torch::bitwise_and(value * 1103515245 + 12345, mask);
 }
 // Removing seven low bits before conversion keeps the float result below one.
 return torch::bitwise_right_shift(value, 7).to(torch::kFloat32) * 0x1.0p-24;
}
[[nodiscard]] inline torch::Tensor semantic_coordinates(std::uint64_t key, std::int64_t count, std::uint64_t purpose, const torch::Device& device) {
 auto identity = torch::full({1}, static_cast<std::int64_t>(key & 0x7fffffffffffffffULL), torch::TensorOptions().dtype(torch::kInt64).device(device));
 return semantic_uniform(identity, count * 2, purpose).reshape({1, count, 2});
}
}
