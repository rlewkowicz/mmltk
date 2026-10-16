#include "archive_cbor.h"
#include <cstdint>
#include <cstring>

namespace mmltk::backend::ml::serialization::archive_cbor_detail {
torch::Tensor admit_tensor(torch::Tensor tensor, std::size_t capacity, CborTensorPolicy policy, const char* error) {
 if (!tensor.defined() || !tensor.is_cpu() || tensor.scalar_type() != torch::kUInt8 || tensor.dim() != 1 || tensor.numel() < (policy.allow_empty ? 0 : 1) ||
     static_cast<std::uint64_t>(tensor.numel()) > capacity || (policy.require_contiguous && !tensor.is_contiguous()))
  throw std::runtime_error(error);
 return tensor.contiguous();
}
void write_bytes(torch::serialize::OutputArchive& archive, const char* key, std::span<const std::byte> bytes) {
 auto tensor = torch::empty({static_cast<int64_t>(bytes.size())}, torch::TensorOptions().dtype(torch::kUInt8).device(torch::kCPU));
 std::memcpy(tensor.data_ptr(), bytes.data(), bytes.size());
 archive.write(key, tensor);
}
}  // namespace mmltk::backend::ml::serialization::archive_cbor_detail
