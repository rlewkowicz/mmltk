#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include <system_error>
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/detail/benchmark_json.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_json.h"
#include <tuple>
#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotations.h"
#include <exception>
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory_io.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/frameworks/serialization/json_scalar.h"
#include "src/pch_std.h"
#include <iterator>
#include <condition_variable>
#include <deque>
#include <thread>
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
namespace mmltk::backend::data::benchmark_internal {
class CoconutComponentBacking final : public CoconutComponentMetadata {
public:
 NormalizedAnnotationReadView index;
 std::shared_ptr<const std::vector<CoconutInventoryImage>> inventory;
 std::vector<CoconutRecoveryImage> recovery;
 CoconutPhysicalDependencies dependencies;
 std::vector<std::string> image_dependencies;
 std::vector<std::uint64_t> rejected_images, compiled_ordinal_bases, incomplete_ordinal_images;
 bool source_extent_complete = false;
 std::uint64_t source_rows = 0, source_segments = 0;
 std::shared_ptr<CoconutInventorySeal> seal;
 mutable std::once_flag full_admission;
 mutable std::mutex completion_mutex;
 mutable std::shared_ptr<const NormalizedAnnotationCompletion> completion;
 bool metadata_only = false;
 static CoconutComponent finish(std::shared_ptr<CoconutComponentBacking>, NormalizedAnnotationIndex, bool, mmltk::common::concurrency::CancellationObservation, const std::filesystem::path&,
  StorageReservationPool*, const CoconutComponent* reuse = nullptr);
 static CoconutComponent loaded(std::shared_ptr<CoconutComponentBacking> backing) { return CoconutComponent(backing, backing->index, backing->seal, false); }
};
namespace {
using Json = nlohmann::json;
using Cancellation = mmltk::common::concurrency::CancellationObservation;
[[noreturn]] void invalid(std::string_view detail) { throw CoconutInputError("COCONut: " + std::string(detail)); }
std::string component_input_identity(std::string_view annotations, CoconutImageNamespace source, std::string_view physical, std::uint32_t policy, std::string_view original) {
 const auto material = nlohmann::json{
  {"domain", "coconut-component-input-v2"}, {"annotations", annotations}, {"source", source}, {"physical", physical},
  {"recovery_policy", policy}, {"original", original}
 }.dump();
 return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size())));
}
struct PhysicalKey {
 CoconutImageNamespace source;
 std::uint64_t id;
 bool operator==(const PhysicalKey&) const = default;
};
struct PhysicalKeyHash {
 std::size_t operator()(PhysicalKey key) const noexcept { return std::hash<std::uint64_t>{}(key.id) ^ (static_cast<std::size_t>(key.source) * 0x9e3779b97f4a7c15ULL); }
};
PhysicalKey physical_key(CoconutImageNamespace source, std::uint64_t id) { return {source, id}; }
// Both import assembly and the final artifact join supply rows sorted by
// physical ID within one namespace. Compact in place; never create a second
// identity registry or renumber surviving source rows.
template <class Row, class Inventory, class Dependency, class Reject>
void settle_annotation_identities(std::vector<Row>& rows, Inventory inventory, Dependency dependency, Reject reject, bool local_rejection,
 const CoconutPhysicalMembership& membership, BenchmarkCompilePipeline* execution, Cancellation cancellation) {
 std::size_t kept = 0;
 for (std::size_t first = 0; first < rows.size();) {
  throw_if_benchmark_cancelled(cancellation);
  const auto& image = inventory(rows[first]).physical;
  auto end = first + 1;
  while (end < rows.size() && inventory(rows[end]).physical.image_id == image.image_id) ++end;
  if (end == first + 1) {
   if (kept != first) rows[kept] = std::move(rows[first]);
   ++kept;
  } else {
   if (!local_rejection) invalid("duplicate annotation physical identity: " + image.member);
   for (auto i = first; i < end; ++i) {
    throw_if_benchmark_cancelled(cancellation);
    const auto& row = rows[i];
    const auto& physical = inventory(row).physical;
    const std::string_view input = dependency(row);
    if (execution && !input.empty()) {
     const auto root = membership.identity_root(physical.source);
     execution->retire_labels(root, physical.image_id, input);
     execution->retire_labels(root, physical.image_id, std::string(input) + ".provisional");
    }
    reject(row, "ambiguous annotation physical identity");
   }
  }
  first = end;
 }
 rows.resize(kept);
}
}  // namespace
namespace {
using Archive = BenchmarkArchive;
namespace reflection = mmltk::frameworks::reflection;
template <class T>
struct JsonBorrowedScalar;
template <class... T>
struct JsonBorrowedScalar<std::variant<T...>> {
 using type = std::variant<std::conditional_t<std::same_as<T, std::string>, std::string_view, T>...>;
};
struct JsonAtom {
 bool present = false;
 std::optional<typename JsonBorrowedScalar<CoconutJsonScalar>::type> value;
 void reset() {
  present = false;
  value.reset();
 }
 [[nodiscard]] std::uint64_t retained_bytes() const noexcept { return 0; }
 explicit operator bool() const noexcept { return present; }
 template <class T>
 const T* get() const noexcept {
  return value ? std::get_if<T>(&*value) : nullptr;
 }
 bool is_null() const noexcept { return get<std::nullptr_t>() != nullptr; }
 bool is_bool() const noexcept { return get<bool>() != nullptr; }
 bool boolean() const {
  if (const auto* result = get<bool>()) return *result;
  invalid("flag must be boolean or integer");
 }
 std::uint64_t unsigned_integer() const {
  if (const auto* result = get<std::uint64_t>()) return *result;
  if (const auto* result = get<std::int64_t>(); result && *result >= 0) return static_cast<std::uint64_t>(*result);
  invalid("integral field must decode from a nonnegative integer JSON number");
 }
 double number() const {
  if (const auto* result = get<double>()) return *result;
  if (const auto* result = get<std::uint64_t>()) return static_cast<double>(*result);
  if (const auto* result = get<std::int64_t>()) return static_cast<double>(*result);
  invalid("JSON field must be numeric");
 }
 std::string_view text() const {
  if (const auto* result = get<std::string_view>()) return *result;
  invalid("JSON field must be text");
 }
};
// Accounting follows the field actually consumed, including exceptional unwind.
// Retained inactive slots are never visited to reset or total a smaller row.
template <class T>
struct JsonStorageAccounting {
 T& storage;
 std::uint64_t& total;
 std::uint64_t before;
 JsonStorageAccounting(T& value, std::uint64_t& bytes) : storage(value), total(bytes), before(value.retained_bytes()) {}
 ~JsonStorageAccounting() { total = total - before + storage.retained_bytes(); }
};
template <class T>
struct JsonArray {
 bool present = false, valid = false;
 std::vector<T> values;
 std::size_t length = 0;
 std::uint64_t nested_bytes = 0;
 void reset() {
  present = valid = false;
  length = 0;
 }
 [[nodiscard]] auto active() const { return std::span(values).first(length); }
 [[nodiscard]] std::uint64_t retained_bytes() const noexcept { return values.capacity() * sizeof(T) + nested_bytes; }
 T& next() {
  if (length == values.size()) values.emplace_back();
  auto& value = values[length++];
  value.reset();
  return value;
 }
};
template <class Shape>
class JsonObject;
template <class Value>
struct JsonStorage {
 using type = JsonObject<Value>;
};
template <>
struct JsonStorage<CoconutJsonScalar> {
 using type = JsonAtom;
};
template <class Value>
struct JsonStorage<std::vector<Value>> {
 using type = JsonArray<typename JsonStorage<Value>::type>;
};
template <class Fields>
struct JsonMemberTuple;
template <class Bases, class... Declaration>
struct JsonMemberTuple<reflection::MaterializedFieldPolicyProduct<Bases, Declaration...>> {
 using type = std::tuple<typename JsonStorage<typename Declaration::member_type>::type...>;
};
// The two wire declarations project reusable parser storage without reflecting
// presence/type/object state or retaining unknown values as a DOM.
template <class Shape>
class JsonObject {
 static_assert(std::same_as<Shape, CoconutJsonRow> || std::same_as<Shape, CoconutJsonSegment>);
 using Fields = std::remove_cvref_t<decltype(reflection::field_declarations<Shape>())>;
 typename JsonMemberTuple<Fields>::type fields_;
 std::uint64_t retained_ = 0;

public:
 bool object = false;
 [[nodiscard]] std::uint64_t retained_bytes() const noexcept { return retained_; }
 void reset() {
  object = false;
  Fields::Visit([&]<class Declaration, std::size_t Index>() { std::get<Index>(fields_).reset(); });
 }
 template <auto Member>
 const auto& get() const {
  return std::get<reflection::member_index<Member>(reflection::field_declarations<Shape>())>(fields_);
 }
 template <class Visitor>
 void select(std::string_view key, Visitor&& visitor) {
  bool selected = false;
  Fields::Visit([&]<class Declaration, std::size_t Index>() {
   if (!selected && key == reflection::field_declarations<Shape>()[Index].member_name) {
    auto& field = std::get<Index>(fields_);
    JsonStorageAccounting accounting(field, retained_);
    visitor(field);
    selected = true;
   }
  });
 }
};
using JsonSegment = JsonObject<CoconutJsonSegment>;
using JsonRow = JsonObject<CoconutJsonRow>;
struct JsonParser {
 simdjson::ondemand::parser parser;
 JsonRow row;
 std::atomic<std::uint64_t> retained{0};
 [[nodiscard]] std::uint64_t retained_bytes() const noexcept { return retained.load(std::memory_order_relaxed); }
 void account() noexcept { retained.store(row.retained_bytes() + parser.capacity() * 32ULL, std::memory_order_relaxed); }
 void retire() noexcept {
  row = {};
  parser = simdjson::ondemand::parser{};
  retained.store(0, std::memory_order_relaxed);
 }
};
struct JsonConsumption {
 std::size_t events = 0, maximum;
 bool limited;
 std::size_t segment_limit;
 void event(std::size_t depth) {
  if (!limited) return;
  if (depth > 16) invalid("annotation JSON nesting exceeds admission");
  if (++events > maximum) invalid("panoptic row exceeds segment admission");
 }
};
void consume_json_value(simdjson::ondemand::value value, JsonConsumption& admission, std::size_t depth, JsonAtom* atom = nullptr, JsonRow* row = nullptr, JsonSegment* segment = nullptr,
 JsonArray<JsonSegment>* segments = nullptr, JsonArray<JsonAtom>* coordinates = nullptr) {
 admission.event(depth);
 if (atom) {
  atom->present = true;
  atom->value.reset();
 }
 if (segments) {
  segments->present = true;
  segments->valid = false;
  segments->length = 0;
 }
 if (coordinates) {
  coordinates->present = true;
  coordinates->valid = false;
  coordinates->length = 0;
 }
 const auto type = value.type().value();
 if (type == simdjson::ondemand::json_type::object) {
  if (row) row->object = true;
  if (segment) segment->object = true;
  for (auto field : value.get_object().value()) {
   admission.event(depth + 1);
   const auto key = field.unescaped_key().value();
   bool selected = false;
   const auto select = [&]<class T>(T& target) {
    selected = true;
    if constexpr (std::same_as<T, JsonAtom>)
     consume_json_value(field.value(), admission, depth + 1, &target);
    else if constexpr (std::same_as<T, JsonArray<JsonSegment>>)
     consume_json_value(field.value(), admission, depth + 1, nullptr, nullptr, nullptr, &target);
    else if constexpr (std::same_as<T, JsonArray<JsonAtom>>)
     consume_json_value(field.value(), admission, depth + 1, nullptr, nullptr, nullptr, nullptr, &target);
   };
   if (row)
    row->select(key, select);
   else if (segment)
    segment->select(key, select);
   if (!selected) consume_json_value(field.value(), admission, depth + 1);
  }
  admission.event(depth);
 } else if (type == simdjson::ondemand::json_type::array) {
  if (segments) segments->valid = true;
  if (coordinates) coordinates->valid = true;
  for (auto child : value.get_array().value()) {
   if (segments) {
    if (segments->length < admission.segment_limit) {
     auto& target = segments->next();
     JsonStorageAccounting accounting(target, segments->nested_bytes);
     consume_json_value(child.value(), admission, depth + 1, nullptr, nullptr, &target);
    } else {
     ++segments->length;
     consume_json_value(child.value(), admission, depth + 1);
    }
   } else if (coordinates) {
    if (coordinates->length < 4) {
     auto& target = coordinates->next();
     consume_json_value(child.value(), admission, depth + 1, &target);
    } else {
     ++coordinates->length;
     consume_json_value(child.value(), admission, depth + 1);
    }
   } else
    consume_json_value(child.value(), admission, depth + 1);
  }
  admission.event(depth);
 } else if (type == simdjson::ondemand::json_type::string) {
  const auto text = value.get_string().value();
  if (admission.limited && text.size() > 4096) invalid("panoptic text exceeds admission");
  if (atom) atom->value = text;
 } else if (type == simdjson::ondemand::json_type::number) {
  auto token = value.raw_json_token();
  while (!token.empty() && static_cast<unsigned char>(token.back()) <= ' ') token.remove_suffix(1);
  auto parsed = value.get_number();
  auto digits = token;
  if (digits.starts_with('-')) digits.remove_prefix(1);
  const bool large_integer = !digits.empty() && digits.front() >= '1' && digits.front() <= '9' && std::ranges::all_of(digits, [](char character) { return character >= '0' && character <= '9'; });
  if (parsed.error() == simdjson::BIGINT_ERROR || (parsed.error() == simdjson::NUMBER_ERROR && large_integer)) {
   double fallback = 0;
   const auto result = std::from_chars(token.data(), token.data() + token.size(), fallback);
   auto end = result.ptr;
   while (end != token.data() + token.size() && static_cast<unsigned char>(*end) <= ' ') ++end;
   if (result.ec != std::errc{} || end != token.data() + token.size() || !std::isfinite(fallback)) invalid("JSON number exceeds admission");
   if (atom) atom->value = fallback;
   return;
  }
  const auto number = parsed.value();
  if (atom) {
   if (number.is_uint64())
    atom->value = number.get_uint64();
   else if (number.is_int64())
    atom->value = number.get_int64();
   else
    atom->value = number.get_double();
  }
 } else if (type == simdjson::ondemand::json_type::boolean) {
  const auto boolean = value.get_bool().value();
  if (atom) atom->value = boolean;
 } else {
  if (!value.is_null().value()) invalid("invalid JSON value");
  if (atom) atom->value = nullptr;
 }
}
bool flag(const JsonAtom& field, bool required = false) {
 if (!field) {
  if (required) invalid("missing required flag");
  return false;
 }
 if (field.is_bool()) return field.boolean();
 const auto value = field.unsigned_integer();
 if (value > 1) invalid("invalid flag");
 return value != 0;
}
void segments_from_json(const JsonArray<JsonSegment>& input, CoconutRecord& record, const CoconutImportLimits& limits) {
 if (!input.present || !input.valid || input.length > limits.max_segments) invalid("segment list exceeds admission");
 record.segments.reserve(input.length);
 for (const auto& value : input.active()) {
  CoconutSegment segment;
  try {
  if (!value.object) invalid("segment must be an object");
  const auto id = value.get<&CoconutJsonSegment::id>().unsigned_integer();
  if (id == 0 || id > 0xffffffU) invalid("segment ID exceeds RGB24");
  segment.id = static_cast<std::uint32_t>(id);
  segment.category_id = value.get<&CoconutJsonSegment::category_id>().unsigned_integer();
  segment.isthing = flag(value.get<&CoconutJsonSegment::isthing>(), true);
  segment.crowd = flag(value.get<&CoconutJsonSegment::iscrowd>());
  segment.ignore = flag(value.get<&CoconutJsonSegment::ignore>());
  if (value.get<&CoconutJsonSegment::area>() && !value.get<&CoconutJsonSegment::area>().is_null()) {
   segment.area = value.get<&CoconutJsonSegment::area>().number();
   if (!std::isfinite(*segment.area) || *segment.area < 0) invalid("invalid supplied segment area");
  }
  if (value.get<&CoconutJsonSegment::bbox>().present) {
   if (!value.get<&CoconutJsonSegment::bbox>().valid || value.get<&CoconutJsonSegment::bbox>().length != 4) invalid("invalid supplied COCO bbox");
   segment.bbox.emplace();
   for (std::size_t i = 0; i < 4; ++i) (*segment.bbox)[i] = value.get<&CoconutJsonSegment::bbox>().values[i].number();
  }
  } catch (const CoconutInputError&) { segment.valid = false; segment.isthing = true; }
  record.segments.push_back(std::move(segment));
 }
}
InventoryHeader component_header(const CoconutComponentMetadata& component, std::size_t count) {
 InventoryHeader header;
 header.component = true;
 header.input_identity = component.input_identity;
 header.edition = component.edition;
 header.source = component.source;
 header.count = count;
 return header;
}
std::shared_ptr<CoconutInventorySeal> seal_inventory(const CoconutComponent& component, Cancellation cancellation, const std::filesystem::path& directory, StorageReservationPool* storage) {
 auto seal = std::make_shared<CoconutInventorySeal>();
 seal->directory = directory.empty() ? std::filesystem::temp_directory_path() : directory;
 std::filesystem::create_directories(seal->directory);
 const auto destination = seal->directory / ".coconut-inventory";
 StorageReservationPool reservations(destination, {}, storage);
 seal->staged = BenchmarkStagedArtifact::create(reservations, destination, 0, "COCONut inventory staging");
 InventoryOutput output(&seal->staged, cancellation);
 const CoconutComponentMetadata metadata{component.edition(), component.source(), component.input_identity(), component.recovery_policy(), std::string(component.original_annotation_identity())};
 seal->identity = encode_inventory(
  output, component_header(metadata, component.index().image_count()), component.inventory(), cancellation, component.recovery_policy(), component.original_annotation_identity(), component.recovery());
 seal->staged.file().sync_data();
 seal->capture(seal->staged.file());
 seal->staged.close();
 return seal;
}
BenchmarkDatasetSource index_source(CoconutImageNamespace source) {
 return source == CoconutImageNamespace::Objects365V1 || source == CoconutImageNamespace::Objects365V2 ? BenchmarkDatasetSource::kObjects365V2 : BenchmarkDatasetSource::kCoco2017;
}
std::string component_split(CoconutEdition edition, CoconutImageNamespace source) {
 return "coconut-" + std::to_string(static_cast<unsigned>(edition)) + "-" + std::to_string(static_cast<unsigned>(source));
}
void admit_component_metadata(const CoconutComponentMetadata& component, const NormalizedAnnotationMetadata& index, std::size_t images, std::size_t inventory, std::size_t recovery) {
 (void)coconut_namespace_name(component.source);
 (void)coconut_release_component(component.edition);
 if (component.input_identity.empty() || images != inventory || index.source != index_source(component.source) || index.split != component_split(component.edition, component.source))
  invalid("component index/inventory admission mismatch");
 if (component.recovery_policy) {
  if (component.recovery_policy != kCoconutRecoveryPolicy || (component.source != CoconutImageNamespace::CocoTrain && component.source != CoconutImageNamespace::CocoValidation) ||
      recovery != inventory)
   invalid("invalid recovery source or image count");
  (void)mmltk::common::io::parse_sha256_hex(component.original_annotation_identity);
 } else if (!component.original_annotation_identity.empty() || recovery)
  invalid("unexpected recovery facts");
}
// Runs in the owning assembly/decode loop. The ordinal set is temporary join
// evidence, never a second inventory or a retained selection.
class ComponentInventoryAdmission final {
public:
 explicit ComponentInventoryAdmission(CoconutImageNamespace source) : source_(source) {}
 void image(const CoconutInventoryImage& row, const NormalizedImage& normalized, bool physical_admitted = false, bool ordinal_complete = true) {
  if (!physical_admitted) validate_coconut_physical_image(row.physical);
  if (row.physical.source != source_ || row.physical.image_id != normalized.source_image_id || row.physical.shard != normalized.source_shard || (ordinal_complete && !ordinals_.insert(row.source_ordinal).second) ||
      (previous_ && row.physical.image_id <= *previous_))
   invalid("invalid component image inventory");
  previous_ = row.physical.image_id;
 }

private:
 CoconutImageNamespace source_;
 std::optional<std::uint64_t> previous_;
 std::unordered_set<std::uint64_t> ordinals_;
};
void admit_recovery_image(const CoconutRecoveryImage& fact, const NormalizedImage& image) {
 if (fact.image_id != image.source_image_id || fact.unresolved > 65535U || fact.unresolved != fact.omissions.size() || fact.objects.size() > image.box_count ||
     fact.objects.size() > 65535U - fact.omissions.size())
  invalid("invalid image recovery facts");
}
class ComponentRecoveryAdmission final {
public:
 explicit ComponentRecoveryAdmission(const CoconutRecoveryImage* fact) : fact_(fact) {}
 void box(const NormalizedBox& box) {
  if (fact_ && (!fact_->objects.empty() || !fact_->omissions.empty())) by_annotation_.emplace(box.annotation_id, box);
 }
 void finish() const {
  if (!fact_) return;
  std::unordered_set<std::uint64_t> originals, annotations;
  for (const auto& object : fact_->omissions)
   if (object.original_annotation_id != 0 || object.source_category_id == 0 || !annotations.insert(object.annotation_id).second || by_annotation_.contains(object.annotation_id))
    invalid("invalid omitted object identity");
  for (const auto& object : fact_->objects) {
   const auto found = by_annotation_.find(object.annotation_id);
   if (!originals.insert(object.original_annotation_id).second || !annotations.insert(object.annotation_id).second || found == by_annotation_.end() ||
       found->second.source_ordinal != object.source_ordinal || found->second.source_category_id != object.source_category_id)
    invalid("invalid recovered object identity");
  }
 }

private:
 const CoconutRecoveryImage* fact_;
 std::unordered_map<std::uint64_t, NormalizedBox> by_annotation_;
};
struct PendingNativeImage {
 struct Support {
  std::uint64_t area;
  dataset::RowMajorMaskBounds bounds;
  std::uint64_t first_run, runs;
  bool valid;
 };
 const CoconutRecord* record = nullptr;
 std::size_t position = 0;
 CoconutPhysicalImage physical;
 dataset::MaskDimensions dimensions;
 std::uint32_t unlisted_segment = 0;
 std::shared_ptr<const BenchmarkMappedArtifact> backing;
 std::span<const Support> supports;
 std::span<const RLEPair> runs;
 std::shared_ptr<const CoconutNativeImage> product;
 BenchmarkSourcePublication publication;
 std::string provisional_dependency;
 void stage_support(std::vector<CoconutSegmentSupport> input, const std::filesystem::path& directory, StorageReservationPool* storage) {
  std::uint64_t count = 0;
  for (const auto& item : input) count = mmltk::common::math::checked_add(count, item.runs.size(), "pending support run count overflow");
  const auto run_offset = mmltk::common::math::checked_multiply(input.size(), sizeof(Support), "pending support extent overflow");
  const auto bytes = mmltk::common::math::checked_add(run_offset, mmltk::common::math::checked_multiply(count, sizeof(RLEPair), "pending support extent overflow"), "pending support extent overflow");
  if (!bytes) return;
  const auto root = directory.empty() ? std::filesystem::temp_directory_path() : directory;
  std::filesystem::create_directories(root);
  StorageReservationPool reservations(root, {}, storage);
  auto owner = std::make_shared<BenchmarkMappedArtifact>(reservations, root / ".native-support", bytes, "pending native support");
  std::size_t offset = 0;
  for (std::size_t i = 0; i < input.size(); ++i) {
   const auto& item = input[i];
   const Support header{item.area, item.bounds, offset, item.runs.size(), item.valid};
   owner->write(&header, sizeof(header), i * sizeof(header));
   owner->write(item.runs.data(), item.runs.size() * sizeof(RLEPair), run_offset + offset * sizeof(RLEPair));
   offset += item.runs.size();
  }
  const void* address = owner->map_readonly();
  supports = {static_cast<const Support*>(address), input.size()};
  runs = {reinterpret_cast<const RLEPair*>(static_cast<const std::byte*>(address) + run_offset), static_cast<std::size_t>(count)};
  backing = std::move(owner);
 }
 std::vector<CoconutSegmentSupport> read_support() const {
  std::vector<CoconutSegmentSupport> result(supports.size());
  for (std::size_t i = 0; i < result.size(); ++i) {
   result[i].valid = supports[i].valid;
   result[i].area = supports[i].area;
   result[i].bounds = supports[i].bounds;
   result[i].runs.borrow(runs.subspan(supports[i].first_run, supports[i].runs), backing);
  }
  return result;
 }
};

class Importer final {
 struct Worker {
  BenchmarkAllowance allowance;
  CoconutNativeWorkspace native;
  std::atomic<std::uint64_t> retained{0};
  explicit Worker(const CoconutImportRequest& request) : native(request.limits, request.cancellation) { native.recovery(request.recovery); }
 };

public:
 explicit Importer(const CoconutImportRequest& request) : request_(request), archive_worker_(request), assembly_workspace_(request.limits, request.cancellation) {
  if (request.input_identity.empty()) invalid("missing pinned input identity");
  if (!request.physical_membership) invalid("missing physical membership admission");
  assembly_workspace_.recovery(request.recovery);
  if (request_.progress) request_.progress(0);
 }
 ~Importer() { stop_recovery(); }
 void stop_recovery() {
  if (recovery_controller_.joinable()) {
   recovery_controller_.request_stop();
   pending_changed_.notify_all();
   recovery_controller_.join();
  }
 }
 void retire_scratch() noexcept {
  archive_worker_.native.retire();
  archive_worker_.retained.store(0, std::memory_order_relaxed);
 }
 std::optional<CoconutPhysicalImage> identify(const CoconutRecord& record, bool wait = false, bool report = false) {
  const auto reject = [&](std::string_view reason) {
   if (report && request_.rejected_object) {
    const auto sources = coconut_release_sources(request_.edition);
    const CoconutPhysicalImage available{record.namespace_hint.value_or(sources.front()), record.image_id, 0, record.file_name, request_.input_identity};
    request_.rejected_object(available, record, {}, reason, 0);
   }
  };
  try {
   return wait ? request_.physical_membership->resolve(request_.edition, record) : request_.physical_membership->identify(request_.edition, record);
  } catch (const CoconutInputError& error) {
   if (!request_.accept_partial && !request_.reject_invalid_identities) throw;
   reject(error.what());
  } catch (const BenchmarkArchiveError& error) {
   if (!request_.accept_partial && !request_.reject_invalid_identities) throw;
   reject(error.what());
  } catch (const CoconutPhysicalMembershipError& error) {
   if (!request_.accept_partial && !request_.reject_invalid_identities) throw;
   reject(error.what());
  }
  return std::nullopt;
 }
 [[nodiscard]] std::uint64_t retained_workspace_bytes() const noexcept { return archive_worker_.retained.load(std::memory_order_relaxed); }
 dataset::MaskDimensions admit_png(CoconutNativeWorkspace& workspace, const CoconutRecord& record, std::span<const std::uint8_t> png, const CoconutPhysicalImage& physical) {
  try {
   return workspace.admit_png(record, png);
  } catch (const CoconutInputError& error) {
   throw CoconutInputError(physical.member + ": " + error.what());
  }
 }
 std::uint64_t png_workspace(const CoconutRecord& record, std::size_t position, std::span<const std::uint8_t> png, const BenchmarkAllowance& allowance, dataset::MaskDimensions& dimensions) {
  const auto admitted = identify(record);
  if (!admitted) return 0;
  const auto& physical = *admitted;
  std::size_t reused = 0;
  if (std::ranges::find(request_.retained_sources, physical.source) != request_.retained_sources.end() || reusable_native(record, allowance, position)) return 0;
  if (!reusable_image(physical, reused)) {
   const auto inspect = [&](std::size_t) {
    try { dimensions = admit_png(archive_worker_.native, record, png, physical); } catch (const CoconutInputError&) { dimensions = {record.width, record.height}; }
   };
   if (request_.execution)
    request_.execution->run(BenchmarkStage::Normalize, {}, inspect, allowance);
   else
    inspect(0);
  }
  return mmltk::common::math::checked_add(retained_workspace_bytes(),
   mmltk::common::math::checked_multiply(assembly_workspace_.workspace_bytes(record, std::uint64_t{dimensions.width} * dimensions.height, record.segments.size(), png.size()),
    2U, "COCONut PNG workspace overflow"), "COCONut retained PNG workspace overflow");
 }
 void canonical_ordinals(std::vector<std::size_t> ordinals) {
  source_ordinals_ = std::move(ordinals);
  for (auto& [source, component] : components_) {
   (void)source;
   for (auto& image : component.inventory) image.source_ordinal = source_ordinals_.at(image.source_ordinal);
  }
 }
 void retire_input(const BenchmarkAllowance& allowance) {
  const std::lock_guard lock(groups_mutex_);
  const auto retire = [&](Worker& worker) {
   if (worker.allowance.aliases(allowance)) {
    worker.native.retire();
    worker.retained.store(0, std::memory_order_relaxed);
    worker.allowance = {};
   }
  };
  for (auto& worker : idle_) retire(*worker);
  for (auto& [group, worker] : groups_) {
   (void)group;
   retire(*worker);
  }
 }
 std::uint64_t maximum_workspace(const CoconutRecord& record) {
  std::uint64_t retained = archive_worker_.retained.load(std::memory_order_relaxed);
  {
   const std::lock_guard lock(groups_mutex_);
   for (const auto& worker : idle_) retained = std::max(retained, worker->retained.load(std::memory_order_relaxed));
   for (const auto& [group, worker] : groups_) {
    (void)group;
    retained = std::max(retained, worker->retained.load(std::memory_order_relaxed));
   }
  }
  const auto physical = identify(record);
  if (physical && std::ranges::find(request_.retained_sources, physical->source) != request_.retained_sources.end()) return retained;
  const auto pixels = record.width && record.height ? std::uint64_t{record.width} * record.height : request_.limits.max_pixels;
  return mmltk::common::math::checked_add(retained,
   mmltk::common::math::checked_multiply(
    assembly_workspace_.workspace_bytes(record, pixels, request_.limits.max_segments, request_.limits.max_png_bytes), 2U, "COCONut retained normalizer workspace overflow"),
   "COCONut retained normalizer workspace overflow");
 }
 void consume_group(std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) {
  if (input.membership && request_.preparation_progress) {
   const std::lock_guard lock(progress_mutex_);
   ++parsed_rows_;
   if (parsed_rows_ == 1 || parsed_rows_ % kProgressQuantum == 0) request_.preparation_progress(CoconutPreparationStage::Annotations, parsed_rows_);
  }
  if (input.membership) (void)identify(record);
  if (input.membership && !request_.metadata_only) {
   if (request_.execution && input.allowance && input.live_bytes <= input.allowance.bytes())
    request_.execution->with_unused_workspace(input.allowance, input.live_bytes, [&] { request_.execution->run(BenchmarkStage::Metadata, {}, [](std::size_t) {}, input.allowance); });
   return;
  }
  Worker* worker;
  {
   const std::lock_guard lock(groups_mutex_);
   auto& slot = groups_[group];
   if (!slot) {
    const auto reusable = std::ranges::find_if(idle_, [&](const auto& candidate) { return !candidate->allowance || candidate->allowance.aliases(input.allowance); });
    if (reusable != idle_.end()) {
     slot = std::move(*reusable);
     idle_.erase(reusable);
    } else
     slot = std::make_unique<Worker>(request_);
   }
   worker = slot.get();
   worker->allowance = input.allowance;
  }
  consume(record, input.png, input.allowance, input.live_bytes, worker);
 }
 void retire_group(std::size_t group) {
  const std::lock_guard lock(groups_mutex_);
  const auto found = groups_.find(group);
  if (found == groups_.end()) return;
  idle_.push_back(std::move(found->second));
  groups_.erase(found);
 }
 void collect_groups() {
  groups_.clear();
  idle_.clear();
  offered_ = decltype(offered_){};
 }
 void settle_ready_recovery(const std::function<void()>& release_input) {
  // Pressure comes from blocked admission, never a diagnostic observer. The
  // sequence also checks at every growth/group boundary and closes at
  // terminal, so recovery becoming ready after this check cannot pin a reader.
  if (!request_.execution || !request_.execution->resource_pressure()) return;
  std::unique_lock lock(groups_mutex_);
  const auto ready = recovery_ready_;
  if (ready == recovery_settled_) return;
  lock.unlock();
  release_input();
  lock.lock();
  pending_changed_.wait(lock, [&] { return recovery_settled_ >= ready || recovery_failure_; });
  if (recovery_failure_) std::rethrow_exception(recovery_failure_);
 }
 bool reusable_native(const CoconutRecord& record, const BenchmarkAllowance& allowance, std::size_t position = SIZE_MAX) {
  const auto admitted = identify(record);
  if (!admitted) return false;
  const auto& physical = *admitted;
  const auto& row = request_.records->row(position == SIZE_MAX ? record.source_ordinal : position);
  if (!row.complete() || !row.native()) return false;
  auto original = original_input(physical.source);
  if (!original.terminal) return false;
  const auto& saved = row.native();
  const auto& lineage = saved->lineage().component;
  const auto identity = original.originals ? original.originals->identity(physical.source) : request_.recovery ? request_.recovery->original_identity(physical.source) : std::string_view{};
  return lineage.annotation_input_identity == request_.input_identity && saved->inventory().physical == physical && lineage.original_generation == original.generation &&
         lineage.original_annotation_identity == identity;
 }
 void consume(
  const CoconutRecord& record, std::span<const std::uint8_t> png, BenchmarkAllowance allowance = {}, std::uint64_t input_live_bytes = 0, Worker* selected = nullptr, std::size_t position = SIZE_MAX,
  dataset::MaskDimensions dimensions = {}) {
  const auto admitted = identify(record, false, true);
  if (!admitted) {
   { const std::lock_guard lock(groups_mutex_); ++rows_; }
   retain_image(position == SIZE_MAX ? record.source_ordinal : position, {});
   return;
  }
  const auto& physical = *admitted;
  try {
   consume_image(record, physical, png, std::move(allowance), input_live_bytes, selected, position, dimensions);
  } catch (const CoconutInputError& error) {
   const bool required = !record.width || !record.height || std::ranges::any_of(record.segments, [](const auto& segment) { return segment.isthing || !segment.valid; });
   if (required && !request_.accept_partial) throw;
   if (position == SIZE_MAX) position = record.source_ordinal;
   if (request_.rejected_object) request_.rejected_object(physical, record, {}, error.what(), 0);
   auto product = CoconutNativeImage::rejected(record, physical, lineage_for(physical.source, nullptr, 0));
   {
    const std::lock_guard lock(groups_mutex_);
    ++rows_;
   }
   retain_image(position, std::move(product));
  }
 }
 void consume_image(const CoconutRecord& record, const CoconutPhysicalImage& physical, std::span<const std::uint8_t> png, BenchmarkAllowance allowance, std::uint64_t input_live_bytes,
  Worker* selected, std::size_t position, dataset::MaskDimensions dimensions) {
  if (position == SIZE_MAX) position = record.source_ordinal;
  auto& worker = selected ? *selected : archive_worker_;
  auto& workspace = worker.native;
  if (request_.membership_ready && request_.originals && !request_.metadata_only) {
   const std::lock_guard lock(groups_mutex_);
   auto& component = components_[physical.source];
   component.edition = request_.edition;
   component.source = physical.source;
   component.annotation_input_identity = component.input_identity = request_.input_identity;
   component.index.source = index_source(physical.source);
   component.index.split = component_split(request_.edition, physical.source);
   component.index.images.push_back({physical.image_id, 0, 0, record.width, record.height, physical.shard, 0});
   component.inventory.push_back({physical, record.image_id, record.source_ordinal});
  }
  const bool normalize = !request_.metadata_only && std::ranges::find(request_.retained_sources, physical.source) == request_.retained_sources.end();
  const auto original = normalize && request_.originals && physical.archive_identity == "pending" ? CoconutOriginalInput{{}, false, 0} : normalize ? original_input(physical.source) : CoconutOriginalInput{{}, true, 0};
  bool deferred = !original.terminal;
  if (request_.originals) workspace.originals(original.originals);
  const auto lineage = lineage_for(physical.source, workspace.recovery(), original.generation);
  std::shared_ptr<const CoconutNativeImage> saved;
  {
   const std::lock_guard lock(groups_mutex_);
   const auto& candidate = request_.records->row(position).native();
   if (normalize && !deferred && candidate && candidate->inventory().physical == physical && candidate->lineage().component.annotation_input_identity == request_.input_identity &&
       candidate->lineage().component.original_generation == original.generation && candidate->lineage().component.original_annotation_identity == lineage->component.original_annotation_identity)
    saved = candidate;
  }
  std::size_t reused_position = 0;
  const auto* reused = normalize && !saved ? reusable_image(physical, reused_position) : nullptr;
  const auto publication = normalize ? request_.physical_membership->label_publication(request_.edition, physical) : BenchmarkSourcePublication{};
  if (normalize && !saved && !reused && png.empty()) throw CoconutInputError("missing required panoptic mask");
  const auto inspect = [&](std::size_t) {
   if (!normalize || reused || saved || dimensions.width) return;
   dimensions = admit_png(workspace, record, png, physical);
  };
  if (request_.execution)
   request_.execution->run(BenchmarkStage::Normalize, {}, inspect, allowance);
  else
   inspect(0);
  auto upcoming = normalize && !saved ? workspace.workspace_bytes(record, std::uint64_t{dimensions.width} * dimensions.height, record.segments.size(), png.size(), !deferred) : 0;
  if (!deferred && workspace.recovery() && allowance && (input_live_bytes > allowance.bytes() || upcoming > allowance.bytes() - input_live_bytes)) {
   deferred = true;
   upcoming = workspace.workspace_bytes(record, std::uint64_t{dimensions.width} * dimensions.height, record.segments.size(), png.size(), false);
  }
  auto retained = workspace.retained_bytes();
  if (allowance && (input_live_bytes > allowance.bytes() || upcoming > allowance.bytes() - input_live_bytes || retained > allowance.bytes() - input_live_bytes - upcoming)) {
   workspace.retire();
   retained = 0;
  }
  const auto bytes = mmltk::common::math::checked_add(retained, upcoming, "COCONut retained normalizer overflow");
  std::shared_ptr<const CoconutNativeImage> complete = saved;
  const auto work = [&](std::size_t) {
   throw_if_benchmark_cancelled(request_.cancellation);
   {
    const std::lock_guard lock(groups_mutex_);
    if (!offered_.insert(physical_key(physical.source, physical.image_id)).second) invalid("duplicate offered physical member: " + physical.member);
   }
   try {
    if (!normalize) {
     if (request_.metadata_only && std::ranges::find(request_.retained_sources, physical.source) == request_.retained_sources.end()) {
      const std::lock_guard lock(groups_mutex_);
      auto& component = components_[physical.source];
      static_cast<CoconutComponentMetadata&>(component) = lineage->component;
      static_cast<NormalizedAnnotationMetadata&>(component.index) = lineage->index;
      component.index.images.push_back({physical.image_id, 0, 0, record.width, record.height, physical.shard, 0});
      component.inventory.push_back({physical, record.image_id, record.source_ordinal});
      if (component.recovery_policy) component.recovery.push_back({physical.image_id, 0, {}});
     }
    } else if (!saved) {
     if (reused)
      complete = workspace.reuse(record, physical, reused->index(), reused_position, reused->inventory_image(reused_position),
       reused->recovery_policy() ? &reused->recovery_image(reused_position) : nullptr, lineage, reused->image_input_identity(reused_position), reused->required_labels_valid(reused_position));
     else {
      workspace.decode(record, png, dimensions);
      if (deferred) {
       auto native = std::make_shared<PendingNativeImage>();
       native->record = &record;
       native->position = position;
       native->physical = physical;
       native->dimensions = dimensions;
       native->publication = publication;
       native->unlisted_segment = workspace.unlisted_segment();
       native->stage_support(workspace.take_support(record.segments.size()), request_.inventory_directory, request_.execution ? &request_.execution->storage() : nullptr);
       auto support = native->read_support();
       workspace.originals({});
       workspace.borrow_support(record, support, dimensions, native, native->unlisted_segment);
       auto provisional = workspace.finish(record, physical, dimensions, lineage, {});
       provisional = CoconutNativeImage::stage(std::move(provisional), request_.inventory_directory, request_.execution ? &request_.execution->storage() : nullptr);
       native->provisional_dependency = provisional->input_identity() + ".provisional";
       publish_labels(provisional, publication, true);
       defer_native(std::move(native));
      } else
       complete = workspace.finish(record, physical, dimensions, lineage, request_.rejected_object);
     }
    }
   } catch (const CoconutInputError& error) {
    throw CoconutInputError(physical.member + ": " + error.what());
   }
   if (complete) complete = CoconutNativeImage::stage(std::move(complete), request_.inventory_directory, request_.execution ? &request_.execution->storage() : nullptr);
   const std::lock_guard lock(groups_mutex_);
   ++rows_;
  };
  const auto run = [&] {
   if (request_.execution)
    request_.execution->run(request_.metadata_only ? BenchmarkStage::Metadata : BenchmarkStage::Normalize, {bytes, 1}, work, allowance);
   else
    work(0);
  };
  try {
   if (request_.execution && allowance && input_live_bytes)
    request_.execution->with_unused_workspace(allowance, mmltk::common::math::checked_add(input_live_bytes, bytes, "COCONut live consumer workspace overflow"), run);
   else
    run();
  } catch (...) {
   workspace.retire();
   worker.retained.store(0, std::memory_order_relaxed);
   throw;
  }
  worker.retained.store(workspace.retained_bytes(), std::memory_order_relaxed);
  if (complete) {
   if (!saved && !reused) settle_report(*complete);
   if (!saved)
    retain_image(position, complete);
   else
    complete_row();
   publish_labels(complete, publication);
  } else if (!normalize)
   complete_row();
 }
 std::vector<CoconutComponent> finish(bool partial = false) {
  if (partial && rows_ == 0 && std::ranges::none_of(request_.records->rows(), [](const auto& row) { return bool(row.native()); })) return {};
  if (partial && !request_.records->groups.empty()) request_.records->rebase_parquet_segments();
  if (!partial && rows_ == 0) invalid("selected release contains no offered image rows");
  if (!partial && request_.expected_rows != 0 && rows_ != request_.expected_rows) invalid("offered row count does not match the selected release");
  const bool early_membership = request_.membership_ready && !request_.metadata_only && recovery_controller_.joinable();
  if (early_membership) request_.membership_ready(assemble(true));
  {
   const std::lock_guard lock(groups_mutex_);
   input_done_ = true;
  }
  pending_changed_.notify_all();
  if (recovery_controller_.joinable()) recovery_controller_.join();
  if (recovery_failure_) std::rethrow_exception(recovery_failure_);
  if (request_.progress && completed_rows_ != 1 && completed_rows_ % kProgressQuantum != 0) request_.progress(completed_rows_);
  if (request_.metadata_only && request_.preparation_progress && completed_rows_ != 1 && completed_rows_ % kProgressQuantum != 0)
   request_.preparation_progress(CoconutPreparationStage::Membership, completed_rows_);
  throw_if_benchmark_cancelled(request_.cancellation);
  auto result = assemble(request_.metadata_only);
  if (request_.membership_ready && !request_.metadata_only && !early_membership) {
   std::vector<CoconutComponent> membership;
   membership.reserve(result.size());
   for (const auto& component : result) membership.push_back(component.membership());
   request_.membership_ready(std::move(membership));
  }
  return result;
 }
 std::vector<CoconutComponent> assemble(bool metadata_only) {
  if (!metadata_only) {
   request_.parent_allowance.retire_continuation();
   request_.physical_membership->wait_sources(request_.edition);
   for (std::size_t position = 0; position < request_.records->size(); ++position) {
    const auto& row = request_.records->row(position);
    if (!row.native()) continue;
    if (!row.native()->image().width || !row.native()->image().height) { request_.records->native(position, {}); continue; }
    const auto physical = identify(row.record(), true, true);
    if (physical) request_.records->native(position, CoconutNativeImage::bind(row.native(), *physical));
    else {
     if (request_.execution) {
      const auto& previous = row.native()->inventory().physical;
      const auto root = request_.physical_membership->identity_root(previous.source);
      request_.execution->retire_labels(root, previous.image_id, row.native()->input_identity());
      request_.execution->retire_labels(root, previous.image_id, row.native()->input_identity() + ".provisional");
     }
     request_.records->native(position, {});
    }
   }
  }

  struct ImageChunk {
   const CoconutComponentMetadata* component;
   const NormalizedAnnotationMetadata* metadata;
   const NormalizedImage* image;
   std::span<const NormalizedBox> boxes;
   std::span<const RLEPair> runs;
   const CoconutInventoryImage* inventory;
   const CoconutRecoveryImage* recovery;
   std::uint64_t segment_base;
   const std::string* dependency = nullptr;
   bool required_labels_valid = true;
   bool ordinals_complete = true;
  };
  std::uint64_t image_count = 0, recovery_join_boxes = 0;
  for (const auto& row : request_.records->rows())
   if (const auto& native = row.native(); !metadata_only && native && std::ranges::find(request_.retained_sources, native->lineage().component.source) == request_.retained_sources.end()) {
    ++image_count;
    if (native->lineage().component.recovery_policy) recovery_join_boxes = std::max(recovery_join_boxes, std::uint64_t{native->boxes().size()});
   }
  if (metadata_only) for (const auto& [source, component] : components_) {
   (void)source;
   image_count += component.inventory.size();
  }
  // Layout capacity and the temporary ordinal set live through assembly. Only
  // one image's recovery join is live; bound it by that component's boxes and
  // the admitted per-image segment limit without another image census.
  const auto join_bytes = mmltk::common::math::checked_multiply(recovery_join_boxes, std::uint64_t{256}, "COCONut recovery join workspace overflow");
  const auto workspace = mmltk::common::math::checked_add(mmltk::common::math::checked_multiply(image_count, std::uint64_t{6 * sizeof(ImageChunk)}, "COCONut merge workspace overflow"),
   mmltk::common::math::checked_add(join_bytes, std::uint64_t{65536}, "COCONut merge workspace overflow"), "COCONut merge workspace overflow");
  // Layout references retain their credit through final assembly and intervening
  // inventory I/O; only finite CPU work enters the shared lane.
  auto merge_allowance = request_.execution ? request_.execution->reserve({workspace, 1}) : BenchmarkAllowance{};
  std::map<CoconutImageNamespace, std::vector<ImageChunk>> images;
  std::map<CoconutImageNamespace, AnnotationRejectCounts> rejected;
  std::map<CoconutImageNamespace, std::pair<std::size_t, std::size_t>> extents;
  const auto gather = [&](ImageChunk chunk, const AnnotationRejectCounts& counts) {
   const auto source = chunk.component->source;
   auto& [boxes, runs] = extents[source];
   boxes = mmltk::common::math::checked_add(boxes, chunk.boxes.size(), "COCONut box count overflow");
   runs = mmltk::common::math::checked_add(runs, chunk.runs.size(), "COCONut run count overflow");
   images[source].push_back(chunk);
   mmltk::frameworks::reflection::visit_materialized_members<AnnotationRejectCounts>([&]<class Declaration>(const auto&) {
    rejected[source].*Declaration::pointer = mmltk::common::math::checked_add(rejected[source].*Declaration::pointer, counts.*Declaration::pointer, "COCONut rejection count overflow");
   });
  };
  const auto gather_images = [&](std::size_t) {
   if (!metadata_only)
    for (const auto& row : request_.records->rows()) {
     const auto& native = row.native();
     if (!native || std::ranges::find(request_.retained_sources, native->lineage().component.source) != request_.retained_sources.end()) continue;
     if (row.segment_ordinal() < native->segment_begin()) invalid("native chunk ordinal exceeds its canonical row");
     gather({&native->lineage().component, &native->lineage().index, &native->image(), native->boxes(), native->runs(), &native->inventory(), &native->recovery(),
             row.segment_ordinal() - native->segment_begin(), &native->input_identity(), native->required_labels_valid(), row.segment_ordinal_complete()},
      native->rejected());
    }
   if (metadata_only) for (auto& [source, component] : components_) {
    (void)source;
    for (std::size_t i = 0; i < component.inventory.size(); ++i)
     gather({&component, &component.index, &component.index.images[i], {}, {}, &component.inventory[i], component.recovery_policy ? &component.recovery[i] : nullptr, 0}, {});
   }
  };
  if (request_.execution)
   request_.execution->run(BenchmarkStage::Metadata, {}, gather_images, merge_allowance);
  else
   gather_images(0);
  std::vector<CoconutComponent> result;
  for (auto& [source, rows] : images) {
   throw_if_benchmark_cancelled(request_.cancellation);
   const auto order = [&](std::size_t) { std::ranges::sort(rows, {}, [](const ImageChunk& row) { return row.inventory->physical.image_id; }); };
   if (request_.execution) request_.execution->run(BenchmarkStage::Metadata, {}, order, merge_allowance);
   else order(0);
   settle_annotation_identities(rows, [](const ImageChunk& row) -> const CoconutInventoryImage& { return *row.inventory; },
    [](const ImageChunk& row) -> std::string_view { return row.dependency ? std::string_view(*row.dependency) : std::string_view{}; },
    [&](const ImageChunk& row, std::string_view reason) {
     if (request_.rejected_object) {
      CoconutRecord available;
      available.image_id = row.inventory->release_image_id;
      available.file_name = row.inventory->physical.member;
      request_.rejected_object(row.inventory->physical, available, {}, reason, 0);
     }
     extents[source].first -= row.boxes.size();
     extents[source].second -= row.runs.size();
    }, request_.accept_partial || request_.reject_invalid_identities, *request_.physical_membership, request_.execution, request_.cancellation);
   if (rows.empty()) continue;
   if (!metadata_only && request_.originals && (source == CoconutImageNamespace::CocoTrain || source == CoconutImageNamespace::CocoValidation)) {
    auto original = request_.originals(source, true, {});
    original_owner_ = std::move(original.originals);
    assembly_workspace_.originals(original_owner_);
   }
   auto header = *rows.front().component;
   if (!metadata_only && request_.originals && (source == CoconutImageNamespace::CocoTrain || source == CoconutImageNamespace::CocoValidation)) {
    const auto original = request_.originals(source, true, {});
    header = lineage_for(source, assembly_workspace_.recovery(), original.generation)->component;
   }
   auto sealed = std::make_shared<CoconutComponentBacking>();
   static_cast<CoconutComponentMetadata&>(*sealed) = header;
   sealed->source_rows = request_.records->size();
   if ((request_.records->parquet || request_.records->document_complete) && std::ranges::all_of(request_.records->rows(), [](const auto& row) { return row.complete(); })) {
    sealed->source_extent_complete = true;
    for (const auto& record : request_.records->rows()) sealed->source_segments = mmltk::common::math::checked_add(sealed->source_segments, record.record().segments.size(), "COCONut source segment extent overflow");
   }
   auto inventory = std::make_shared<std::vector<CoconutInventoryImage>>();
   const CoconutComponent* reusable = nullptr;
   if (!sealed->recovery_policy && request_.records && request_.records->identity == request_.input_identity)
    for (const auto& candidate : request_.records->inventories)
     if (!candidate.index().selected() && !candidate.recovery_policy() && candidate.edition() == sealed->edition && candidate.source() == source &&
         candidate.matches_inputs(request_.input_identity, *request_.physical_membership, metadata_only ? nullptr : assembly_workspace_.recovery()) && candidate.index().image_count() == rows.size()) {
      reusable = &candidate;
      break;
     }
   NormalizedAnnotationIndex normalized;
   const auto assemble = [&](std::size_t) {
    inventory->reserve(rows.size());
    sealed->image_dependencies.reserve(rows.size());
    if (sealed->recovery_policy) sealed->recovery.reserve(rows.size());
    ComponentInventoryAdmission inventory_admission(source);
    NormalizedAnnotationMetadata metadata = *rows.front().metadata;
    metadata.rejected = rejected[source];
    const auto [boxes, runs] = extents[source];
    std::optional<NormalizedAnnotationAssembler> assembly;
    if (!metadata_only) assembly.emplace(metadata, rows.size(), boxes, runs, request_.cancellation, request_.inventory_directory, request_.execution ? &request_.execution->storage() : nullptr);
    NormalizedAnnotationBuilder membership;
    static_cast<NormalizedAnnotationMetadata&>(membership) = metadata;
    if (metadata_only) membership.images.reserve(rows.size());
    for (const auto& row : rows) {
     throw_if_benchmark_cancelled(request_.cancellation);
     const auto& image = *row.image;
     inventory->push_back(*row.inventory);
     sealed->compiled_ordinal_bases.push_back(row.segment_base);
     if (!row.required_labels_valid) sealed->rejected_images.push_back(image.source_image_id);
     if (!row.ordinals_complete) sealed->incomplete_ordinal_images.push_back(image.source_image_id);
     auto& inventory_row = inventory->back();
     if (!metadata_only && !source_ordinals_.empty()) inventory_row.source_ordinal = source_ordinals_.at(inventory_row.source_ordinal);
     if (reusable && inventory_row != reusable->inventory_image(inventory->size() - 1)) reusable = nullptr;
     inventory_admission.image(inventory_row, image, reusable != nullptr, row.ordinals_complete);
     sealed->dependencies.add(row.inventory->physical);
     sealed->image_dependencies.push_back(
      row.dependency ? *row.dependency : coconut_image_input_identity(sealed->annotation_input_identity, row.inventory->physical, sealed->original_annotation_identity));
     if (sealed->recovery_policy) {
      auto recovery = row.component->recovery_policy ? *row.recovery : CoconutRecoveryImage{image.source_image_id, 0, {}};
      for (auto* objects : {&recovery.objects, &recovery.omissions})
       for (auto& object : *objects) object.source_ordinal = mmltk::common::math::checked_add(object.source_ordinal, row.segment_base, "COCONut recovery ordinal overflow");
      admit_recovery_image(recovery, image);
      sealed->recovery.push_back(std::move(recovery));
     }
     ComponentRecoveryAdmission recovery_admission(sealed->recovery_policy ? &sealed->recovery.back() : nullptr);
     if (metadata_only)
      membership.images.push_back(image);
     else {
      assembly->begin_image(image);
      for (auto box : row.boxes) {
       box.source_ordinal = mmltk::common::math::checked_add(box.source_ordinal, row.segment_base, "COCONut segment ordinal overflow");
       if (!row.ordinals_complete) box.flags |= kAnnotationOrdinalIncomplete;
       recovery_admission.box(box);
       assembly->append_box(box, row.runs.subspan(static_cast<std::size_t>(box.mask_rle_offset), box.mask_rle_pairs));
      }
      recovery_admission.finish();
     }
    }
    normalized = metadata_only ? seal_normalized_annotation_metadata(std::move(membership)) : assembly->finish();
   };
   if (request_.execution)
    request_.execution->run(BenchmarkStage::Metadata, {}, assemble, merge_allowance);
   else
    assemble(0);
   sealed->input_identity = coconut_component_input_identity(request_.input_identity, source, metadata_only ? nullptr : assembly_workspace_.recovery(), request_.physical_membership, request_.edition, sealed->dependencies);
   sealed->inventory = std::move(inventory);
   auto output = request_.execution && !reusable ? request_.execution->reserve(BenchmarkResources::handles(1, false), request_.parent_allowance) : BenchmarkAllowance{};
   auto component = CoconutComponentBacking::finish(
    std::move(sealed), std::move(normalized), metadata_only, request_.cancellation, request_.inventory_directory, request_.execution ? &request_.execution->storage() : nullptr, reusable);
   if (request_.records && metadata_only) request_.records->inventories.push_back(component);
   result.push_back(std::move(component));
  }
  throw_if_benchmark_cancelled(request_.cancellation);
  if (!metadata_only) request_.records->retire_native();
  return result;
 }

private:
 static constexpr std::uint64_t kProgressQuantum = 64;
 CoconutOriginalInput original_input(CoconutImageNamespace source) const {
  if (request_.originals && (source == CoconutImageNamespace::CocoTrain || source == CoconutImageNamespace::CocoValidation)) return request_.originals(source, false, {});
  return {{}, true, 0};
 }
 std::shared_ptr<const CoconutNativeLineage> lineage_for(CoconutImageNamespace source, const CoconutMaskRecovery* recovery, std::uint64_t generation) {
  const auto original = recovery ? recovery->original_identity(source) : std::string_view{};
  const std::lock_guard lock(groups_mutex_);
  auto& lineage = lineages_[source];
  if (!lineage || lineage->component.original_generation != generation || lineage->component.original_annotation_identity != original) {
   auto next = std::make_shared<CoconutNativeLineage>();
   auto& component = next->component;
   component.edition = request_.edition;
   component.source = source;
   component.annotation_input_identity = component.input_identity = request_.input_identity;
   component.original_generation = generation;
   component.original_annotation_identity = original;
   if (!original.empty()) component.recovery_policy = kCoconutRecoveryPolicy;
   next->index.source = index_source(source);
   next->index.split = component_split(request_.edition, source);
   lineage = std::move(next);
  }
  return lineage;
 }
 const CoconutComponent* reusable_image(const CoconutPhysicalImage& physical, std::size_t& position) const {
  for (const auto& component : request_.reusable_images) {
   if (component.source() != physical.source) continue;
   const auto images = component.index().images();
   const auto found = std::ranges::lower_bound(images, physical.image_id, {}, &NormalizedImage::source_image_id);
   if (found == images.end() || (*found).source_image_id != physical.image_id) continue;
   position = static_cast<std::size_t>(found - images.begin());
   if (component.inventory_image(position).physical == physical) return &component;
  }
  return nullptr;
 }
 void retain_image(std::size_t position, std::shared_ptr<const CoconutNativeImage> product) {
  {
   const std::lock_guard lock(groups_mutex_);
   request_.records->native(position, std::move(product));
  }
  complete_row();
 }
 void complete_row() {
  const std::lock_guard lock(progress_mutex_);
  ++completed_rows_;
  if (completed_rows_ != 1 && completed_rows_ % kProgressQuantum != 0) return;
  if (request_.progress) request_.progress(completed_rows_);
  if (request_.metadata_only && request_.preparation_progress) request_.preparation_progress(CoconutPreparationStage::Membership, completed_rows_);
 }
 void settle_report(const CoconutNativeImage& image) {
  // Recovery omissions are reported later by the recipe. Metadata, retained
  // products and healthy rows must not flush another producer's report bytes.
  if (image.rejected().degenerate_boxes && !image.lineage().component.recovery_policy && request_.image_terminal) request_.image_terminal();
 }
 void publish_labels(std::shared_ptr<const CoconutNativeImage> image, const BenchmarkSourcePublication& publication, bool provisional = false) {
  if (!request_.execution) return;
  const auto& physical = image->inventory().physical;
  const auto& lineage = image->lineage().component;
  request_.execution->labels_ready(publication, physical.image_id, CoconutNativeImage::labels(image), provisional ? image->input_identity() + ".provisional" : image->input_identity(), lineage.original_generation, provisional ? 0U : lineage.edition == CoconutEdition::Large ? 2U : 1U);
 }
 void defer_native(std::shared_ptr<PendingNativeImage> native) {
  const std::lock_guard lock(groups_mutex_);
  if (recovery_failure_) std::rethrow_exception(recovery_failure_);
  pending_.push_back(std::move(native));
  if (!recovery_controller_.joinable())
   recovery_controller_ = std::jthread([this](std::stop_token stop) {
    try {
     recover_pending(stop);
    } catch (...) {
     const std::lock_guard failure_lock(groups_mutex_);
     recovery_failure_ = std::current_exception();
    }
    pending_changed_.notify_all();
   });
  pending_changed_.notify_one();
 }
 void recover_pending(std::stop_token stop) {
  const auto lanes = request_.execution ? request_.execution->workers() : 1;
  std::vector<std::unique_ptr<CoconutNativeWorkspace>> workers(lanes);
  auto retained_bytes = std::make_unique<std::atomic<std::uint64_t>[]>(lanes);
  std::optional<BenchmarkCompilePipeline::Workspace> workspace;
  if (request_.execution)
   workspace.emplace(*request_.execution, [&](std::size_t lane) noexcept {
    workers[lane].reset();
    retained_bytes[lane].store(0, std::memory_order_relaxed);
   });
  for (;;) {
   std::vector<std::shared_ptr<PendingNativeImage>> batch;
   {
    std::unique_lock lock(groups_mutex_);
    pending_changed_.wait(lock, [&] { return stop.stop_requested() || !pending_.empty() || input_done_; });
    if (stop.stop_requested() || pending_.empty()) return;
    const auto count = std::size_t{1};
    batch.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
     batch.push_back(std::move(pending_.front()));
     pending_.pop_front();
    }
   }
   auto& first = *batch.front();
   const auto admitted = identify(*first.record, true, true);
   if (!admitted) {
    if (request_.execution) request_.execution->retire_labels(request_.physical_membership->identity_root(first.physical.source), first.physical.image_id, first.provisional_dependency);
    retain_image(first.position, {});
    continue;
   }
   first.physical = *admitted;
   first.publication = request_.physical_membership->label_publication(request_.edition, first.physical);
   const auto source = first.physical.source;
   const auto original = (source == CoconutImageNamespace::CocoTrain || source == CoconutImageNamespace::CocoValidation) ? request_.originals(source, true, stop) : CoconutOriginalInput{{}, true, 0};
   {
    const std::lock_guard lock(groups_mutex_);
    ++recovery_ready_;
   }
   if (request_.execution) request_.execution->notify_admission_change();
   CoconutNativeWorkspace sizing(request_.limits, request_.cancellation);
   sizing.originals(original.originals);
   std::uint64_t retained = 0;
   for (std::size_t lane = 0; lane < lanes; ++lane) retained = std::max(retained, retained_bytes[lane].load(std::memory_order_relaxed));
   const auto demand = [&](std::size_t i) {
    const auto& native = *batch[i];
    const auto support = mmltk::common::math::checked_multiply<std::uint64_t>(native.supports.size(), std::uint64_t{1024}, "COCONut recovery support overflow");
    const auto pixels = std::uint64_t{native.dimensions.width} * native.dimensions.height;
    return BenchmarkResources{
     mmltk::common::math::checked_add(
      mmltk::common::math::checked_add(mmltk::common::math::checked_add(retained, support, "COCONut retained recovery overflow"), pixels * 32U, "COCONut carving workspace overflow"),
      sizing.recovery() ? sizing.recovery()->workspace_bytes(*native.record) : 0, "COCONut recovery workspace overflow"),
     1
    };
   };
   const auto recover = [&](std::size_t i) {
    const auto lane = request_.execution ? request_.execution->current_lane() : 0;
    auto& worker = workers[lane];
    if (!worker) worker = std::make_unique<CoconutNativeWorkspace>(request_.limits, request_.cancellation);
    worker->originals(original.originals);
    auto& native = batch[i];
    const auto support = native->read_support();
    worker->borrow_support(*native->record, support, native->dimensions, native, native->unlisted_segment);
    native->product = worker->finish(*native->record, native->physical, native->dimensions, lineage_for(native->physical.source, worker->recovery(), original.generation), request_.rejected_object);
    native->product = CoconutNativeImage::stage(std::move(native->product), request_.inventory_directory, request_.execution ? &request_.execution->storage() : nullptr);
    retained_bytes[lane].store(worker->retained_bytes(), std::memory_order_relaxed);
   };
   if (request_.execution)
    request_.execution->for_each(BenchmarkStage::Recovery, batch.size(), demand, recover, *workspace);
   else
    for (std::size_t i = 0; i < batch.size(); ++i) recover(i);
   for (auto& native : batch) {
    settle_report(*native->product);
    retain_image(native->position, native->product);
    publish_labels(std::move(native->product), native->publication);
   }
   batch.clear();
   {
    const std::lock_guard lock(groups_mutex_);
    ++recovery_settled_;
   }
   pending_changed_.notify_all();
  }
 }
 const CoconutImportRequest& request_;
 Worker archive_worker_;
 CoconutNativeWorkspace assembly_workspace_;
 std::shared_ptr<const CoconutRecoveryOriginals> original_owner_;
 std::deque<std::shared_ptr<PendingNativeImage>> pending_;
 std::condition_variable pending_changed_;
 std::exception_ptr recovery_failure_;
 std::uint64_t recovery_ready_ = 0, recovery_settled_ = 0;
 bool input_done_ = false;
 std::jthread recovery_controller_;
 std::mutex groups_mutex_, progress_mutex_;
 std::uint64_t completed_rows_ = 0, rows_ = 0, parsed_rows_ = 0;
 std::map<std::size_t, std::unique_ptr<Worker>> groups_;
 std::vector<std::unique_ptr<Worker>> idle_;
 std::unordered_set<PhysicalKey, PhysicalKeyHash> offered_;
 std::map<CoconutImageNamespace, std::shared_ptr<const CoconutNativeLineage>> lineages_;
 std::map<CoconutImageNamespace, CoconutComponentBuilder> components_;
 std::vector<std::size_t> source_ordinals_;
};
// One forward discovery cursor plus the archive's generation-bound locators.
// Ready annotation chunks consume their masks immediately; later chunks reuse
// encountered positions instead of repeating an archive census.
class AnnotationMaskStream final {
 struct Mask {
  std::size_t position;
  bool consumed = false;
 };

public:
 AnnotationMaskStream(const CoconutImportRequest& request, CoconutAnnotationRecords& records, Importer& importer, std::uint64_t metadata_workspace = 0)
     : request_(request), records_(records), importer_(importer), archive_(records.archive),
       prefix_(request.edition == CoconutEdition::XLarge ? "coconuts_xlarge/panseg/" : request.edition == CoconutEdition::Large ? "panoptic_object365/" : "panoptic_o365val_v3/"), metadata_workspace_(metadata_workspace) {}
 std::function<void()> retire_metadata;
 ~AnnotationMaskStream() {
  importer_.retire_scratch();
  if (archive_) archive_->pause();
 }
 void consume(std::size_t position) {
  const auto& record = records_.record(position);
  const auto member = prefix_ + record.physical_stem + ".png";
  const auto [mask, inserted] = wanted_.emplace(member, Mask{position});
  if (!inserted) invalid("duplicate offered mask: " + member);
  ensure();
  if (archive_->has_member(member) && !archive_->direct_member(member)) return;
  bool found = archive_->seek(member, request_.cancellation);
  if (!found) {
   if (forward_ && archive_->position() != forward_ordinal_) (void)archive_->seek(*forward_, request_.cancellation);
   while (archive_->next(request_.cancellation)) {
    forward_ = archive_->member_position();
    forward_ordinal_ = archive_->position();
    if (request_.preparation_progress && (forward_ordinal_ == 0 || (forward_ordinal_ + 1) % 64 == 0))
     request_.preparation_progress(CoconutPreparationStage::Archive, forward_ordinal_ + 1);
    if (archive_->member() == member) { found = true; break; }
   }
  }
  if (!found) importer_.consume(record, {}, {}, 0, nullptr, position);
  else consume_current(position);
  mask->second.consumed = true;
 }
 bool consume_known(std::size_t position) {
  const auto& record = records_.record(position);
  const auto member = prefix_ + record.physical_stem + ".png";
  auto& mask = wanted_.try_emplace(member, Mask{position}).first->second;
  if (mask.consumed) return true;
  if (!archive_->has_member(member)) return false;
  const auto cursor = archive_->member_position();
  const auto ordinal = archive_->position();
  const bool current = archive_->member() == member;
  if (!current && !archive_->direct_member(member)) return false;
  const bool retained_cursor = ensure();
  if ((!retained_cursor || !current) && !archive_->seek(member, request_.cancellation)) invalid("discovered mask disappeared from its archive generation");
  consume_current(position);
  mask.consumed = true;
  if (archive_->position() != ordinal) (void)archive_->seek(cursor, request_.cancellation);
  return true;
 }
 void metadata(std::uint64_t bytes, const std::function<void()>& work) {
  if (!archive_ || !request_.execution || !archive_->allowance()) { work(); return; }
  const auto live = mmltk::common::math::checked_add(archive_->retained_workspace_bytes(),
   importer_.retained_workspace_bytes(),
   "COCONut streaming parser overflow");
  const auto allowance = archive_->allowance();
  if (live <= allowance.bytes() && bytes <= allowance.bytes() - live)
   request_.execution->run(BenchmarkStage::Metadata, {bytes, 0}, [&](std::size_t) { work(); }, allowance);
  else {
   pause();
   work();
  }
 }
 void finish() {
  if (!archive_) return;
  std::vector<std::string> pending;
  for (const auto& [member, mask] : wanted_) {
   if (mask.consumed) continue;
   if (!archive_->has_member(member)) {
    importer_.consume(records_.record(mask.position), {}, {}, 0, nullptr, mask.position);
    continue;
   }
   pending.push_back(member);
  }
  // Context-dependent and compressed inputs share one ordered remainder pass.
  // Never replay their prefix independently for each out-of-order annotation.
  archive_->visit_known(pending, [&](std::size_t i) {
   auto& mask = wanted_.at(pending[i]);
   consume_current(mask.position);
   mask.consumed = true;
  }, request_.cancellation);
  archive_->inspect_members([&](std::string_view member, bool conflict) {
   if (!member.starts_with(prefix_) || !member.ends_with(".png")) return;
   if (!wanted_.contains(std::string(member))) {
    if (request_.edition != CoconutEdition::XLarge) invalid("extra mask without annotation: " + std::string(member));
    return;
   }
   if (conflict) invalid("duplicate archive mask: " + std::string(member));
  });
  pause();
 }
private:
 void pause() {
  if (retire_metadata) retire_metadata();
  importer_.retire_scratch();
  archive_->pause();
  paused_ = true;
 }
 void consume_current(std::size_t position) {
  const auto& record = records_.record(position);
  auto png = archive_->read(request_.limits.max_png_bytes, request_.cancellation);
  dataset::MaskDimensions dimensions{};
  const auto demand = importer_.png_workspace(record, position, png, archive_->allowance(), dimensions);
  // Read the image header before reserving decoder/output scratch. Unknown
  // annotation dimensions must not reserve a maximum-size image per record.
  const auto cursor = archive_->member_position();
  if (!ensure(demand)) {
   (void)archive_->seek(cursor, request_.cancellation);
   png = archive_->read(request_.limits.max_png_bytes, request_.cancellation);
  }
  importer_.consume(record, png, archive_->allowance(),
   archive_->retained_workspace_bytes(),
   nullptr, position, dimensions);
 }
 bool ensure(std::uint64_t normalizer = 0) {
  if (normalizer > normalizer_workspace_)
   normalizer_workspace_ = std::max(normalizer, mmltk::common::math::checked_multiply(normalizer_workspace_, 2U, "COCONut normalizer growth overflow"));
  const auto workspace = mmltk::common::math::checked_add(mmltk::common::math::checked_add(0U, metadata_workspace_, "COCONut streaming metadata overflow"),
   mmltk::common::math::checked_add(mmltk::common::math::checked_multiply(request_.limits.max_png_bytes, 2U, "COCONut mask input overflow"), normalizer_workspace_,
    "COCONut mask consumer overflow"), "COCONut streaming envelope overflow");
  if (!archive_) {
   archive_ = std::make_shared<Archive>(request_.mask_archive, request_.execution, workspace, request_.parent_allowance, 1, true, BenchmarkAllowance{}, 1024, 0);
   records_.archive = archive_;
   workspace_ = workspace;
  } else if (paused_ || workspace > workspace_) {
   // Grow image scratch geometrically without multiplying the fixed parser
   // and physical-input envelope when a small image needs more scratch.
   workspace_ = workspace;
   const auto cursor = archive_->member_position();
   if (paused_) {
    archive_->resume(workspace_, 0);
    paused_ = false;
    if (!archive_->member().empty()) (void)archive_->seek(cursor, request_.cancellation);
    return false;
   }
   if (!archive_->try_resize_workspace(workspace_)) {
    pause();
    archive_->resume(workspace_, 0);
    paused_ = false;
    return false;
   }
  }
  return true;
 }
 const CoconutImportRequest& request_;
 CoconutAnnotationRecords& records_;
 Importer& importer_;
 std::shared_ptr<Archive> archive_;
 std::string prefix_;
 std::unordered_map<std::string, Mask> wanted_;
 std::optional<Archive::MemberPosition> forward_;
 std::uint64_t forward_ordinal_ = 0, workspace_ = 0, metadata_workspace_ = 0, normalizer_workspace_ = 0;
 bool paused_ = false;
};
std::uint32_t dimension(const JsonAtom& field, const CoconutImportLimits& limits) {
 if (!field || field.is_null()) return 0;
 const auto value = field.unsigned_integer();
 if (value == 0 || value > limits.max_dimension) invalid("image dimension exceeds admission");
 return static_cast<std::uint32_t>(value);
}
void json_rows(const CoconutImportRequest& request, const PaddedMappedFile& input, std::span<const ByteRange> rows, std::vector<JsonParser>& parsers, BenchmarkCompilePipeline::Workspace* workspace,
 const std::function<void(const JsonRow&, std::size_t)>& consume, const std::function<void(std::size_t, std::size_t)>& ready = {}, bool parallel = false,
 const std::function<bool(std::size_t)>& parsed = {}, AnnotationMaskStream* masks = nullptr) {
 // Source/lane parser capacity survives bounded callbacks under IdleScratch
 // custody. The typed row borrows it only during this synchronous callback.
 for (std::size_t first = 0; first < rows.size();) {
  auto end = first + 1;
  while (end < rows.size() && rows[end].end - rows[first].begin <= (256U << 10)) ++end;
  const auto parse = [&](std::size_t job) {
   auto& storage = parsers[request.execution ? request.execution->current_lane() : 0];
   for (auto i = parallel ? first + job : first; i < (parallel ? first + job + 1 : end); ++i) {
    throw_if_benchmark_cancelled(request.cancellation);
    if (parsed && parsed(i)) continue;
    const auto range = rows[i];
    auto document = storage.parser.iterate(simdjson::padded_string_view(input.data() + range.begin, range.end - range.begin, input.capacity_from(range.begin)));
    JsonConsumption admission{0, static_cast<std::size_t>(request.limits.max_segments) * 32U + 128U, true, request.limits.max_segments};
    auto& row = storage.row;
    row.reset();
    consume_json_value(document.get_value().value(), admission, 2, nullptr, &row);
    if (!row.object) invalid("panoptic row must be an object");
    consume(row, i);
    storage.account();
    if (request.execution && !parallel && (i - first + 1) % 32 == 0) request.execution->cooperate();
   }
  };
  // Previous nested high-water capacity is charged until real retirement.
  // Counting visits only the bounded lanes; each row stores its own O(1) total.
  std::uint64_t retained = 0, largest = 0;
  for (const auto& parser : parsers) retained = std::max(retained, parser.retained_bytes());
  for (auto i = first; i < end; ++i) largest = std::max(largest, std::uint64_t{rows[i].end - rows[i].begin});
  const auto demand = [&](std::size_t job) {
   const auto bytes = parallel ? rows[first + job].end - rows[first + job].begin : largest;
   const auto nested = mmltk::common::math::checked_multiply(
    std::min<std::uint64_t>(request.limits.max_segments, bytes / 2) + 1, std::uint64_t{3 * (sizeof(JsonSegment) + 4 * sizeof(JsonAtom))}, "COCONut JSON nested workspace overflow");
   const auto incoming = mmltk::common::math::checked_add(nested,
    mmltk::common::math::checked_add(std::uint64_t{65536}, mmltk::common::math::checked_multiply(std::uint64_t{bytes}, 32U, "COCONut JSON workspace overflow"), "COCONut JSON workspace overflow"),
    "COCONut JSON workspace overflow");
   return BenchmarkResources{mmltk::common::math::checked_add(retained, incoming, "COCONut retained JSON workspace overflow"), 0};
  };
  const auto work = [&] {
   if (request.execution) request.execution->for_each(BenchmarkStage::Metadata, parallel ? end - first : 1, demand, parse, *workspace);
   else if (parallel) { for (auto i = first; i < end; ++i) parse(i - first); }
   else parse(0);
  };
  if (masks && request.execution) {
   std::uint64_t largest_demand = 0;
   for (std::size_t i = 0; i < (parallel ? end - first : 1); ++i) largest_demand = std::max(largest_demand, demand(i).bytes);
   masks->metadata(largest_demand, work);
  } else work();
  if (ready) ready(first, end);
  first = end;
 }
}
void json_records(const CoconutImportRequest& request, CoconutAnnotationRecords& retained, AnnotationMaskStream* masks = nullptr) {
 throw_if_benchmark_cancelled(request.cancellation);
 const auto& categories = coconut_categories();
 NumericCategoryAdmission category_admission(categories);
 struct ImageRow {
  std::optional<std::uint64_t> id;
  std::string file_name;
  std::string physical_stem;
  std::uint32_t width = 0, height = 0;
 };
 std::vector<ImageRow> images;
 std::unordered_map<std::uint64_t, std::size_t> by_id;
 std::unordered_map<std::string, std::size_t> by_file, by_physical_stem;
 const auto physical_stem = [](const JsonRow& row) {
  std::string stem;
  for (const auto* field : {&row.get<&CoconutJsonRow::object365_file_name>(), &row.get<&CoconutJsonRow::object365_name>(), &row.get<&CoconutJsonRow::file_name>()}) {
   if (!*field || field->is_null()) continue;
   const auto name = field->text();
   if (name.empty() || (field == &row.get<&CoconutJsonRow::file_name>() && !std::filesystem::path(name).filename().string().starts_with("objects365_"))) continue;
   const auto parsed = parse_coconut_objects_member(name);
   if (!stem.empty() && stem != parsed.stem) invalid("contradictory declared Objects365 members");
   stem = parsed.stem;
  }
  return stem;
 };
 auto handles = request.execution ? request.execution->reserve(BenchmarkResources::handles(1, true), request.parent_allowance) : BenchmarkAllowance{};
 const PaddedMappedFile input(request.annotation_json);
 std::vector<JsonParser> parsers(request.execution ? request.execution->workers() : 1);
 std::optional<BenchmarkCompilePipeline::Workspace> parser_workspace;
 if (request.execution) parser_workspace.emplace(*request.execution, [&](std::size_t lane) noexcept { parsers[lane].retire(); });
 auto* workspace = parser_workspace ? &*parser_workspace : nullptr;
 constexpr std::array<std::string_view, 3> fields{"categories", "images", "annotations"};
 const auto category_row = [&](const JsonRow& row, std::size_t) {
  std::optional<std::uint32_t> id;
  std::optional<std::string> name;
  if (row.get<&CoconutJsonRow::id>()) id = mmltk::common::math::checked_cast<std::uint32_t>(row.get<&CoconutJsonRow::id>().unsigned_integer(), "category ID overflow");
  if (row.get<&CoconutJsonRow::name>()) name = std::string(row.get<&CoconutJsonRow::name>().text());
  category_admission.observe(id, name ? std::optional<std::string_view>(*name) : std::nullopt);
 };
 const auto image_row = [&](const JsonRow& row, std::size_t) {
  ImageRow image;
  if (row.get<&CoconutJsonRow::id>()) {
   image.id = row.get<&CoconutJsonRow::id>().unsigned_integer();
   if (!by_id.emplace(*image.id, images.size()).second) invalid("duplicate JSON image ID");
  }
  if (row.get<&CoconutJsonRow::file_name>()) {
   image.file_name = std::string(row.get<&CoconutJsonRow::file_name>().text());
   if (!by_file.emplace(image.file_name, images.size()).second) invalid("duplicate JSON image filename");
  }
  image.physical_stem = physical_stem(row);
  if (!image.physical_stem.empty() && !by_physical_stem.emplace(image.physical_stem, images.size()).second) invalid("duplicate JSON physical image: " + image.physical_stem);
  image.width = dimension(row.get<&CoconutJsonRow::width>(), request.limits);
  image.height = dimension(row.get<&CoconutJsonRow::height>(), request.limits);
  images.push_back(std::move(image));
 };
 std::size_t discovered_rows = 0;
 std::vector<bool> joined_images;
 std::uint64_t segment_ordinal = 0;
 std::size_t ready_records = 0;
 const auto annotations = [&](std::span<const ByteRange> ranges) {
  const auto base = discovered_rows;
  discovered_rows += ranges.size();
  if (retained.size() < discovered_rows) retained.resize(discovered_rows);
  const auto annotation_row = [&](const JsonRow& annotation, std::size_t position) {
   throw_if_benchmark_cancelled(request.cancellation);
   CoconutRecord record;
   record.source_ordinal = base + position;
   std::optional<std::size_t> image_index;
   const auto join = [&](const auto& lookup, const auto& key) {
    const auto found = lookup.find(key);
    if (found == lookup.end()) return;
    if (image_index && *image_index != found->second) invalid("contradictory JSON image joins: " + record.file_name);
    image_index = found->second;
   };
   if (annotation.get<&CoconutJsonRow::image_id>()) {
    record.image_id = annotation.get<&CoconutJsonRow::image_id>().unsigned_integer();
    join(by_id, record.image_id);
   }
   if (annotation.get<&CoconutJsonRow::file_name>()) {
    record.file_name = std::string(annotation.get<&CoconutJsonRow::file_name>().text());
    join(by_file, record.file_name);
   }
   record.physical_stem = physical_stem(annotation);
   if (!record.physical_stem.empty()) join(by_physical_stem, record.physical_stem);
   if (!image_index && request.edition == CoconutEdition::ObjectsValidation) invalid("validation annotation has no image-record join: " + record.file_name);
   if (image_index) {
    const auto& image = images[*image_index];
    if (image.id && annotation.get<&CoconutJsonRow::image_id>() && *image.id != record.image_id) invalid("annotation/image ID disagreement");
    if (!image.physical_stem.empty()) {
     if (!record.physical_stem.empty() && record.physical_stem != image.physical_stem) invalid("contradictory declared Objects365 members");
     record.physical_stem = image.physical_stem;
    }
    record.width = image.width;
    record.height = image.height;
   }
   if (record.physical_stem.empty()) invalid("unresolved offered JSON annotation: " + record.file_name);
   if (!annotation.get<&CoconutJsonRow::image_id>()) {
    const auto declared_id = image_index ? images[*image_index].id : std::nullopt;
    record.image_id = declared_id ? *declared_id : parse_coconut_objects_member(record.physical_stem).id;
   }
   segments_from_json(annotation.get<&CoconutJsonRow::segments_info>(), record, request.limits);
   retained.admit_json(base + position, std::move(record), image_index);
  };
  const auto ready = [&](std::size_t first, std::size_t end) {
   if (request.preparation_progress) request.preparation_progress(CoconutPreparationStage::Annotations, base + end);
   for (auto i = first; i < end; ++i) {
    const auto& row = retained.row(base + i);
    const auto& record = row.record();
    if (row.image_join()) {
     if (joined_images[*row.image_join()]) invalid("multiple annotations join one image row: " + record.file_name);
     joined_images[*row.image_join()] = true;
    }
    retained.json_prefix(base + i, segment_ordinal);
    segment_ordinal = mmltk::common::math::checked_add(segment_ordinal, std::uint64_t{record.segments.size()}, "segment ordinal overflow");
   }
   // A ready chunk can start its physical images before later annotation rows
   // parse. Resolution may wait on sources, so it runs after CPU custody ends.
   while (ready_records < base + end) {
    if (masks) masks->consume(ready_records++);
    else (void)request.physical_membership->identify(request.edition, retained.record(ready_records++));
   }
  };
  json_rows(request, input, ranges, parsers, workspace, annotation_row, ready, true, [&](std::size_t i) { return retained.row(base + i).complete(); }, masks);
 };
 bool categories_complete = false, images_complete = false;
 std::vector<ByteRange> pending_annotations;
 discover_json_arrays(input, fields, true, [&](std::size_t field, std::span<const ByteRange> rows, bool complete) {
  if (field == 0) {
   json_rows(request, input, rows, parsers, workspace, category_row, {}, false, {}, masks);
   if (complete) {
    category_admission.complete();
    categories_complete = true;
   }
  } else if (field == 1) {
   json_rows(request, input, rows, parsers, workspace, image_row, {}, false, {}, masks);
   if (request.preparation_progress) request.preparation_progress(CoconutPreparationStage::Images, images.size());
   if (complete) {
    joined_images.assign(images.size(), false);
    images_complete = true;
   }
  } else if (categories_complete && images_complete)
   annotations(rows);
  else
   pending_annotations.insert(pending_annotations.end(), rows.begin(), rows.end());
  if (categories_complete && images_complete && !pending_annotations.empty()) {
   annotations(pending_annotations);
   std::vector<ByteRange>().swap(pending_annotations);
  }
 }, request.cancellation, request.execution, [&](const std::function<void()>& work) {
  if (masks) masks->metadata(65536, work);
  else work();
 });
 for (std::size_t begin = 0; begin < images.size(); begin += 256) {
  const auto check_joins = [&](std::size_t) {
   for (auto i = begin; i < std::min(images.size(), begin + 256); ++i) {
    throw_if_benchmark_cancelled(request.cancellation);
    if (!joined_images[i]) invalid("offered JSON image has no annotation/mask join: " + images[i].file_name);
   }
  };
  if (request.execution)
   request.execution->run(BenchmarkStage::Metadata, {}, check_joins);
  else
   check_joins(0);
 }
 if (discovered_rows != retained.size()) invalid("retained JSON row count mismatch");
 retained.document_complete = true;
}
void xlarge_records(const CoconutImportRequest& request, CoconutAnnotationRecords& retained, Importer* importer = nullptr) {
 const auto nested = mmltk::common::math::checked_multiply(
  std::min<std::uint64_t>(request.limits.max_segments, 8ULL << 20) + 1, std::uint64_t{3 * (sizeof(JsonSegment) + 4 * sizeof(JsonAtom))}, "COCONut XL nested storage overflow");
 const auto workspace = mmltk::common::math::checked_add(
  nested, mmltk::common::math::checked_add(576ULL << 20, 0U, "COCONut discovery consumer envelope overflow"), "COCONut XL parser workspace overflow");
 auto owned = retained.archive;
 if (owned)
  owned->resume(workspace, 0);
 else
  owned = std::make_shared<Archive>(request.mask_archive, request.execution, workspace, request.parent_allowance, 1, true, BenchmarkAllowance{}, 1024, 0);
 auto& archive = *owned;
 retained.archive = owned;
 std::unique_ptr<AnnotationMaskStream> masks;
 if (importer) masks = std::make_unique<AnnotationMaskStream>(request, retained, *importer, workspace - 0U);
 std::unordered_map<std::string, std::size_t> records;
 records.reserve(retained.size());
 std::unordered_set<std::string> encountered;
 for (std::size_t i = 0; i < retained.size(); ++i) records.emplace(retained.record(i).physical_stem, i);
 simdjson::ondemand::parser parser;
 std::vector<char> padded;
 JsonArray<JsonSegment> segments;
 if (masks) masks->retire_metadata = [&] {
  parser = simdjson::ondemand::parser{};
  std::vector<char>().swap(padded);
  segments = {};
 };
 constexpr std::string_view prefix = "coconuts_xlarge/panseg_info/";
 while (archive.next(request.cancellation)) {
  if (request.preparation_progress && (archive.position() == 0 || (archive.position() + 1) % 64 == 0)) request.preparation_progress(CoconutPreparationStage::Archive, archive.position() + 1);
  if (masks && archive.regular() && archive.member().starts_with("coconuts_xlarge/panseg/") && archive.member().ends_with(".png")) {
   const auto stem = std::filesystem::path(archive.member()).stem().string();
   if (const auto found = records.find(stem); found != records.end()) (void)masks->consume_known(found->second);
   continue;
  }
  if (!archive.regular() || !archive.member().starts_with(prefix) || !archive.member().ends_with(".json")) continue;
  const auto name = parse_coconut_objects_member(archive.member());
  if (name.source != CoconutImageNamespace::Objects365V2 || archive.member() != std::string(prefix) + name.stem + ".json") invalid("unsupported XL info member: " + archive.member());
  if (!encountered.insert(name.stem).second) invalid("duplicate XL info member: " + archive.member());
  if (const auto found = records.find(name.stem); found != records.end()) {
   if (masks) (void)masks->consume_known(found->second);
   else (void)request.physical_membership->identify(request.edition, retained.record(found->second));
   continue;
  }
  CoconutRecord record;
  record.image_id = name.id;
  record.physical_stem = name.stem;
  const auto bytes = archive.read(16U * 1024U * 1024U, request.cancellation);
  archive.cpu([&] {
   padded.resize(bytes.size() + simdjson::SIMDJSON_PADDING);
   std::memcpy(padded.data(), bytes.data(), bytes.size());
   auto document = parser.iterate(simdjson::padded_string_view(padded.data(), bytes.size(), padded.size()));
   JsonConsumption admission{0, 0, false, request.limits.max_segments};
   segments.reset();
   consume_json_value(document.get_value().value(), admission, 0, nullptr, nullptr, nullptr, &segments);
   segments_from_json(segments, record, request.limits);
  });
  const auto position = retained.size();
  record.source_ordinal = position;
  const auto [inserted, unique] = records.emplace(name.stem, position);
  if (!unique) invalid("duplicate XL info member: " + archive.member());
  retained.resize(position + 1);
  retained.admit_json(position, std::move(record));
  if (request.preparation_progress && (position == 0 || (position + 1) % 64 == 0)) request.preparation_progress(CoconutPreparationStage::Annotations, position + 1);
  // Resolve real image work as each JSON record arrives. XL source ordering is
  // assigned below; its full physical stem is already a stable logical key.
  if (masks) (void)masks->consume_known(inserted->second);
  else (void)request.physical_membership->identify(request.edition, retained.record(inserted->second));
 }
 if (encountered.size() != records.size()) invalid("retained XL row count mismatch");
 throw_if_benchmark_cancelled(request.cancellation);
 if (masks) {
  for (std::size_t i = 0; i < retained.size(); ++i) (void)masks->consume_known(i);
  masks->finish();
  masks->retire_metadata = {};
  importer->canonical_ordinals(retained.seal_xlarge_order());
 } else (void)retained.seal_xlarge_order();
}
}  // namespace
CoconutComponent::CoconutComponent(std::shared_ptr<const CoconutComponentBacking> backing, NormalizedAnnotationReadView index, std::shared_ptr<CoconutInventorySeal> seal, bool membership)
    : backing_(std::move(backing)), index_(std::move(index)), seal_(std::move(seal)), membership_(membership), original_generation_(backing_->original_generation) {}
