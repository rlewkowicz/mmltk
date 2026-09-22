#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
namespace mmltk::backend::ml::runtime {
namespace {
[[nodiscard]] constexpr std::size_t element_bytes(const RuntimeElementType type) noexcept {
 switch (type) {
  case RuntimeElementType::Float16: return 2U;
  case RuntimeElementType::Float32:
  case RuntimeElementType::Int32: return 4U;
  case RuntimeElementType::Int64: return 8U;
  case RuntimeElementType::Bool: return 1U;
 }
 return 0U;
}
}  // namespace
std::size_t validate_runtime_tensor_buffer(const RuntimeTensorDescriptor& descriptor, const RuntimeTensorBuffer& buffer, const RuntimeShape* resolved_shape) {
 if (descriptor.name.empty() || buffer.device_data == nullptr || descriptor.element_type != buffer.element_type || descriptor.shape.rank > kMaximumRuntimeRank ||
     buffer.shape.rank > kMaximumRuntimeRank || descriptor.shape.rank != buffer.shape.rank) {
  throw std::invalid_argument("invalid runtime tensor descriptor");
 }
 const RuntimeShape& actual = resolved_shape == nullptr ? buffer.shape : *resolved_shape;
 if (actual.rank > kMaximumRuntimeRank || actual.rank != descriptor.shape.rank || actual.rank != buffer.shape.rank) { throw std::invalid_argument("runtime tensor rank mismatch"); }
 std::size_t elements = 1U;
 for (std::size_t axis = 0U; axis < actual.rank; ++axis) {
  const std::int64_t declared = descriptor.shape.extents[axis];
  const std::int64_t supplied = buffer.shape.extents[axis];
  const std::int64_t resolved = actual.extents[axis];
  if (declared == 0 || declared < -1 || supplied <= 0 || resolved <= 0 || (declared > 0 && supplied != declared) || (declared > 0 && resolved != declared) ||
      (resolved_shape == nullptr && declared == -1 && supplied != resolved)) {
   throw std::invalid_argument("runtime tensor extent mismatch");
  }
  const auto extent = static_cast<std::size_t>(resolved);
  if (extent > std::numeric_limits<std::size_t>::max() / elements) { throw std::overflow_error("runtime tensor element count overflow"); }
  elements *= extent;
 }
 const std::size_t scalar_bytes = element_bytes(buffer.element_type);
 if (scalar_bytes == 0U || elements > std::numeric_limits<std::size_t>::max() / scalar_bytes) { throw std::overflow_error("runtime tensor byte count overflow"); }
 const std::size_t required = elements * scalar_bytes;
 if (buffer.capacity_bytes < required) { throw std::invalid_argument("runtime tensor capacity is too small"); }
 return required;
}
}  // namespace mmltk::backend::ml::runtime
