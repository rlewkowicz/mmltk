#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include "src/backend/models/rfdetr/core/postprocess.h"
#include "src/common/system/numa_memory.h"
#include "src/backend/models/rfdetr/contract/prediction_limits.h"
namespace mmltk::backend::models::rfdetr {
struct PredictionCapacity final {
 std::size_t candidates = 0U;
 std::size_t pixels = 0U;
 // One UInt8 mask, independent of discarded top-k candidates. Chunks are selected later.
 std::size_t mask_bytes = 0U;
 [[nodiscard]] static PredictionCapacity Resolve(std::size_t requested, std::size_t batch, std::int64_t queries, std::int64_t classes, std::uint32_t width, std::uint32_t height, bool masks,
  std::optional<std::size_t> eligible_classes = std::nullopt) {
  if (requested != 0 || !eligible_classes || *eligible_classes != 0) validate_prediction_candidates(requested);
  if (queries <= 0 || classes <= 0 || queries > std::numeric_limits<std::int32_t>::max() || classes > std::numeric_limits<std::int32_t>::max())
   throw std::invalid_argument("RF-DETR backend candidate shape is invalid");
  const auto available = checked_prediction_extent(static_cast<std::size_t>(queries), static_cast<std::size_t>(classes), static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
  static_cast<void>(checked_prediction_extent(batch, available, kMaximumPredictionTensorBytes / sizeof(float)));
  const auto eligible = eligible_classes.value_or(static_cast<std::size_t>(classes));
  if (eligible > static_cast<std::size_t>(classes)) throw std::invalid_argument("eligible class count exceeds physical logits width");
  const auto count = eligible == 0 ? 0U : std::min(requested, available);
  if (count) static_cast<void>(checked_prediction_extent(batch, count, kMaximumPredictionTensorBytes / (4U * sizeof(float))));
  const auto pixels = checked_prediction_extent(width, height, kMaximumEncodedMaskPixels);
  return {count, pixels, masks && count ? checked_prediction_extent(pixels, sizeof(std::uint8_t), kMaximumPredictionTensorBytes) : 0U};
 }
};
struct PredictionMaskChunk final {
 std::size_t count = 0U;
 std::size_t retained_limit = 0U;
 SelectedMaskCapacity capacity;
 [[nodiscard]] static PredictionMaskChunk Resolve(std::size_t survivors, std::size_t model_height, std::size_t model_width, std::size_t height, std::size_t width, std::size_t dtype_bytes) {
  validate_prediction_candidates(survivors);
  const auto one = SelectedMaskCapacity::Resolve(1U, model_height, model_width, height, width, dtype_bytes);
  const auto limit = std::max(kPredictionMaskChunkBytes, one.Total());
  auto count = std::min(survivors, limit / one.Total());
  for (const auto bytes : one.bytes) count = std::min(count, kMaximumPredictionTensorBytes / bytes);
  auto capacity = SelectedMaskCapacity::Resolve(count, model_height, model_width, height, width, dtype_bytes);
  capacity.bytes.back() = mmltk::common::system::page_rounded_bytes(capacity.bytes.back());
  // The registered host allocation owns whole pages. Its maximum padding
  // is charged separately from the chunk's useful pixel storage.
  const auto padding = mmltk::common::system::host_page_size() - 1U;
  if (limit > std::numeric_limits<std::size_t>::max() - padding) throw std::invalid_argument("RF-DETR mask retained capacity overflows");
  return {count, limit + padding, capacity};
 }
};
}  // namespace mmltk::backend::models::rfdetr