CoconutEdition CoconutComponent::edition() const noexcept { return backing_->edition; }
CoconutImageNamespace CoconutComponent::source() const noexcept { return backing_->source; }
const std::string& CoconutComponent::input_identity() const noexcept { return backing_->input_identity; }
BenchmarkLabelInput CoconutComponent::labels(std::size_t image) const {
 if (membership_ || backing_->metadata_only) invalid("membership has no normalized labels");
 return BenchmarkLabelInput(index_, image);
}
CoconutComponent CoconutComponent::with_original(const CoconutOriginalInput& original) const {
 if (membership_ || backing_->metadata_only || !original.terminal || !original.generation || (source() != CoconutImageNamespace::CocoTrain && source() != CoconutImageNamespace::CocoValidation))
  invalid("component has no admitted original generation");
 const auto identity = original.originals ? original.originals->identity(source()) : std::string_view{};
 if (identity != backing_->original_annotation_identity) invalid("component has a different original input");
 auto result = *this;
 result.original_generation_ = original.generation;
 return result;
}
bool CoconutComponent::source_extent_complete() const { return backing_->source_extent_complete; }
bool CoconutComponent::source_ordinals_complete(std::size_t image) const {
 return !std::ranges::binary_search(backing_->incomplete_ordinal_images, index_.image(image).source_image_id);
}
std::pair<std::uint64_t, std::uint64_t> CoconutComponent::source_extent() const { return {backing_->source_rows, backing_->source_segments}; }
bool CoconutComponent::required_labels_valid(std::size_t image) const {
 return !std::ranges::binary_search(backing_->rejected_images, index_.image(image).source_image_id);
}
std::uint64_t CoconutComponent::compiled_ordinal_base(std::size_t image) const {
 return backing_->compiled_ordinal_bases.empty() ? 0 : backing_->compiled_ordinal_bases.at(index_.source_position(image));
}
const std::string& CoconutComponent::image_input_identity(std::size_t image) const { return backing_->image_dependencies.at(index_.source_position(image)); }
bool CoconutComponent::matches_inputs(std::string_view annotations, const CoconutPhysicalMembership& physical, const CoconutMaskRecovery* recovery, bool current_physical) const {
 return input_identity() == coconut_component_input_identity(annotations, source(), recovery, &physical, edition(), backing_->dependencies, current_physical);
}
std::uint32_t CoconutComponent::recovery_policy() const noexcept { return membership_ ? 0 : backing_->recovery_policy; }
std::string_view CoconutComponent::original_annotation_identity() const noexcept { return membership_ ? std::string_view{} : backing_->original_annotation_identity; }
const CoconutInventoryImage& CoconutComponent::inventory_image(std::size_t position) const { return backing_->inventory->at(index_.source_position(position)); }
const CoconutRecoveryImage& CoconutComponent::recovery_image(std::size_t position) const {
 if (!recovery_policy()) throw std::out_of_range("COCONut membership has no recovery claims");
 return backing_->recovery.at(index_.source_position(position));
}
std::shared_ptr<const NormalizedAnnotationCompletion> CoconutComponent::completion() const {
 const std::lock_guard lock(backing_->completion_mutex);
 return backing_->completion;
}
CoconutComponent CoconutComponent::membership() const {
 auto result = *this;
 result.membership_ = true;
 return result;
}
CoconutComponent CoconutComponent::select_images(std::vector<std::size_t> positions, Cancellation cancellation) const {
 auto result = *this;
 result.index_ = index_.select_images(std::move(positions), std::nullopt, cancellation);
 if (result.index_.image_count() == index_.image_count()) return result;
 // Selection identities include the underlying recovery facts even when the
 // caller asks only for membership. Neither selection nor publication mutates
 // the full backing's completion or once-only admission.
 const bool membership = result.membership_;
 result.membership_ = false;
 result.seal_ = seal_inventory(result, cancellation, seal_->directory, nullptr);
 result.membership_ = membership;
 result.index_.annotation_sha256 = result.seal_->identity;
 return result;
}
CoconutComponent CoconutComponentBacking::finish(std::shared_ptr<CoconutComponentBacking> backing, NormalizedAnnotationIndex normalized, bool metadata_only, Cancellation cancellation,
 const std::filesystem::path& directory, StorageReservationPool* storage, const CoconutComponent* reuse) {
 admit_component_metadata(*backing, normalized, normalized.images.size(), backing->inventory->size(), backing->recovery.size());
 const auto expected_identity = std::exchange(normalized.annotation_sha256, {});
 normalized.completion.reset();
 backing->index = NormalizedAnnotationReadView(normalized);
 backing->metadata_only = metadata_only;
 if (reuse) {
  backing->inventory = reuse->backing_->inventory;
  backing->seal = reuse->seal_;
 } else
  backing->seal = seal_inventory(loaded(backing), cancellation, directory, storage);
 if (!expected_identity.empty() && expected_identity != backing->seal->identity) invalid("component inventory/index identity mismatch");
 normalized.annotation_sha256 = backing->seal->identity;
 backing->index = NormalizedAnnotationReadView(std::move(normalized));
 if (!metadata_only) std::call_once(backing->full_admission, [] {});
 auto result = loaded(std::move(backing));
 return metadata_only ? result.membership() : result;
}
CoconutComponent CoconutComponentBuilder::finish(Cancellation cancellation, const std::filesystem::path& directory, StorageReservationPool* storage) && {
 admit_component_metadata(*this, index, index.images.size(), inventory.size(), recovery.size());
 auto backing = std::make_shared<CoconutComponentBacking>();
 static_cast<CoconutComponentMetadata&>(*backing) = std::move(static_cast<CoconutComponentMetadata&>(*this));
 NormalizedAnnotationAssembler assembly(index, index.images.size(), index.boxes.size(), index.mask_rle_pairs.size(), cancellation);
 ComponentInventoryAdmission admission(backing->source);
 std::size_t boxes = 0, runs = 0;
 for (std::size_t position = 0; position < index.images.size(); ++position) {
  throw_if_benchmark_cancelled(cancellation);
  const auto& image = index.images[position];
  admission.image(inventory[position], image);
  backing->dependencies.add(inventory[position].physical);
  backing->image_dependencies.push_back(coconut_image_input_identity(backing->annotation_input_identity, inventory[position].physical, backing->original_annotation_identity));
  if (image.first_box != boxes || image.box_count > index.boxes.size() - boxes) invalid("invalid component box extent");
  const auto* facts = backing->recovery_policy ? &recovery[position] : nullptr;
  if (facts) admit_recovery_image(*facts, image);
  ComponentRecoveryAdmission join(facts);
  assembly.begin_image(image);
  for (const auto& box : std::span(index.boxes).subspan(boxes, image.box_count)) {
   if (box.mask_rle_offset != runs || box.mask_rle_pairs > index.mask_rle_pairs.size() - runs) invalid("invalid component mask extent");
   join.box(box);
   assembly.append_box(box, std::span(index.mask_rle_pairs).subspan(runs, box.mask_rle_pairs));
   runs += box.mask_rle_pairs;
  }
  join.finish();
  boxes += image.box_count;
 }
 if (boxes != index.boxes.size() || runs != index.mask_rle_pairs.size()) invalid("component has unreferenced normalized records");
 backing->inventory = std::make_shared<const std::vector<CoconutInventoryImage>>(std::move(inventory));
 backing->recovery = std::move(recovery);
 return CoconutComponentBacking::finish(std::move(backing), assembly.finish(), false, cancellation, directory, storage);
}
std::string coconut_component_input_identity(std::string_view base, CoconutImageNamespace source, const CoconutMaskRecovery* recovery, const CoconutPhysicalMembership* physical,
 CoconutEdition edition, const CoconutPhysicalDependencies& dependencies, bool current) {
 const auto original = recovery ? recovery->original_identity(source) : std::string_view{};
 return component_input_identity(base, source, physical ? physical->dependency_identity(edition, source, dependencies, current) : std::string{}, original.empty() ? 0 : kCoconutRecoveryPolicy, original);
}
std::vector<CoconutComponent> merge_coconut_components(std::span<const CoconutComponentPart> parts, std::string_view identity, const CoconutPhysicalMembership& physical,
 const std::filesystem::path& directory, Cancellation cancellation, StorageReservationPool* storage, BenchmarkCompilePipeline* execution, const CoconutImageRejection& rejected) {
 struct Row { const CoconutComponent* component; std::size_t position; std::optional<std::uint64_t> rows, segments; };
 std::map<CoconutImageNamespace, std::vector<Row>> sources;
 for (const auto& part : parts) for (const auto& component : part.components)
  for (std::size_t i = 0; i < component.index().image_count(); ++i) sources[component.source()].push_back({&component, i, part.row_base, part.segment_base});
 std::vector<CoconutComponent> result;
 for (auto& [source, rows] : sources) {
  std::ranges::sort(rows, {}, [](const Row& row) { return row.component->index().image(row.position).source_image_id; });
  settle_annotation_identities(rows, [](const Row& row) -> const CoconutInventoryImage& { return row.component->inventory_image(row.position); },
   [](const Row& row) -> std::string_view { return row.component->image_input_identity(row.position); },
   [&](const Row& row, std::string_view reason) { if (rejected) rejected(row.component->inventory_image(row.position), reason); }, true, physical, execution, cancellation);
  if (rows.empty()) continue;
  const auto& first = *rows.front().component;
  auto backing = std::make_shared<CoconutComponentBacking>();
  static_cast<CoconutComponentMetadata&>(*backing) = static_cast<const CoconutComponentMetadata&>(*first.backing_);
  backing->annotation_input_identity = identity;
  backing->source_extent_complete = std::ranges::all_of(parts, [](const auto& part) {
   return part.row_base && part.segment_base && (part.source_extent || (!part.components.empty() && std::ranges::all_of(part.components, &CoconutComponent::source_extent_complete)));
  });
  if (backing->source_extent_complete) for (const auto& part : parts) {
   auto extent = part.source_extent.value_or(std::pair<std::uint64_t, std::uint64_t>{});
   if (!part.source_extent) for (const auto& component : part.components) {
    extent.first = std::max(extent.first, component.source_extent().first);
    extent.second = std::max(extent.second, component.source_extent().second);
   }
   const auto [count, segments] = extent;
   backing->source_rows = std::max(backing->source_rows, mmltk::common::math::checked_add(*part.row_base, count, "COCONut source row extent overflow"));
   backing->source_segments = std::max(backing->source_segments, mmltk::common::math::checked_add(*part.segment_base, segments, "COCONut source segment extent overflow"));
  }
  std::uint64_t boxes = 0, runs = 0;
  for (const auto& row : rows) {
   const auto& image = row.component->index().image(row.position);
   boxes += image.box_count;
   for (const auto& box : row.component->index().storage().boxes.subspan(image.first_box, image.box_count)) runs += box.mask_rle_pairs;
  }
  NormalizedAnnotationMetadata metadata = first.index();
  metadata.annotation_sha256.clear();
  metadata.completion.reset();
  metadata.rejected = {};
  for (const auto& part : parts) for (const auto& component : part.components) if (component.source() == source)
   mmltk::frameworks::reflection::visit_materialized_members<AnnotationRejectCounts>([&]<class Declaration>(const auto&) {
    metadata.rejected.*Declaration::pointer = mmltk::common::math::checked_add(metadata.rejected.*Declaration::pointer, component.index().rejected.*Declaration::pointer, "COCONut rejected count overflow");
   });
  NormalizedAnnotationAssembler assembled(metadata, rows.size(), boxes, runs, cancellation, directory, storage);
  auto inventory = std::make_shared<std::vector<CoconutInventoryImage>>();
  inventory->reserve(rows.size());
  for (const auto& row : rows) {
   throw_if_benchmark_cancelled(cancellation);
   const auto& component = *row.component;
   if (component.recovery_policy() != first.recovery_policy() || component.original_annotation_identity() != first.original_annotation_identity() || component.original_generation() != first.original_generation())
    throw CoconutOriginalChanged(source);
   auto physical_row = component.inventory_image(row.position);
   const auto segment_base = component.source_ordinals_complete(row.position) ? row.segments : std::nullopt;
   const bool complete = row.rows && segment_base;
   if (row.rows) physical_row.source_ordinal = mmltk::common::math::checked_add(physical_row.source_ordinal, *row.rows, "COCONut shard row ordinal overflow");
   if (!complete) backing->incomplete_ordinal_images.push_back(physical_row.physical.image_id);
   inventory->push_back(physical_row);
   backing->dependencies.add(physical_row.physical);
   backing->image_dependencies.push_back(component.image_input_identity(row.position));
   backing->compiled_ordinal_bases.push_back(mmltk::common::math::checked_add(component.compiled_ordinal_base(row.position), segment_base.value_or(0), "COCONut compiled ordinal overflow"));
   if (!component.required_labels_valid(row.position)) backing->rejected_images.push_back(physical_row.physical.image_id);
   const auto& image = component.index().image(row.position);
   assembled.begin_image(image);
   for (auto box : component.index().storage().boxes.subspan(image.first_box, image.box_count)) {
    if (segment_base) box.source_ordinal = mmltk::common::math::checked_add(box.source_ordinal, *segment_base, "COCONut shard segment ordinal overflow");
    if (!complete) box.flags |= kAnnotationOrdinalIncomplete;
    assembled.append_box(box, component.index().storage().mask_rle_pairs.subspan(box.mask_rle_offset, box.mask_rle_pairs));
   }
   if (backing->recovery_policy) {
    auto recovery = component.recovery_image(row.position);
    for (auto* objects : {&recovery.objects, &recovery.omissions}) for (auto& object : *objects)
     if (segment_base) object.source_ordinal = mmltk::common::math::checked_add(object.source_ordinal, *segment_base, "COCONut shard recovery ordinal overflow");
    backing->recovery.push_back(std::move(recovery));
   }
  }
  backing->input_identity = component_input_identity(identity, source, physical.dependency_identity(first.edition(), source, backing->dependencies), backing->recovery_policy, backing->original_annotation_identity);
  backing->inventory = std::move(inventory);
  result.push_back(CoconutComponentBacking::finish(std::move(backing), assembled.finish(), false, cancellation, directory, storage));
 }
 return result;
}
void CoconutAnnotationRecords::admit_metadata(std::size_t position, CoconutRecord record) {
 auto& row = rows_.at(position);
 if (row.metadata_ready()) return;
 row.record_ = std::move(record);
 row.state_ = Row::State::Metadata;
 metadata_rows_.fetch_add(1, std::memory_order_relaxed);
}
void CoconutAnnotationRecords::admit_segments(std::size_t position, std::vector<CoconutSegment> segments, std::uint64_t local_ordinal) {
 auto& row = rows_.at(position);
 if (row.complete()) return;
 if (!row.metadata_ready()) throw std::logic_error("COCONut: segments have no admitted image row");
 row.record_.segments = CoconutSegments(std::move(segments));
 row.record_.segments.stage(staging_directory, storage);
 row.record_.first_segment_ordinal = local_ordinal;
 row.segment_ordinal_ = local_ordinal;
 row.state_ = Row::State::Complete;
}
void CoconutAnnotationRecords::adopt(std::vector<CoconutRecord> records) {
 std::vector<Row> rows(records.size());
 for (std::size_t i = 0; i < records.size(); ++i) {
  rows[i].segment_ordinal_ = records[i].first_segment_ordinal;
  rows[i].record_ = std::move(records[i]);
  rows[i].state_ = Row::State::Complete;
 }
 rows_ = std::move(rows);
 metadata_rows_.store(rows_.size(), std::memory_order_relaxed);
 prefixes_sealed_ = false;
 document_complete = true;
}
void CoconutAnnotationRecords::admit_json(std::size_t position, CoconutRecord record, std::optional<std::size_t> image_join) {
 record.segments.stage(staging_directory, storage);
 auto& row = rows_.at(position);
 if (row.complete()) return;
 if (!row.metadata_ready()) metadata_rows_.fetch_add(1, std::memory_order_relaxed);
 row.record_ = std::move(record);
 row.image_join_ = image_join;
 row.segment_ordinal_ = row.record_.first_segment_ordinal;
 row.state_ = Row::State::Complete;
}
void CoconutAnnotationRecords::json_prefix(std::size_t position, std::uint64_t ordinal) {
 auto& row = rows_.at(position);
 if (row.native_ && row.record_.first_segment_ordinal != ordinal) throw std::logic_error("COCONut: cannot change a published JSON ordinal");
 row.record_.first_segment_ordinal = row.segment_ordinal_ = ordinal;
}
std::vector<std::size_t> CoconutAnnotationRecords::seal_xlarge_order() {
 // Validate before moving any owned rows. Sorting and ordinal assignment then
 // form one nonallocating commit; cancellation cannot leave a partial move.
 std::uint64_t ordinal = 0;
 std::vector<std::size_t> order(rows_.size(), SIZE_MAX);
 for (const auto& row : rows_) {
  if (!row.complete() || row.record_.source_ordinal >= order.size() || order[row.record_.source_ordinal] != SIZE_MAX) invalid("cannot order incomplete or duplicate annotation rows");
  order[row.record_.source_ordinal] = 0;
  ordinal = mmltk::common::math::checked_add(ordinal, std::uint64_t{row.record_.segments.size()}, "segment ordinal overflow");
 }
 std::ranges::sort(rows_, {}, [](const Row& row) -> const std::string& { return row.record_.physical_stem; });
 ordinal = 0;
 for (std::size_t i = 0; i < rows_.size(); ++i) {
  auto& row = rows_[i];
  order[row.record_.source_ordinal] = i;
  row.record_.source_ordinal = i;
  row.record_.first_segment_ordinal = row.segment_ordinal_ = ordinal;
  ordinal += row.record_.segments.size();
 }
 document_complete = true;
 return order;
}
void CoconutAnnotationRecords::native(std::size_t position, std::shared_ptr<const CoconutNativeImage> product) {
 auto& row = rows_.at(position);
 if (product && !row.complete()) throw std::logic_error("COCONut: native image has no complete annotation row");
 row.native_ = std::move(product);
}
void CoconutAnnotationRecords::retire_native() noexcept {
 for (auto& row : rows_) row.native_.reset();
}
void CoconutAnnotationRecords::discard() noexcept {
 archive.reset();
 parquet.reset();
 groups.clear();
 inventories.clear();
 std::vector<Row>().swap(rows_);
 metadata_rows_.store(0, std::memory_order_relaxed);
 identity.clear();
 document_complete = false;
 prefixes_sealed_ = false;
}
std::shared_ptr<BenchmarkArchive> discover_coconut_masks(const CoconutImportRequest& request, std::stop_token stop) {
 auto archive = std::make_shared<Archive>(request.mask_archive, request.execution, 0, request.parent_allowance, 1, true, BenchmarkAllowance{}, 1024, 0);
 // Keep only the archive owner's compact locators. The eventual annotation
 // consumer borrows this same generation and its forward compressed context.
 try {
  while (!stop.stop_requested() && archive->next(request.cancellation)) {
   if (request.preparation_progress && (archive->position() == 0 || (archive->position() + 1) % 64 == 0))
    request.preparation_progress(CoconutPreparationStage::Archive, archive->position() + 1);
  }
  archive->pause();
  return archive;
 } catch (...) {
  archive->pause();
  throw;
 }
}
std::vector<CoconutComponent> import_coconut_annotations(const CoconutImportRequest& supplied) {
 auto retained = supplied.records ? supplied.records : std::make_shared<CoconutAnnotationRecords>();
 auto request = supplied;
 request.records = retained;
 retained->staging_directory = request.inventory_directory;
 retained->storage = request.execution ? &request.execution->storage() : nullptr;
 (void)coconut_release_component(request.edition);
 Importer importer(request);
 if (request.finish_retained) {
  if (!request.accept_partial) throw std::logic_error("retained annotation settlement requires partial admission");
  return importer.finish(true);
 }
 try {
  if (request.edition == CoconutEdition::Base || request.edition == CoconutEdition::RelabeledValidation) {
   if (retained->identity != request.input_identity) retained->discard();
   retained->identity = request.input_identity;
   if (request.parquet_shards.empty()) invalid("missing Parquet shards");
   read_coconut_parquet(request.parquet_shards, request.limits, request.cancellation,
    [&](std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) { importer.consume_group(group, record, input); }, request.metadata_only, request.execution,
    [&](std::size_t group) { importer.retire_group(group); }, request.parent_allowance, retained.get(),
    [&](const CoconutRecord& record) { return importer.maximum_workspace(record); }, [&](const BenchmarkAllowance& producer) {
    importer.retire_input(producer);
   }, [&](const std::function<void()>& release) { importer.settle_ready_recovery(release); }, [&](const CoconutRecord& record, const BenchmarkAllowance& allowance) {
    return importer.reusable_native(record, allowance);
   });
   importer.collect_groups();
   retained->identity = request.input_identity;
  } else {
   if (!retained) retained = std::make_shared<CoconutAnnotationRecords>();
   if (retained->identity != request.input_identity) {
    retained->discard();
    retained->identity = request.input_identity;
   }
   if (request.metadata_only) {
    if (!retained->document_complete) {
     if (request.edition == CoconutEdition::XLarge)
      xlarge_records(request, *retained);
     else
      json_records(request, *retained);
    }
    for (std::size_t i = 0; i < retained->size(); ++i) importer.consume(retained->record(i), {}, retained->archive ? retained->archive->allowance() : BenchmarkAllowance{}, 0, nullptr, i);
    if (retained->archive) retained->archive->pause();
   } else if (request.mask_archive.empty() && request.accept_partial) {
    if (!retained->document_complete) json_records(request, *retained);
    for (std::size_t i = 0; i < retained->size(); ++i) importer.consume(retained->record(i), {}, {}, 0, nullptr, i);
   } else if (request.edition == CoconutEdition::XLarge && !retained->document_complete) {
    xlarge_records(request, *retained, &importer);
   } else {
    AnnotationMaskStream masks(request, *retained, importer);
    if (!retained->document_complete)
     json_records(request, *retained, &masks);
    else
     for (std::size_t i = 0; i < retained->size(); ++i) masks.consume(i);
    masks.finish();
   }
  }
  if (retained->archive) retained->archive->pause();
  return importer.finish();
 } catch (const AnnotationDocumentRejected& error) {
  if (!request.accept_partial) throw CoconutInputError(error.what());
  if (request.rejected_input) request.rejected_input(error.what());
  if (retained->archive) retained->archive->pause();
  return importer.finish(true);
 } catch (const CoconutInputError& error) {
  if (!request.accept_partial) throw;
  if (request.rejected_input) request.rejected_input(error.what());
  if (retained->archive) retained->archive->pause();
  return importer.finish(true);
 } catch (const BenchmarkArchiveError& error) {
  if (!request.accept_partial) throw CoconutInputError(error.what());
  if (request.rejected_input) request.rejected_input(error.what());
  if (retained->archive) retained->archive->pause();
  return importer.finish(true);
 } catch (...) {
  if (retained->archive) retained->archive->pause();
  importer.stop_recovery();
  throw;
 }
}
std::uint64_t reconcile_coconut_extensions(std::vector<CoconutComponent>& components, Cancellation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 auto replacement = components;
 std::unordered_set<PhysicalKey, PhysicalKeyHash> large, xlarge;
 for (const auto& component : components)
  if (component.edition() == CoconutEdition::Large) {
   for (const auto& image : component.inventory()) {
    throw_if_benchmark_cancelled(cancellation);
    if (!large.insert(physical_key(image.physical.source, image.physical.image_id)).second) invalid("duplicate Large physical image");
   }
  }
 std::uint64_t removed = 0;
 for (auto& component : replacement)
  if (component.edition() == CoconutEdition::XLarge) {
   std::vector<std::size_t> retained;
   for (std::size_t i = 0; i < component.inventory().size(); ++i) {
    throw_if_benchmark_cancelled(cancellation);
    const auto& image = component.inventory()[i].physical;
    const auto key = physical_key(image.source, image.image_id);
    if (!xlarge.insert(key).second) invalid("duplicate XL physical image");
    if (large.contains(key))
     ++removed;
    else
     retained.push_back(i);
   }
   if (retained.size() != component.inventory().size()) component = component.select_images(std::move(retained), cancellation);
  }
 throw_if_benchmark_cancelled(cancellation);
 components = std::move(replacement);
 return removed;
}
void store_coconut_component(const std::filesystem::path& index_path, const CoconutComponent& component, Cancellation cancellation, StorageReservationPool* storage) {
 if (component.membership_ || component.backing_->metadata_only) invalid("membership cannot publish a full component");
 admit_coconut_component(component, cancellation);
 // Reassembled compact provenance can describe an already published immutable
 // cache. Preserve its inode and completion rather than rewriting equal bytes.
 if (auto cached = load_coconut_component(index_path, component.edition(), component.source(), component.input_identity(), cancellation)) {
  if (!component.index().selected() && cached->index().annotation_sha256 == component.index().annotation_sha256 && cached->seal_->identity == component.seal_->identity &&
      cached->source_extent() == component.source_extent() && cached->source_extent_complete() == component.source_extent_complete() &&
      cached->backing_->incomplete_ordinal_images == component.backing_->incomplete_ordinal_images && cached->backing_->rejected_images == component.backing_->rejected_images) {
   const std::lock_guard lock(component.backing_->completion_mutex);
   component.backing_->completion = cached->completion();
   return;
  }
 }
 const auto& identity = component.seal_->identity;
 const auto inventory_path = std::filesystem::path(index_path.string() + ".inventory");
 auto& seal = *component.seal_;
 const std::lock_guard lock(seal.mutex);
 seal.publish(inventory_path, cancellation, storage);
 throw_if_benchmark_cancelled(cancellation);
 Json manifest = Json::object();
 std::vector<std::uint64_t> rejected_images, incomplete_images;
 for (std::size_t i = 0; i < component.index().image_count(); ++i) {
  const auto id = component.index().image(i).source_image_id;
  if (!component.required_labels_valid(i)) rejected_images.push_back(id);
  if (!component.source_ordinals_complete(i)) incomplete_images.push_back(id);
 }
 const CoconutCompletionFacts facts{
  component.edition(), component.source(), component.input_identity(), std::string(kCoconutNormalizationRevision), identity, component.inventory().size(), component.recovery_policy(),
  std::string(component.original_annotation_identity()), component.recovery().size(), std::move(rejected_images), component.backing_->source_rows, component.backing_->source_segments, component.backing_->source_extent_complete, std::move(incomplete_images)
 };
 auto extension = Json::object();
 mmltk::frameworks::reflection::visit_materialized_members<CoconutCompletionFacts>([&]<class Declaration>(const auto& field) {
  const auto& value = facts.*Declaration::pointer;
  if constexpr (mmltk::frameworks::reflection::OptionalValue<typename Declaration::member_type>::value) {
   if (value) extension[field.member_name] = *value;
  } else
   extension[field.member_name] = value;
 });
 manifest["coconut"] = std::move(extension);
 throw_if_benchmark_cancelled(cancellation);
 // Persistence is an explicit assembly boundary. The view and its original
 // backing keep their cache identity; the newly written product gets its own.
 NormalizedAnnotationIndex persisted;
 if (component.index().selected()) {
  NormalizedAnnotationAssembler assembly(component.index(), component.index().image_count(), component.index().box_count(), component.index().run_count(), cancellation);
  const auto& source = component.index().storage();
  for (const auto& image : component.index().images()) {
   assembly.begin_image(image);
   for (const auto& box : source.boxes.subspan(static_cast<std::size_t>(image.first_box), image.box_count))
    assembly.append_box(box, source.mask_rle_pairs.subspan(static_cast<std::size_t>(box.mask_rle_offset), box.mask_rle_pairs));
  }
  persisted = assembly.finish();
 } else {
  persisted = component.index().storage();
  static_cast<NormalizedAnnotationMetadata&>(persisted) = component.index();
 }
 const auto completion = store_normalized_annotation_index(index_path, persisted, cancellation, {}, storage, manifest);
 if (!component.index().selected()) {
  const std::lock_guard completion_lock(component.backing_->completion_mutex);
  component.backing_->completion = completion;
 }
}
void admit_coconut_component(const CoconutComponent& component, Cancellation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 const auto& backing = *component.backing_;
 if (backing.metadata_only) invalid("membership has no normalized labels");
 admit_normalized_annotations(backing.index.storage(), cancellation);
 std::call_once(backing.full_admission, [&] {
  for (std::size_t i = 0; i < backing.recovery.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   const auto& fact = backing.recovery[i];
   if (fact.objects.empty() && fact.omissions.empty()) continue;
   ComponentRecoveryAdmission join(&fact);
   const auto& image = backing.index.image(i);
   for (const auto& box : backing.index.storage().boxes.subspan(static_cast<std::size_t>(image.first_box), image.box_count)) join.box(box);
   join.finish();
  }
 });
}
std::uint64_t coconut_component_storage_bytes(const CoconutComponent& component) {
 const auto completion = component.completion();
 if (!completion) invalid("component has no completed storage");
 return mmltk::common::math::checked_add(
  completion->size, mmltk::common::math::checked_add(completion->proof_bytes, component.backing_->seal->bytes, "COCONut index storage overflow"), "COCONut index storage overflow");
}
std::optional<CoconutComponent> load_coconut_component(const std::filesystem::path& index_path, CoconutEdition edition, CoconutImageNamespace source, std::string_view input_identity,
 Cancellation cancellation, bool metadata_only, const CoconutPhysicalMembership* physical, const CoconutMaskRecovery* recovery, bool allow_changed_physical, const CoconutCacheAdmission& publication) {
 throw_if_benchmark_cancelled(cancellation);
 if (bool(publication.ready) != bool(publication.withdrawn)) throw std::invalid_argument("incremental component admission requires paired publication and withdrawal");
 std::shared_ptr<CoconutComponentBacking> component;
 std::size_t published = 0;
 bool publication_failed = false, mapping_io = false;
 const auto withdraw = [&] {
  if (published && publication.withdrawn) publication.withdrawn(CoconutComponentBacking::loaded(component), published);
  published = 0;
 };
 try {
  if (!std::filesystem::is_regular_file(index_path.string() + ".complete.json")) return std::nullopt;
  std::uint64_t completion_bytes = 0;
  const auto manifest = read_json_file(index_path.string() + ".complete.json", &completion_bytes);
  const auto& extension = manifest.at("coconut");
  CoconutCompletionFacts facts;
  mmltk::frameworks::reflection::visit_materialized_members<CoconutCompletionFacts>([&]<class Declaration>(const auto& field) {
   if constexpr (mmltk::frameworks::reflection::OptionalValue<typename Declaration::member_type>::value) {
    if (const auto found = extension.find(field.member_name); found != extension.end())
     facts.*Declaration::pointer = found->template get<mmltk::frameworks::reflection::OptionalValueT<typename Declaration::member_type>>();
   } else
    extension.at(field.member_name).get_to(facts.*Declaration::pointer);
  });
  if (facts.edition != edition || facts.source != source || (!physical && facts.input_identity != input_identity) || facts.normalization != kCoconutNormalizationRevision) return std::nullopt;
  component = std::make_shared<CoconutComponentBacking>();
  component->edition = edition;
  component->source = source;
  component->input_identity = facts.input_identity;
  component->source_rows = facts.source_rows.value_or(0);
  component->source_segments = facts.source_segments.value_or(0);
  component->source_extent_complete = facts.source_extent_complete.value_or(false);
  component->incomplete_ordinal_images = facts.incomplete_ordinal_images.value_or(std::vector<std::uint64_t>{});
  if (!std::ranges::is_sorted(component->incomplete_ordinal_images) || std::ranges::adjacent_find(component->incomplete_ordinal_images) != component->incomplete_ordinal_images.end()) return std::nullopt;
  component->rejected_images = facts.rejected_images.value_or(std::vector<std::uint64_t>{});
  if (!std::ranges::is_sorted(component->rejected_images) || std::ranges::adjacent_find(component->rejected_images) != component->rejected_images.end()) return std::nullopt;
  if (physical) component->annotation_input_identity = input_identity;
  component->recovery_policy = facts.recovery_policy.value_or(0);
  component->original_annotation_identity = facts.original_annotation_identity.value_or("");
  if (physical && component->original_annotation_identity != (recovery ? recovery->original_identity(source) : std::string_view{})) return std::nullopt;
  if (!std::filesystem::is_regular_file(index_path.string() + ".inventory")) return std::nullopt;
  mapping_io = true;
  InventoryInput input(index_path.string() + ".inventory", cancellation);
  mapping_io = false;
  InventoryHeader header;
  input.value(header);
  validate_inventory_header(header, component_header(*component, facts.inventory_count));
  if (header.count > input.maximum_records() || facts.inventory_count != header.count) invalid("invalid component inventory count");
  auto inventory = std::make_shared<std::vector<CoconutInventoryImage>>();
  inventory->reserve(mmltk::common::math::checked_cast<std::size_t>(header.count, "component inventory count overflow"));
  component->inventory = inventory;
  ComponentInventoryAdmission admission(source);
  component->image_dependencies.reserve(static_cast<std::size_t>(header.count));
  // The same row admission owns both ordinary full loads and incremental
  // production. Inventory/recovery checksum acceptance remains terminal.
  const auto row_ready = [&](const NormalizedAnnotationIndex& normalized_index, std::size_t i) {
   if (!i) {
    component->index = NormalizedAnnotationReadView(normalized_index);
    if (header.count != component->index.image_count()) invalid("invalid component inventory count");
    admit_component_metadata(*component, component->index, component->index.image_count(), header.count, facts.recovery_images.value_or(0));
   }
   throw_if_benchmark_cancelled(cancellation);
   CoconutInventoryImage image;
   input.value(image);
   const bool complete = !std::ranges::binary_search(component->incomplete_ordinal_images, image.physical.image_id);
   const auto& normalized = normalized_index.images[i];
   admission.image(image, normalized, false, complete);
   if (!metadata_only) for (const auto& box : normalized_index.boxes.subspan(normalized.first_box, normalized.box_count))
    if (((box.flags & kAnnotationOrdinalIncomplete) == 0U) != complete) invalid("inconsistent cached annotation provenance");
   component->dependencies.add(image.physical);
   component->image_dependencies.push_back(coconut_image_input_identity(component->annotation_input_identity, image.physical, component->original_annotation_identity));
   inventory->push_back(std::move(image));
   if (!metadata_only && publication.ready) {
    // Count before entry: a throwing receiver may already have installed work.
    published = i + 1;
    try { publication.ready(CoconutComponentBacking::loaded(component), i); }
    catch (...) { publication_failed = true; throw; }
   }
  };
  auto index = load_normalized_annotation_index(index_path, index_source(source), component_split(edition, source), facts.inventory_identity, cancellation, {}, &manifest,
   metadata_only, completion_bytes, row_ready);
  if (!index) { withdraw(); return std::nullopt; }
  component->index = NormalizedAnnotationReadView(std::move(*index));
  if (header.count != component->index.image_count()) invalid("invalid component inventory count");
  if (physical && facts.input_identity != coconut_component_input_identity(input_identity, source, recovery, physical, edition, component->dependencies, !allow_changed_physical)) invalid("component input dependency mismatch");
  for (const auto id : component->incomplete_ordinal_images)
   if (!std::ranges::binary_search(*inventory, id, {}, [](const auto& image) { return image.physical.image_id; })) invalid("unknown incomplete-provenance image");

  if (component->recovery_policy) {
   std::uint32_t policy = 0;
   input.value(policy);
   if (policy != component->recovery_policy || policy != kCoconutRecoveryPolicy) invalid("invalid recovery policy");
   input.value(component->original_annotation_identity);
   std::uint64_t count = 0;
   input.value(count);
   if (count != header.count || facts.recovery_images != count || facts.original_annotation_identity != component->original_annotation_identity) invalid("invalid recovery completion");
   component->recovery.resize(static_cast<std::size_t>(count));
   for (std::size_t i = 0; i < count; ++i) {
    throw_if_benchmark_cancelled(cancellation);
    auto& fact = component->recovery[i];
    const auto& image = component->index.image(i);
    input.value(fact);
    admit_recovery_image(fact, image);
    if (!metadata_only) {
     ComponentRecoveryAdmission join(&fact);
     if (!fact.objects.empty() || !fact.omissions.empty())
      for (const auto& box : component->index.storage().boxes.subspan(static_cast<std::size_t>(image.first_box), image.box_count)) join.box(box);
     join.finish();
    }
   }
  } else if (!facts.original_annotation_identity.value_or("").empty() || facts.recovery_images.value_or(0))
   invalid("unexpected recovery completion");
  admit_component_metadata(*component, component->index, component->index.image_count(), component->inventory->size(), component->recovery.size());
  const auto identity = input.finish();
  if (facts.inventory_identity != identity) invalid("component inventory completion mismatch");
  component->seal = input.seal();
  component->seal->identity = identity;
  component->completion = component->index.completion;
  if (!metadata_only) std::call_once(component->full_admission, [] {});
  throw_if_benchmark_cancelled(cancellation);
  return CoconutComponentBacking::loaded(std::move(component));
 } catch (const std::exception& error) {
  withdraw();
  if (publication_failed || (mapping_io && !dynamic_cast<const CoconutInputError*>(&error)) || is_benchmark_capacity_failure(error) || dynamic_cast<const std::system_error*>(&error)) throw;
  throw_if_benchmark_cancelled(cancellation);
  return std::nullopt;
 } catch (...) {
  withdraw();
  throw;
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
