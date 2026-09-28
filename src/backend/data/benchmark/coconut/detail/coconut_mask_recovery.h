#pragma once  // backend.data private implementation boundary
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/backend/data/detail/mask_rle_utils.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
struct CoconutRecord;
inline constexpr std::uint32_t kCoconutRecoveryPolicy = 1;
// A support owns mutable native runs or borrows admitted immutable originals.
// Borrowing retains the original mapping; capacity reports only physical owned
// storage, including cleared reusable capacity, never the borrowed span twice.
class CoconutSupportRuns final {
 struct Accounting {
  CoconutSupportRuns& runs;
  std::uint64_t before;
  explicit Accounting(CoconutSupportRuns& value) : runs(value), before(value.retained_bytes()) {}
  ~Accounting() { if (runs.total_) *runs.total_ = *runs.total_ - before + runs.retained_bytes(); }
 };
public:
 CoconutSupportRuns() = default;
 CoconutSupportRuns(const CoconutSupportRuns& other) : owned_(other.owned_), borrowed_(other.borrowed_), owner_(other.owner_) {}
 CoconutSupportRuns& operator=(const CoconutSupportRuns& other) {
  if (this == &other) return *this;
  Accounting accounting(*this);
  owned_ = other.owned_; borrowed_ = other.borrowed_; owner_ = other.owner_;
  return *this;
 }
 CoconutSupportRuns(CoconutSupportRuns&& other) noexcept
  : owned_(std::move(other.owned_)), borrowed_(std::exchange(other.borrowed_, {})), owner_(std::move(other.owner_)), total_(std::exchange(other.total_, nullptr)) {}
 CoconutSupportRuns& operator=(CoconutSupportRuns&& other) noexcept {
  if (this == &other) return *this;
  account(nullptr);
  owned_ = std::move(other.owned_); borrowed_ = std::exchange(other.borrowed_, {});
  owner_ = std::move(other.owner_); total_ = std::exchange(other.total_, nullptr);
  return *this;
 }
 ~CoconutSupportRuns() { account(nullptr); }
 // The owning workspace updates one total at real growth or custody changes.
 void account(std::uint64_t* total) noexcept {
  if (total == total_) return;
  if (total_) *total_ -= retained_bytes();
  total_ = total;
  if (total_) *total_ += retained_bytes();
 }
 [[nodiscard]] std::span<const RLEPair> view() const noexcept { return borrowed_.empty() ? std::span<const RLEPair>(owned_) : borrowed_; }
 [[nodiscard]] auto begin() const noexcept { return view().begin(); }
 [[nodiscard]] auto end() const noexcept { return view().end(); }
 [[nodiscard]] auto data() const noexcept { return view().data(); }
 [[nodiscard]] std::size_t size() const noexcept { return view().size(); }
 [[nodiscard]] bool empty() const noexcept { return view().empty(); }
 [[nodiscard]] const RLEPair& operator[](std::size_t i) const { return view()[i]; }
 [[nodiscard]] const RLEPair& front() const { return view().front(); }
 [[nodiscard]] std::uint64_t retained_bytes() const noexcept { return owned_.capacity() * sizeof(RLEPair); }
 void clear() noexcept { owner_.reset(); borrowed_ = {}; owned_.clear(); }
 template<class Iterator> void assign(Iterator begin, Iterator end) { Accounting accounting(*this); clear(); owned_.assign(begin, end); }
 void append(std::uint32_t begin, std::uint32_t end) {
  Accounting accounting(*this);
  if (!owned_.empty() && owned_.back().start + owned_.back().length == begin) owned_.back().length += end - begin;
  else owned_.push_back({begin, end - begin});
 }
 void borrow(std::span<const RLEPair> runs, std::shared_ptr<const void> owner) { clear(); borrowed_ = runs; owner_ = std::move(owner); }
 void replace(std::vector<RLEPair>& runs) noexcept { Accounting accounting(*this); owner_.reset(); borrowed_ = {}; owned_.swap(runs); }
private:
 std::vector<RLEPair> owned_;
 std::span<const RLEPair> borrowed_;
 std::shared_ptr<const void> owner_;
 std::uint64_t* total_ = nullptr;
};
struct CoconutSegmentSupport {
 std::uint64_t area = 0;
 dataset::RowMajorMaskBounds bounds;
 CoconutSupportRuns runs;
 std::optional<NormalizedBox> recovered;
 bool carved = false;
};
// Immutable physical-ID lookup shared by concurrent importers. The normalized
// backing stays owned by this index and every borrowed recovered span.
class CoconutRecoveryOriginals final {
public:
 CoconutRecoveryOriginals(const NormalizedAnnotationIndex* train, const NormalizedAnnotationIndex* validation, mmltk::common::concurrency::CancellationObservation cancellation = {});
 CoconutRecoveryOriginals(const CoconutRecoveryOriginals&) = delete;
 CoconutRecoveryOriginals& operator=(const CoconutRecoveryOriginals&) = delete;
 [[nodiscard]] std::string_view identity(CoconutImageNamespace) const noexcept;

private:
 friend class CoconutMaskRecovery;
 struct Originals {
  std::optional<NormalizedAnnotationIndex> index;
  std::unordered_map<std::uint64_t, const NormalizedImage*> images;
 };
 [[nodiscard]] const Originals* originals(CoconutImageNamespace source) const noexcept;
 // Workspaces share the immutable lookup and normalized backing, including
 // when the caller's original-input facade or batch has already retired.
 struct Share {};
 CoconutRecoveryOriginals(const CoconutRecoveryOriginals& other, Share) : train_(other.train_), validation_(other.validation_) {}
 std::shared_ptr<const Originals> train_, validation_;
};
// One synchronous importer owns this mutable, capacity-retaining workspace.
class CoconutMaskRecovery final {
public:
 explicit CoconutMaskRecovery(const CoconutRecoveryOriginals& originals) : originals_(originals, CoconutRecoveryOriginals::Share{}) {}
 CoconutMaskRecovery(const CoconutMaskRecovery&) = delete;
 CoconutMaskRecovery& operator=(const CoconutMaskRecovery&) = delete;
 [[nodiscard]] std::unique_ptr<CoconutMaskRecovery> make_workspace() const;
 [[nodiscard]] std::string_view original_identity(CoconutImageNamespace source) const noexcept;
 // Uses admitted image/box endpoints; does not visit the original mask payload.
 [[nodiscard]] std::uint64_t workspace_bytes(const CoconutRecord&) const;
 [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
 // The input owner's allowance covers this reusable workspace until retirement.
 void retire_scratch() noexcept;
 // Support belongs to this decoded image. Facts are appended for successful
 // assignments; rejection counts are settled by the importer after carving.
 void apply(CoconutImageNamespace source, const CoconutRecord& record, std::uint32_t width, std::uint32_t height, std::span<CoconutSegmentSupport> support, CoconutRecoveryImage& facts,
  mmltk::common::concurrency::CancellationObservation cancellation = {});

private:
 using Cancellation = mmltk::common::concurrency::CancellationObservation;
 using GroupKey = std::tuple<std::uint64_t, bool, bool>;
 struct Candidate {
  const NormalizedBox* box = nullptr;
  std::span<const RLEPair> runs{};
  dataset::RowMajorMaskBounds bounds{};
  std::size_t group = 0;
  std::uint64_t area = 0;
 };
 struct Group {
  GroupKey key;
  std::size_t begin = 0, dropped = 0, end = 0, candidate_count = 0;
 };
 [[nodiscard]] static bool candidate_mask(const NormalizedAnnotationIndex& index, std::uint32_t width, std::uint32_t height, Candidate& candidate, Cancellation cancellation);
 [[nodiscard]] static bool intersects(const CoconutSegmentSupport& support, const Candidate& candidate, Cancellation cancellation);
 const CoconutRecoveryOriginals originals_;
 // Flat image/group workspaces retain only high-water capacity, never historical keys.
 struct Workspace {
  std::vector<Group> groups;
  std::vector<std::size_t> ordinals;
  std::vector<Candidate> candidates;
  std::vector<std::uint8_t> represented;
  std::vector<const Candidate*> remaining;
  std::vector<std::uint64_t> identities;
  struct Cursor { std::span<const RLEPair> runs; std::size_t next = 0; };
  std::vector<Cursor> merge;
  std::vector<RLEPair> combined, scratch;
 } workspace_;
};
}  // namespace mmltk::backend::data::benchmark_internal
