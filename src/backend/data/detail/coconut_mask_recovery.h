#pragma once  // backend.data private implementation boundary
#include "coconut_annotations.h"
#include "coconut_inventory.h"
#include "mask_rle_utils.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
inline constexpr std::uint32_t kCoconutRecoveryPolicy = 1;
struct CoconutSegmentSupport {
 std::uint64_t area = 0;
 dataset::RowMajorMaskBounds bounds;
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
 using Cancellation = mmltk::common::concurrency::CancellationObservation;
 using GroupKey = std::tuple<std::uint64_t, bool, bool>;
 struct Candidate {
  const NormalizedBox* box = nullptr;
  std::span<const RLEPair> runs;
  dataset::RowMajorMaskBounds bounds;
  std::size_t group = 0;
 };
 struct Group {
  GroupKey key;
  std::size_t begin = 0, dropped = 0, end = 0, candidate_count = 0;
  bool valid = true;
 };
 [[nodiscard]] static bool candidate_mask(const NormalizedAnnotationIndex& index, std::uint32_t width, std::uint32_t height,
                                           Candidate& candidate, Cancellation cancellation);
 [[nodiscard]] static bool intersects(const CoconutSegmentSupport& support, const Candidate& candidate, Cancellation cancellation);
 struct Originals {
  const NormalizedAnnotationIndex* index = nullptr;
  std::unordered_map<std::uint64_t, const NormalizedImage*> images;
 };
 [[nodiscard]] const Originals* originals(CoconutImageNamespace source) const noexcept;
 Originals train_, validation_;
 // Flat image/group workspaces retain only high-water capacity, never historical keys.
 std::vector<Group> groups_;
 std::vector<std::size_t> ordinals_;
 std::vector<Candidate> candidates_;
 std::vector<std::uint8_t> represented_;
 std::vector<const Candidate*> remaining_;
 std::vector<std::uint64_t> identities_;
 std::vector<RLEPair> union_, scratch_;
};
}  // namespace mmltk::backend::data::benchmark_internal
