#pragma once
#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>
#include <torch/types.h>
#include <torch/serialize.h>
#include "src/frameworks/serialization/reflected_cbor.h"
namespace mmltk::backend::ml::serialization {
struct CborArchiveBudget final {
 std::size_t max_items;
 std::size_t max_depth;
};
struct CborTensorPolicy final {
 bool require_contiguous = false;
 // Owners distinguish an empty envelope from malformed encoded content.
 bool allow_empty = false;
};
template <class T>
[[nodiscard]] constexpr mmltk::frameworks::serialization::wire::Limits cbor_archive_limits(CborArchiveBudget budget) {
 return {.max_bytes = mmltk::frameworks::serialization::reflected_maximum_cbor_bytes<T>(), .max_items = budget.max_items, .max_depth = budget.max_depth};
}
namespace archive_cbor_detail {
[[nodiscard]] torch::Tensor admit_tensor(torch::Tensor tensor, std::size_t capacity, CborTensorPolicy policy, const char* error);
void write_bytes(torch::serialize::OutputArchive& archive, const char* key, std::span<const std::byte> bytes);
}  // namespace archive_cbor_detail
template <class T>
void write_cbor_record(torch::serialize::OutputArchive& archive, const char* key, const T& value, CborArchiveBudget budget, const char* error) {
 mmltk::frameworks::serialization::wire::ByteBuffer bytes;
 if (!mmltk::frameworks::serialization::encode(value, bytes, cbor_archive_limits<T>(budget))) throw std::runtime_error(error);
 archive_cbor_detail::write_bytes(archive, key, bytes);
}
// Migration readers retain this owning tensor throughout byte-view use.
template <class T>
[[nodiscard]] torch::Tensor admit_cbor_tensor(torch::Tensor tensor, const char* error, CborTensorPolicy policy = {}) {
 return archive_cbor_detail::admit_tensor(std::move(tensor), mmltk::frameworks::serialization::reflected_maximum_cbor_bytes<T>(), policy, error);
}
template <class T>
[[nodiscard]] T read_cbor_record(torch::Tensor tensor, CborArchiveBudget budget, const char* tensor_error, const char* decode_error, CborTensorPolicy policy = {}) {
 const auto storage = admit_cbor_tensor<T>(std::move(tensor), tensor_error, policy);
 const auto bytes = std::span(reinterpret_cast<const std::byte*>(storage.const_data_ptr()), static_cast<std::size_t>(storage.numel()));
 auto value = mmltk::frameworks::serialization::decode<T>({bytes, {}}, cbor_archive_limits<T>(budget));
 if (!value) throw std::runtime_error(decode_error);
 return std::move(*value);
}
}  // namespace mmltk::backend::ml::serialization
