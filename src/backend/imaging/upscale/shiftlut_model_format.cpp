#include "detail/shiftlut_model_format.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace mmltk::backend::imaging::upscale::shiftlut {
void validate_tables(std::span<const std::byte> bytes) {
 static_assert(std::endian::native == std::endian::little);
 static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
 if (bytes.size() != kTableBytes) throw std::invalid_argument("invalid ShiftLUT v1 table layout");
 for (std::size_t index = 0; index < kTableElements; ++index) {
  float value;
  std::memcpy(&value, bytes.data() + index * sizeof(float), sizeof(value));
  if (!std::isfinite(value) || std::abs(value) > 32767.0F) throw std::invalid_argument("invalid ShiftLUT table value");
  if (index >= table_offset(TableFamily::Shifts) && (std::abs(value) > 1.0F || std::trunc(value) != value)) throw std::invalid_argument("invalid ShiftLUT shift");
 }
}
}  // namespace mmltk::backend::imaging::upscale::shiftlut
