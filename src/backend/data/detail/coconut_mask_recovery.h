#pragma once  // backend.data private implementation boundary
#include "coconut_annotations.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
inline constexpr std::uint32_t kCoconutRecoveryPolicy = 1;
struct CoconutSegmentSupport {
 std::uint64_t area = 0;
 std::uint32_t min_x = UINT32_MAX, min_y = UINT32_MAX, max_x = 0, max_y = 0;
 std::vector<RLEPair> runs;
 std::optional<NormalizedBox> recovered;
 bool carved = false;
};
// Borrows immutable normalized originals, indexed once by physical image. One
// synchronous importer uses this owner at a time; RLE scratch retains capacity.
class CoconutMaskRecovery final {
public:
 CoconutMaskRecovery(const NormalizedAnnotationIndex* train, const NormalizedAnnotationIndex* validation,
                     mmltk::common::concurrency::CancellationObservation cancellation = {});
 CoconutMaskRecovery(const CoconutMaskRecovery&) = delete;
 CoconutMaskRecovery& operator=(const CoconutMaskRecovery&) = delete;
 [[nodiscard]] std::string_view original_identity(CoconutImageNamespace source) const noexcept;
 // Support belongs to this decoded image. Facts are appended for successful
 // assignments; rejection counts are settled by the importer after carving.
 void apply(CoconutImageNamespace source, const CoconutRecord& record, std::uint32_t width, std::uint32_t height,
            std::span<CoconutSegmentSupport> support, CoconutRecoveryImage& facts,
            mmltk::common::concurrency::CancellationObservation cancellation = {});

private:
 struct Originals {
  const NormalizedAnnotationIndex* index = nullptr;
  std::unordered_map<std::uint64_t, const NormalizedImage*> images;
 };
 [[nodiscard]] const Originals* originals(CoconutImageNamespace source) const noexcept;
 Originals train_, validation_;
 std::vector<RLEPair> union_, scratch_;
};
}  // namespace mmltk::backend::data::benchmark_internal
