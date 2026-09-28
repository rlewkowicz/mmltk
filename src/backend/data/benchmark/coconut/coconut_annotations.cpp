#include "src/backend/data/benchmark/detail/benchmark_json.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_json.h"
#include <tuple>
#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotations.h"
#include <exception>
#include <bit>
#include <sys/mman.h>
#include <sys/stat.h>
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/frameworks/serialization/json_scalar.h"
#include "src/pch_std.h"
#include <iterator>
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include <stb_image.h>
namespace mmltk::backend::data::benchmark_internal {
class CoconutInventorySeal final {
public:
 void capture(const mmltk::common::io::FileHandle& file) {
  struct stat status{};
  if (::fstat(file.get(), &status) != 0) throw mmltk::common::io::errno_error("stat COCONut inventory");
  if (status.st_size < 32) throw std::runtime_error("COCONut: truncated inventory");
  const auto size = mmltk::common::math::checked_cast<std::size_t>(status.st_size, "COCONut inventory size overflow");
  if (bytes && bytes != size) throw std::logic_error("COCONut inventory custody changed extent");
  if (!bytes) bytes = size;
  void* address = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, file.get(), 0);
  if (address == MAP_FAILED) throw mmltk::common::io::errno_error("map COCONut inventory");
  mapping.adopt(address, bytes);
  device = status.st_dev; inode = status.st_ino;
 }
 [[nodiscard]] std::span<const std::uint8_t> data() const { return {static_cast<const std::uint8_t*>(mapping.address()), bytes}; }
 // Custody changes are serialized; semantic fields and mapped bytes never change.
 mutable std::mutex mutex;
 BenchmarkStagedArtifact staged;
 mmltk::common::io::MappedByteRegion mapping;
 std::string identity;
 std::size_t bytes = 0;
 std::filesystem::path directory, published_path;
 dev_t device = 0;
 ino_t inode = 0;
};
class CoconutComponentBacking final : public CoconutComponentMetadata {
public:
 NormalizedAnnotationReadView index;
 std::shared_ptr<const std::vector<CoconutInventoryImage>> inventory;
 std::vector<CoconutRecoveryImage> recovery;
 std::shared_ptr<CoconutInventorySeal> seal;
 mutable std::once_flag full_admission;
 mutable std::mutex completion_mutex;
 mutable std::shared_ptr<const NormalizedAnnotationCompletion> completion;
 bool metadata_only = false;
 static CoconutComponent finish(std::shared_ptr<CoconutComponentBacking>, NormalizedAnnotationIndex, bool,
  mmltk::common::concurrency::CancellationObservation, const std::filesystem::path&, StorageReservationPool*, const CoconutComponent* reuse = nullptr);
 static CoconutComponent loaded(std::shared_ptr<CoconutComponentBacking> backing) {
  return CoconutComponent(backing, backing->index, backing->seal, false);
 }
};
namespace {
using Json = nlohmann::json;
using Cancellation = mmltk::common::concurrency::CancellationObservation;
[[noreturn]] void invalid(std::string_view detail) { throw std::runtime_error("COCONut: " + std::string(detail)); }
struct PhysicalKey {
 CoconutImageNamespace source;
 std::uint64_t id;
 bool operator==(const PhysicalKey&) const = default;
};
struct PhysicalKeyHash {
 std::size_t operator()(PhysicalKey key) const noexcept { return std::hash<std::uint64_t>{}(key.id) ^ (static_cast<std::size_t>(key.source) * 0x9e3779b97f4a7c15ULL); }
};
PhysicalKey physical_key(CoconutImageNamespace source, std::uint64_t id) { return {source, id}; }
const CategoryLookup& coconut_categories() {
 static const auto lookup = make_numeric_lookup(coco_category_mappings());
 return lookup;
}
} // namespace
namespace {
using Archive = BenchmarkArchive;
namespace reflection = mmltk::frameworks::reflection;
struct JsonAtom {
 bool present = false;
 std::optional<CoconutJsonScalar> value;
 void reset() { present = false; value.reset(); }
 explicit operator bool() const noexcept { return present; }
 template<class T> const T* get() const noexcept { return value ? std::get_if<T>(&*value) : nullptr; }
 bool is_null() const noexcept { return get<std::nullptr_t>() != nullptr; }
 bool is_bool() const noexcept { return get<bool>() != nullptr; }
 bool boolean() const { if (const auto* result = get<bool>()) return *result; invalid("flag must be boolean or integer"); }
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
 std::string_view text() const { if (const auto* result = get<std::string>()) return *result; invalid("JSON field must be text"); }
};
template<class T> struct JsonArray {
 bool present = false, valid = false;
 std::vector<T> values;
 void reset() { present = valid = false; values.clear(); }
};
template<class Shape> class JsonObject;
template<class Value> struct JsonStorage { using type = JsonObject<Value>; };
template<> struct JsonStorage<CoconutJsonScalar> { using type = JsonAtom; };
template<class Value> struct JsonStorage<std::vector<Value>> { using type = JsonArray<typename JsonStorage<Value>::type>; };
template<class Fields> struct JsonMemberTuple;
template<class Bases, class... Declaration>
struct JsonMemberTuple<reflection::MaterializedFieldPolicyProduct<Bases, Declaration...>> {
 using type = std::tuple<typename JsonStorage<typename Declaration::member_type>::type...>;
};
// The two wire declarations project reusable parser storage without reflecting
// presence/type/object state or retaining unknown values as a DOM.
template<class Shape> class JsonObject {
 static_assert(std::same_as<Shape, CoconutJsonRow> || std::same_as<Shape, CoconutJsonSegment>);
 using Fields = std::remove_cvref_t<decltype(reflection::field_declarations<Shape>())>;
 typename JsonMemberTuple<Fields>::type fields_;
public:
 bool object = false;
 void reset() {
  object = false;
  Fields::Visit([&]<class Declaration, std::size_t Index>() { std::get<Index>(fields_).reset(); });
 }
 template<auto Member> const auto& get() const {
  return std::get<reflection::member_index<Member>(reflection::field_declarations<Shape>())>(fields_);
 }
 template<class Visitor> void select(std::string_view key, Visitor&& visitor) {
  bool selected = false;
  Fields::Visit([&]<class Declaration, std::size_t Index>() {
   if (!selected && key == reflection::field_declarations<Shape>()[Index].member_name) {
    visitor(std::get<Index>(fields_));
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
};
struct JsonConsumption {
 std::size_t events = 0, maximum;
 bool limited;
 void event(std::size_t depth) {
  if (!limited) return;
  if (depth > 16) invalid("annotation JSON nesting exceeds admission");
  if (++events > maximum) invalid("panoptic row exceeds segment admission");
 }
};
void consume_json_value(simdjson::ondemand::value value, JsonConsumption& admission, std::size_t depth, JsonAtom* atom = nullptr,
 JsonRow* row = nullptr, JsonSegment* segment = nullptr, JsonArray<JsonSegment>* segments = nullptr, JsonArray<JsonAtom>* coordinates = nullptr) {
 admission.event(depth);
 if (atom) { atom->present = true; atom->value.reset(); }
 if (segments) { segments->present = true; segments->valid = false; segments->values.clear(); }
 if (coordinates) { coordinates->present = true; coordinates->valid = false; coordinates->values.clear(); }
 const auto type = value.type().value();
 if (type == simdjson::ondemand::json_type::object) {
  if (row) row->object = true;
  if (segment) segment->object = true;
  for (auto field : value.get_object().value()) {
   admission.event(depth + 1);
   const auto key = field.unescaped_key().value();
   JsonAtom* destination = nullptr;
   JsonArray<JsonSegment>* list = nullptr;
   JsonArray<JsonAtom>* box = nullptr;
   const auto select = [&]<class T>(T& target) {
    if constexpr (std::same_as<T, JsonAtom>) destination = &target;
    else if constexpr (std::same_as<T, JsonArray<JsonSegment>>) list = &target;
    else if constexpr (std::same_as<T, JsonArray<JsonAtom>>) box = &target;
   };
   if (row) row->select(key, select);
   else if (segment) segment->select(key, select);
   consume_json_value(field.value(), admission, depth + 1, destination, nullptr, nullptr, list, box);
  }
  admission.event(depth);
 } else if (type == simdjson::ondemand::json_type::array) {
  if (segments) segments->valid = true;
  if (coordinates) coordinates->valid = true;
  for (auto child : value.get_array().value()) {
   if (segments) { segments->values.emplace_back(); consume_json_value(child.value(), admission, depth + 1, nullptr, nullptr, &segments->values.back()); }
   else if (coordinates) { coordinates->values.emplace_back(); consume_json_value(child.value(), admission, depth + 1, &coordinates->values.back()); }
   else consume_json_value(child.value(), admission, depth + 1);
  }
  admission.event(depth);
 } else if (type == simdjson::ondemand::json_type::string) {
  const auto text = value.get_string().value();
  if (admission.limited && text.size() > 4096) invalid("panoptic text exceeds admission");
  if (atom) atom->value = std::string(text);
 } else if (type == simdjson::ondemand::json_type::number) {
  const auto token = value.raw_json_token();
  auto parsed = value.get_number();
  if (parsed.error() == simdjson::BIGINT_ERROR) {
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
   if (number.is_uint64()) atom->value = number.get_uint64();
   else if (number.is_int64()) atom->value = number.get_int64();
   else atom->value = number.get_double();
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
 if (!field) { if (required) invalid("missing required flag"); return false; }
 if (field.is_bool()) return field.boolean();
 const auto value = field.unsigned_integer();
 if (value > 1) invalid("invalid flag");
 return value != 0;
}
void segments_from_json(const JsonArray<JsonSegment>& input, CoconutRecord& record, const CoconutImportLimits& limits) {
 if (!input.present || !input.valid || input.values.size() > limits.max_segments) invalid("segment list exceeds admission");
 record.segments.reserve(input.values.size());
 for (const auto& value : input.values) {
  if (!value.object) invalid("segment must be an object");
  CoconutSegment segment;
  const auto id = value.get<&CoconutJsonSegment::id>().unsigned_integer();
  if (id == 0 || id > 0xffffffU) invalid("segment ID exceeds RGB24");
  segment.id = static_cast<std::uint32_t>(id);
  segment.category_id = value.get<&CoconutJsonSegment::category_id>().unsigned_integer();
  segment.isthing = flag(value.get<&CoconutJsonSegment::isthing>(), true);
  segment.crowd = flag(value.get<&CoconutJsonSegment::iscrowd>()); segment.ignore = flag(value.get<&CoconutJsonSegment::ignore>());
  if (value.get<&CoconutJsonSegment::area>() && !value.get<&CoconutJsonSegment::area>().is_null()) {
   segment.area = value.get<&CoconutJsonSegment::area>().number();
   if (!std::isfinite(*segment.area) || *segment.area < 0) invalid("invalid supplied segment area");
  }
  if (value.get<&CoconutJsonSegment::bbox>().present) {
   if (!value.get<&CoconutJsonSegment::bbox>().valid || value.get<&CoconutJsonSegment::bbox>().values.size() != 4) invalid("invalid supplied COCO bbox");
   segment.bbox.emplace();
   for (std::size_t i = 0; i < 4; ++i) (*segment.bbox)[i] = value.get<&CoconutJsonSegment::bbox>().values[i].number();
  }
  record.segments.push_back(std::move(segment));
 }
}
template <class T>
concept InventoryRecord = std::same_as<T, CoconutPhysicalImage> || std::same_as<T, CoconutInventoryImage> || std::same_as<T, InventoryHeader> || std::same_as<T, CoconutRecoveryImage> ||
                          std::same_as<T, CoconutRecoveredObject>;
// A single reflected little-endian encoding is used by hashing, writing and reading.
// Strings carry checked uint32 lengths. Only a fixed buffer and the current row
// are transient; records remain in their ordinary typed inventory owner.
class InventoryOutput final {
public:
 InventoryOutput(BenchmarkStagedArtifact* file, Cancellation cancellation) : file_(file), cancellation_(cancellation) {}
 template <class T>
 void value(const T& item) {
  if constexpr (InventoryRecord<T>) {
   mmltk::frameworks::reflection::visit_materialized_members<T>([&]<class Declaration>(const auto&) { value(item.*Declaration::pointer); });
  } else if constexpr (std::same_as<T, std::vector<CoconutRecoveredObject>>) {
   value(static_cast<std::uint32_t>(item.size()));
   for (const auto& object : item) value(object);
  } else if constexpr (std::is_enum_v<T>) {
   value(static_cast<std::underlying_type_t<T>>(item));
  } else if constexpr (std::same_as<T, std::string>) {
   if (item.size() > 4096) invalid("inventory text exceeds admission");
   value(static_cast<std::uint32_t>(item.size()));
   append({reinterpret_cast<const std::uint8_t*>(item.data()), item.size()});
  } else {
   static_assert(std::is_unsigned_v<T> || std::same_as<T, bool>);
   std::array<std::uint8_t, sizeof(T)> bytes{};
   auto number = static_cast<std::uint64_t>(item);
   for (auto& byte : bytes) {
    byte = static_cast<std::uint8_t>(number & 255U);
    number >>= 8U;
   }
   append(bytes);
  }
 }
 [[nodiscard]] std::uint64_t byte_size() const { return mmltk::common::math::checked_add<std::uint64_t>(offset_, 32U, "inventory extent overflow"); }
 std::string finish() {
  flush();
  const auto digest = hash_.Finish();
  if (file_) { file_->resize(byte_size(), "COCONut inventory digest extent"); file_->file().pwrite_all(digest.data(), digest.size(), offset_); file_->reconcile(); }
  throw_if_benchmark_cancelled(cancellation_);
  return mmltk::common::io::sha256_hex(digest);
 }

private:
 void append(std::span<const std::uint8_t> bytes) {
  while (!bytes.empty()) {
   const auto count = std::min(bytes.size(), buffer_.size() - used_);
   std::memcpy(buffer_.data() + used_, bytes.data(), count);
   used_ += count;
   bytes = bytes.subspan(count);
   if (used_ == buffer_.size()) flush();
  }
 }
 void flush() {
  throw_if_benchmark_cancelled(cancellation_);
  if (!used_) return;
  if (used_ > std::numeric_limits<std::size_t>::max() - offset_) invalid("inventory size overflow");
  hash_.Update(std::span(buffer_.data(), used_));
  if (file_) { file_->resize(offset_ + used_ + 32, "COCONut inventory extent"); file_->file().pwrite_all(buffer_.data(), used_, offset_); file_->reconcile(); }
  offset_ += used_;
  used_ = 0;
 }
 BenchmarkStagedArtifact* file_;
 Cancellation cancellation_;
 mmltk::common::io::Sha256Hasher hash_;
 std::array<std::uint8_t, 65536> buffer_{};
 std::size_t used_ = 0, offset_ = 0;
};
class InventoryInput final {
public:
 InventoryInput(const std::filesystem::path& path, Cancellation cancellation) : seal_(std::make_shared<CoconutInventorySeal>()), cancellation_(cancellation) {
  const auto file = mmltk::common::io::FileHandle::open_readonly(path.string());
  seal_->capture(file);
  seal_->directory = path.parent_path(); seal_->published_path = path;
  content_size_ = seal_->bytes - 32;
 }
 std::shared_ptr<CoconutInventorySeal> seal() const { return seal_; }
 template <class T>
 void value(T& item) {
  if constexpr (InventoryRecord<T>) {
   mmltk::frameworks::reflection::visit_materialized_members<T>([&]<class Declaration>(const auto&) { value(item.*Declaration::pointer); });
  } else if constexpr (std::same_as<T, std::vector<CoconutRecoveredObject>>) {
   std::uint32_t count = 0;
   value(count);
   if (count > 65535U || count > maximum_records()) invalid("invalid recovery object count");
   item.resize(count);
   for (auto& object : item) value(object);
  } else if constexpr (std::is_enum_v<T>) {
   std::underlying_type_t<T> number{};
   value(number);
   item = static_cast<T>(number);
  } else if constexpr (std::same_as<T, std::string>) {
   std::uint32_t count = 0;
   value(count);
   if (count > 4096) invalid("inventory text exceeds admission");
   item.resize(count);
   read({reinterpret_cast<std::uint8_t*>(item.data()), count});
  } else {
   static_assert(std::is_unsigned_v<T> || std::same_as<T, bool>);
   std::array<std::uint8_t, sizeof(T)> bytes{};
   read(bytes);
   std::uint64_t number = 0;
   for (std::size_t i = 0; i < bytes.size(); ++i) number |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
   if constexpr (std::same_as<T, bool>) {
    if (number > 1) invalid("invalid inventory flag");
   }
   item = static_cast<T>(number);
  }
 }
 std::uint64_t maximum_records() const { return content_size_ / 16U; }
 std::uint64_t byte_size() const { return content_size_ + 32U; }
 std::string finish() {
  throw_if_benchmark_cancelled(cancellation_);
  if (loaded_ != content_size_ || cursor_ != available_) invalid("inventory has trailing records");
  mmltk::common::io::Sha256Digest expected{};
  std::memcpy(expected.data(), seal_->data().data() + content_size_, expected.size());
  const auto actual = hash_.Finish();
  if (actual != expected) invalid("inventory checksum mismatch");
  return mmltk::common::io::sha256_hex(actual);
 }

private:
 void read(std::span<std::uint8_t> output) {
  while (!output.empty()) {
   if (cursor_ == available_) {
    throw_if_benchmark_cancelled(cancellation_);
    available_ = std::min(std::size_t{65536}, content_size_ - loaded_);
    cursor_ = 0;
    if (!available_) invalid("truncated inventory record");
    hash_.Update(seal_->data().subspan(loaded_, available_));
    loaded_ += available_;
   }
   const auto count = std::min(output.size(), available_ - cursor_);
   std::memcpy(output.data(), seal_->data().data() + loaded_ - available_ + cursor_, count);
   cursor_ += count;
   output = output.subspan(count);
  }
 }
 std::shared_ptr<CoconutInventorySeal> seal_;
 Cancellation cancellation_;
 mmltk::common::io::Sha256Hasher hash_;
 std::size_t content_size_ = 0, loaded_ = 0, cursor_ = 0, available_ = 0;
};
InventoryHeader component_header(const CoconutComponentMetadata& component, std::size_t count) {
 InventoryHeader header;
 header.component = true; header.input_identity = component.input_identity;
 header.edition = component.edition; header.source = component.source; header.count = count;
 return header;
}
void validate_inventory_header(const InventoryHeader& header, const InventoryHeader& expected) {
 if (header.magic != expected.magic || header.version != expected.version || header.cache_schema != expected.cache_schema || header.normalization != expected.normalization ||
     header.input_identity != expected.input_identity || header.edition != expected.edition || header.source != expected.source || header.shard != expected.shard ||
     header.component != expected.component)
  invalid("inventory identity mismatch");
}
template <class Records>
std::string encode_inventory(InventoryOutput& output, const InventoryHeader& header, const Records& records, Cancellation cancellation,
 std::uint32_t policy = 0, std::string_view original = {}, const CoconutComponent* recovery = nullptr) {
 output.value(header);
 for (const auto& record : records) { throw_if_benchmark_cancelled(cancellation); output.value(record); }
 if (policy) {
  output.value(policy); output.value(std::string(original));
  output.value(static_cast<std::uint64_t>(recovery->recovery().size()));
  for (const auto& image : recovery->recovery()) { throw_if_benchmark_cancelled(cancellation); output.value(image); }
 }
 return output.finish();
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
 seal->identity = encode_inventory(output, component_header(metadata, component.index().image_count()), component.inventory(), cancellation,
  component.recovery_policy(), component.original_annotation_identity(), &component);
 seal->staged.file().sync_data(); seal->capture(seal->staged.file()); seal->staged.close();
 return seal;
}
template <InventoryRecord T>
std::string store_inventory(const std::filesystem::path& path, const InventoryHeader& header, std::span<const T> records, Cancellation cancellation, StorageReservationPool* storage = nullptr) {
 throw_if_benchmark_cancelled(cancellation);
 (void)mmltk::common::io::ensure_parent_directory(path);
 StorageReservationPool destination(path, {}, storage);
 auto staging = BenchmarkStagedArtifact::create(destination, path, 0, "COCONut inventory staging");
 InventoryOutput output(&staging, cancellation);
 const auto identity = encode_inventory(output, header, records, cancellation);
 staging.file().sync_data(); staging.publish(path, cancellation);
 return identity;
}
BenchmarkDatasetSource index_source(CoconutImageNamespace source) {
 return source == CoconutImageNamespace::Objects365V1 || source == CoconutImageNamespace::Objects365V2 ? BenchmarkDatasetSource::kObjects365V2 : BenchmarkDatasetSource::kCoco2017;
}
std::string component_split(CoconutEdition edition, CoconutImageNamespace source) {
 return "coconut-" + std::to_string(static_cast<unsigned>(edition)) + "-" + std::to_string(static_cast<unsigned>(source));
}
void admit_component_metadata(const CoconutComponentMetadata& component, const NormalizedAnnotationMetadata& index, std::size_t images, std::size_t inventory, std::size_t recovery) {
 (void)coconut_namespace_name(component.source); (void)coconut_release_component(component.edition);
 if (component.input_identity.empty() || images != inventory || index.source != index_source(component.source) || index.split != component_split(component.edition, component.source))
  invalid("component index/inventory admission mismatch");
 if (component.recovery_policy) {
  if (component.recovery_policy != kCoconutRecoveryPolicy || (component.source != CoconutImageNamespace::CocoTrain && component.source != CoconutImageNamespace::CocoValidation) || recovery != inventory)
   invalid("invalid recovery source or image count");
  (void)mmltk::common::io::parse_sha256_hex(component.original_annotation_identity);
 } else if (!component.original_annotation_identity.empty() || recovery) invalid("unexpected recovery facts");
}
// Runs in the owning assembly/decode loop. The ordinal set is temporary join
// evidence, never a second inventory or a retained selection.
class ComponentInventoryAdmission final {
public:
 explicit ComponentInventoryAdmission(CoconutImageNamespace source) : source_(source) {}
 void image(const CoconutInventoryImage& row, const NormalizedImage& normalized, bool physical_admitted = false) {
  if (!physical_admitted) validate_coconut_physical_image(row.physical);
  if (row.physical.source != source_ || row.physical.image_id != normalized.source_image_id || row.physical.shard != normalized.source_shard ||
      !ordinals_.insert(row.source_ordinal).second || (previous_ && row.physical.image_id <= *previous_)) invalid("invalid component image inventory");
  previous_ = row.physical.image_id;
 }
private:
 CoconutImageNamespace source_;
 std::optional<std::uint64_t> previous_;
 std::unordered_set<std::uint64_t> ordinals_;
};
void admit_recovery_image(const CoconutRecoveryImage& fact, const NormalizedImage& image) {
 if (fact.image_id != image.source_image_id || fact.unresolved > 65535U || fact.unresolved != fact.omissions.size() || fact.objects.size() > image.box_count ||
     fact.objects.size() > 65535U - fact.omissions.size()) invalid("invalid image recovery facts");
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
   if (object.original_annotation_id != 0 || object.source_category_id == 0 || !annotations.insert(object.annotation_id).second || by_annotation_.contains(object.annotation_id)) invalid("invalid omitted object identity");
  for (const auto& object : fact_->objects) {
   const auto found = by_annotation_.find(object.annotation_id);
   if (!originals.insert(object.original_annotation_id).second || !annotations.insert(object.annotation_id).second || found == by_annotation_.end() ||
       found->second.source_ordinal != object.source_ordinal || found->second.source_category_id != object.source_category_id) invalid("invalid recovered object identity");
  }
 }
private:
 const CoconutRecoveryImage* fact_;
 std::unordered_map<std::uint64_t, NormalizedBox> by_annotation_;
};
class Importer final {
public:
 explicit Importer(const CoconutImportRequest& request, bool group = false) : request_(request), group_(group), recovery_(request.recovery ? request.recovery->make_workspace() : nullptr) {
  if (request.input_identity.empty()) invalid("missing pinned input identity");
  if (!request.physical_membership) invalid("missing physical membership admission");
  if (!group_ && request_.progress) request_.progress(0);
 }
 void retire_scratch() noexcept {
  segment_by_id_ = decltype(segment_by_id_){};
  support_ = decltype(support_){};
  if (recovery_) recovery_->retire_scratch();
  support_capacity_bytes_ = 0;
 }
 std::uint64_t maximum_workspace(const CoconutRecord& record) const {
  const auto pixels = record.width && record.height ? std::uint64_t{record.width} * record.height : request_.limits.max_pixels;
  return mmltk::common::math::checked_multiply(workspace_bytes(record, pixels, request_.limits.max_segments, request_.limits.max_png_bytes), 2U, "COCONut retained normalizer workspace overflow");
 }
 void consume_group(std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) {
  if (input.membership && !request_.metadata_only) {
   (void)request_.physical_membership->resolve(request_.edition, request_.input_identity, record, input.allowance);
   if (request_.execution && input.allowance) request_.execution->with_unused_workspace(input.allowance, input.live_bytes, [&] {
    request_.execution->run(BenchmarkStage::Metadata, {}, [](std::size_t) {}, input.allowance);
   });
   return;
  }
  Importer* worker;
  {
   const std::lock_guard lock(groups_mutex_);
   if (groups_.size() <= group) groups_.resize(group + 1);
   if (!groups_[group]) groups_[group] = std::make_unique<Importer>(request_, true);
   worker = groups_[group].get();
  }
  worker->consume(record, input.png, input.allowance, input.live_bytes);
  const std::lock_guard lock(groups_mutex_);
  ++rows_;
  if (request_.progress && rows_ % kProgressQuantum == 0) request_.progress(rows_);
 }
 void retire_group(std::size_t group) noexcept {
  const std::lock_guard lock(groups_mutex_);
  if (group < groups_.size() && groups_[group]) groups_[group]->retire_scratch();
 }
 void collect_groups(std::span<const CoconutRecordGroup> groups) {
  group_prefixes_ = groups;
  const auto collect = [&](std::size_t) {
  for (const auto& worker : groups_) if (worker)
   for (const auto key : worker->offered_) if (!offered_.insert(key).second) invalid("duplicate offered physical member");
  };
  if (request_.execution) request_.execution->run(BenchmarkStage::Metadata, {65536, 0}, collect); else collect(0);
 }
 void consume(const CoconutRecord& record, std::span<const std::uint8_t> png, BenchmarkAllowance allowance = {}, std::uint64_t input_live_bytes = 0) {
  const auto physical = request_.physical_membership->resolve(request_.edition, request_.input_identity, record, allowance ? allowance : request_.parent_allowance);
  const bool normalize = !request_.metadata_only && std::ranges::find(request_.retained_sources, physical.source) == request_.retained_sources.end();
  dataset::MaskDimensions dimensions{};
  const auto inspect = [&](std::size_t) {
   if (!normalize) return;
   try { dimensions = admit_png(record, png); }
   catch (const std::bad_alloc&) { throw; }
   catch (const std::exception& error) { invalid(physical.member + ": " + error.what()); }
  };
  if (request_.execution) request_.execution->run(BenchmarkStage::Normalize, {}, inspect, allowance); else inspect(0);
  const auto upcoming = normalize ? workspace_bytes(record, std::uint64_t{dimensions.width} * dimensions.height, record.segments.size(), png.size()) : 0;
  // A differently shaped image may have left capacity in several run vectors.
  // Keep it only while the next complete envelope can also cover it. The
  // capacity counter changes alongside run mutation, not in a recount walk.
  if (retained_workspace() > upcoming) retire_scratch();
  const auto bytes = mmltk::common::math::checked_add(retained_workspace(), upcoming, "COCONut retained normalizer overflow");
  const auto work = [&] {
   if (request_.execution) request_.execution->run(request_.metadata_only ? BenchmarkStage::Metadata : BenchmarkStage::Normalize, {bytes, 0},
    [&](std::size_t) { consume_record(record, png, physical, dimensions); }, allowance);
   else consume_record(record, png, physical, dimensions);
  };
  if (request_.execution && allowance && input_live_bytes) request_.execution->with_unused_workspace(allowance,
   mmltk::common::math::checked_add(input_live_bytes, bytes, "COCONut live consumer workspace overflow"), work);
  else work();
 }
 void consume_record(const CoconutRecord& record, std::span<const std::uint8_t> png, const CoconutPhysicalImage& physical, dataset::MaskDimensions dimensions) {
  throw_if_benchmark_cancelled(request_.cancellation);
  const auto key = physical_key(physical.source, physical.image_id);
  if (!offered_.insert(key).second) invalid("duplicate offered physical member: " + physical.member);
  try {
   if (std::ranges::find(request_.retained_sources, physical.source) != request_.retained_sources.end()) {
    // Membership still participates in duplicate and expected-row admission.
   } else if (request_.metadata_only) {
    auto& component = component_for(physical.source);
    component.index.images.push_back({physical.image_id, 0, 0, record.width, record.height, physical.shard, 0});
    component.inventory.push_back({physical, record.image_id, record.source_ordinal});
    if (component.recovery_policy) component.recovery.push_back({physical.image_id, 0, {}});
   } else
    normalize(record, physical, png, dimensions);
  } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) { invalid(physical.member + ": " + error.what()); }
  ++rows_;
  if (!group_ && request_.progress && rows_ % kProgressQuantum == 0) request_.progress(rows_);
 }
 std::vector<CoconutComponent> finish() {
  if (request_.progress && rows_ % kProgressQuantum != 0) request_.progress(rows_);
  throw_if_benchmark_cancelled(request_.cancellation);
  if (rows_ == 0) invalid("selected release contains no offered image rows");
  if (request_.expected_rows != 0 && rows_ != request_.expected_rows) invalid("offered row count does not match the selected release");
  struct ImageChunk { CoconutComponentBuilder* component; std::size_t image; std::uint64_t segment_base; };
  std::uint64_t image_count = 0, recovery_join_boxes = 0;
  const auto count_images = [&](const Importer& owner) {
   for (const auto& [source, component] : owner.components_) {
    image_count = mmltk::common::math::checked_add(image_count, std::uint64_t{component.inventory.size()}, "COCONut merge image count overflow");
    if (component.recovery_policy)
     recovery_join_boxes = std::max(recovery_join_boxes, std::min<std::uint64_t>(component.index.boxes.size(), request_.limits.max_segments));
   }
  };
  count_images(*this);
  for (const auto& worker : groups_) if (worker) count_images(*worker);
  // Layout capacity and the temporary ordinal set live through assembly. Only
  // one image's recovery join is live; bound it by that component's boxes and
  // the admitted per-image segment limit without another image census.
  const auto join_bytes = mmltk::common::math::checked_multiply(recovery_join_boxes, std::uint64_t{256}, "COCONut recovery join workspace overflow");
  const auto workspace = mmltk::common::math::checked_add(mmltk::common::math::checked_multiply(image_count, std::uint64_t{6 * sizeof(ImageChunk)}, "COCONut merge workspace overflow"),
   mmltk::common::math::checked_add(join_bytes, std::uint64_t{65536}, "COCONut merge workspace overflow"), "COCONut merge workspace overflow");
  // Layout references retain their credit through final assembly and intervening
  // inventory I/O; only finite CPU work enters the shared lane.
  auto merge_allowance = request_.execution ? request_.execution->reserve({workspace, 0}) : BenchmarkAllowance{};
  std::map<CoconutImageNamespace, std::vector<ImageChunk>> images;
  std::map<CoconutImageNamespace, AnnotationRejectCounts> rejected;
  std::map<CoconutImageNamespace, std::pair<std::size_t, std::size_t>> extents;
  const auto gather = [&](Importer& owner, std::uint64_t segment_base) {
   for (auto& [source, component] : owner.components_) {
    auto& rows = images[source];
    auto& [boxes, runs] = extents[source];
    boxes = mmltk::common::math::checked_add(boxes, component.index.boxes.size(), "COCONut box count overflow");
    runs = mmltk::common::math::checked_add(runs, component.index.mask_rle_pairs.size(), "COCONut run count overflow");
    for (std::size_t image = 0; image < component.inventory.size(); ++image) rows.push_back({&component, image, segment_base});
    mmltk::frameworks::reflection::visit_materialized_members<AnnotationRejectCounts>([&]<class Declaration>(const auto&) {
     rejected[source].*Declaration::pointer = mmltk::common::math::checked_add(rejected[source].*Declaration::pointer, component.index.rejected.*Declaration::pointer, "COCONut rejection count overflow");
    });
   }
  };
  const auto gather_images = [&](std::size_t) {
   gather(*this, 0);
   for (std::size_t group = 0; group < groups_.size(); ++group) if (groups_[group]) gather(*groups_[group], group_prefixes_[group].first_segment);
  };
  if (request_.execution) request_.execution->run(BenchmarkStage::Metadata, {}, gather_images, merge_allowance); else gather_images(0);
  std::vector<CoconutComponent> result;
  for (auto& [source, rows] : images) {
   throw_if_benchmark_cancelled(request_.cancellation);
   const auto& header = component_for(source);
   auto sealed = std::make_shared<CoconutComponentBacking>();
   static_cast<CoconutComponentMetadata&>(*sealed) = header;
   auto inventory = std::make_shared<std::vector<CoconutInventoryImage>>();
   const CoconutComponent* reusable = nullptr;
   if (!sealed->recovery_policy && request_.records && request_.records->identity == request_.input_identity)
    for (const auto& candidate : request_.records->inventories)
     if (!candidate.index().selected() && !candidate.recovery_policy() && candidate.edition() == sealed->edition && candidate.source() == source && candidate.input_identity() == sealed->input_identity && candidate.index().image_count() == rows.size()) {
      reusable = &candidate; break;
     }
   NormalizedAnnotationIndex normalized;
   const auto assemble = [&](std::size_t) {
   const auto physical = [](const ImageChunk& row) -> const CoconutPhysicalImage& { return row.component->inventory[row.image].physical; };
   std::ranges::sort(rows, [&](const auto& left, const auto& right) { return physical(left).image_id < physical(right).image_id; });
   inventory->reserve(rows.size());
   if (sealed->recovery_policy) sealed->recovery.reserve(rows.size());
   ComponentInventoryAdmission inventory_admission(source);
   NormalizedAnnotationMetadata metadata = header.index;
   metadata.rejected = rejected[source];
   const auto [boxes, runs] = extents[source];
   std::optional<NormalizedAnnotationAssembler> assembly;
   if (!request_.metadata_only) assembly.emplace(metadata, rows.size(), boxes, runs, request_.cancellation);
   NormalizedAnnotationBuilder membership;
   static_cast<NormalizedAnnotationMetadata&>(membership) = metadata;
   if (request_.metadata_only) membership.images.reserve(rows.size());
   for (const auto& row : rows) {
    throw_if_benchmark_cancelled(request_.cancellation);
    const auto& index = row.component->index;
    const auto& image = index.images[row.image];
    if (reusable && row.component->inventory[row.image] != reusable->inventory_image(inventory->size())) reusable = nullptr;
    inventory_admission.image(row.component->inventory[row.image], image, reusable != nullptr);
    if (sealed->recovery_policy) {
     auto recovery = std::move(row.component->recovery[row.image]);
     for (auto* objects : {&recovery.objects, &recovery.omissions}) for (auto& object : *objects)
      object.source_ordinal = mmltk::common::math::checked_add(object.source_ordinal, row.segment_base, "COCONut recovery ordinal overflow");
     admit_recovery_image(recovery, image);
     sealed->recovery.push_back(std::move(recovery));
    }
    ComponentRecoveryAdmission recovery_admission(sealed->recovery_policy ? &sealed->recovery.back() : nullptr);
    if (request_.metadata_only) membership.images.push_back(image);
    else {
     assembly->begin_image(image);
     for (auto box : std::span(index.boxes).subspan(static_cast<std::size_t>(image.first_box), image.box_count)) {
      box.source_ordinal = mmltk::common::math::checked_add(box.source_ordinal, row.segment_base, "COCONut segment ordinal overflow");
      recovery_admission.box(box);
      assembly->append_box(box, std::span(index.mask_rle_pairs).subspan(static_cast<std::size_t>(box.mask_rle_offset), box.mask_rle_pairs));
     }
     recovery_admission.finish();
    }
    inventory->push_back(std::move(row.component->inventory[row.image]));
   }
   normalized = request_.metadata_only ? seal_normalized_annotation_metadata(std::move(membership)) : assembly->finish();
   };
   if (request_.execution) request_.execution->run(BenchmarkStage::Metadata, {}, assemble, merge_allowance); else assemble(0);
   sealed->inventory = std::move(inventory);
   auto output = request_.execution && !reusable ? request_.execution->reserve(BenchmarkResources::handles(1, false), request_.parent_allowance) : BenchmarkAllowance{};
   auto component = CoconutComponentBacking::finish(std::move(sealed), std::move(normalized), request_.metadata_only, request_.cancellation,
    request_.inventory_directory, request_.execution ? &request_.execution->storage() : nullptr, reusable);
   if (request_.records && request_.metadata_only) request_.records->inventories.push_back(component);
   result.push_back(std::move(component));
  }
  throw_if_benchmark_cancelled(request_.cancellation);
  return result;
 }

private:
 static constexpr std::uint64_t kProgressQuantum = 64;
 std::uint64_t retained_workspace() const noexcept {
  return support_capacity_bytes_ + support_.capacity() * sizeof(CoconutSegmentSupport) + segment_by_id_.capacity() * sizeof(SegmentEntry) +
   (recovery_ ? recovery_->retained_bytes() : 0);
 }
 std::uint64_t workspace_bytes(const CoconutRecord& record, std::uint64_t pixels, std::uint64_t segments, std::uint64_t encoded) const {
  using mmltk::common::math::checked_add;
  using mmltk::common::math::checked_multiply;
  // stb's encoded inflate input and scanlines/output; native run growth and
  // carving; support, open-address lookup, recovery grouping and ordinals.
  auto bytes = checked_add(checked_multiply(pixels, 96U, "COCONut pixel workspace overflow"), checked_multiply(encoded, 2U, "COCONut encoded workspace overflow"), "COCONut decode workspace overflow");
  bytes = checked_add(bytes, checked_multiply(segments, 1024U, "COCONut segment workspace overflow"), "COCONut normalize workspace overflow");
  return checked_add(bytes, recovery_ ? recovery_->workspace_bytes(record) : 0, "COCONut recovery workspace overflow");
 }
 CoconutComponentBuilder& component_for(CoconutImageNamespace source) {
  auto [entry, inserted] = components_.try_emplace(source);
  auto& component = entry->second;
  if (inserted) {
   component.edition = request_.edition;
   component.source = source;
   component.input_identity = coconut_component_input_identity(request_.input_identity, source, request_.recovery);
   component.index.source = index_source(source);
   component.index.split = component_split(request_.edition, source);
   if (request_.recovery) {
    component.original_annotation_identity = request_.recovery->original_identity(source);
    if (!component.original_annotation_identity.empty()) component.recovery_policy = kCoconutRecoveryPolicy;
   }
  }
  return component;
 }
 dataset::MaskDimensions admit_png(const CoconutRecord& record, std::span<const std::uint8_t> png) {
  const auto& limits = request_.limits;
  if (png.size() > limits.max_png_bytes || png.size() > INT_MAX || png.size() < 26U || std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) != 0 || png[24] != 8 || png[25] != 2)
   invalid("expected bounded 8-bit RGB panoptic PNG");
  int width = 0, height = 0, channels = 0;
  if (!stbi_info_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels) || width <= 0 || height <= 0 || channels != 3 ||
      static_cast<unsigned>(width) > limits.max_dimension || static_cast<unsigned>(height) > limits.max_dimension || static_cast<std::uint64_t>(width) * height > limits.max_pixels ||
      static_cast<std::uint64_t>(width) * height > UINT32_MAX)
   invalid("PNG dimensions exceed admission");
  if ((record.width && record.width != static_cast<unsigned>(width)) || (record.height && record.height != static_cast<unsigned>(height))) invalid("declared and PNG dimensions disagree");
  if (record.segments.size() > limits.max_segments) invalid("segment count exceeds admission");
  return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
 }
 void normalize(const CoconutRecord& record, const CoconutPhysicalImage& physical, std::span<const std::uint8_t> png, dataset::MaskDimensions dimensions) {
  int width = static_cast<int>(dimensions.width), height = static_cast<int>(dimensions.height), channels = 3;
  throw_if_benchmark_cancelled(request_.cancellation);
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels, 3), stbi_image_free);
  if (!pixels) invalid("cannot decode panoptic PNG");
  segment_by_id_.assign(std::bit_ceil(std::max<std::size_t>(2, record.segments.size() * 2)), {});
  if (support_.size() < record.segments.size()) support_.resize(record.segments.size());
  for (std::size_t i = 0; i < record.segments.size(); ++i) {
   const auto& segment = record.segments[i];
   if (segment.id == 0 || segment.id > 0xffffffU) invalid("duplicate/invalid segment ID");
   auto& entry = segment_entry(segment.id);
   if (entry.id) invalid("duplicate/invalid segment ID");
   entry = {segment.id, i};
   auto& support = support_[i];
   support.area = 0;
   support.bounds = {};
   support.runs.clear();
   support.recovered.reset();
   support.carved = false;
  }
  const std::uint32_t count = static_cast<std::uint32_t>(static_cast<std::uint64_t>(width) * height);
  for (std::uint32_t begin = 0; begin < count;) {
   if ((begin / static_cast<std::uint32_t>(width)) % 64U == 0) throw_if_benchmark_cancelled(request_.cancellation);
   const auto id_at = [&](std::uint32_t position) {
    const auto* pixel = pixels.get() + static_cast<std::size_t>(position) * 3U;
    return static_cast<std::uint32_t>(pixel[0]) | (static_cast<std::uint32_t>(pixel[1]) << 8U) | (static_cast<std::uint32_t>(pixel[2]) << 16U);
   };
   const auto id = id_at(begin);
   auto end = begin + 1U;
   // Row boundaries make bounds O(1) per run; coalesce adjacent support below.
   const auto row_end = std::min(count, (begin / static_cast<std::uint32_t>(width) + 1U) * static_cast<std::uint32_t>(width));
   while (end < row_end && id_at(end) == id) ++end;
   if (id != 0) {
    const auto& found = segment_entry(id);
    if (!found.id) invalid("PNG references undeclared segment " + std::to_string(id));
    auto& support = support_[found.index];
    support.area += end - begin;
    dataset::include_row_major_mask_run(&support.bounds, begin, end, static_cast<std::uint32_t>(width));
    if (record.segments[found.index].isthing) {
     if (!support.runs.empty() && support.runs.back().start + support.runs.back().length == begin)
      support.runs.back().length += end - begin;
     else {
      const auto previous_capacity = support.runs.capacity();
      support.runs.push_back({begin, end - begin});
      support_capacity_bytes_ += (support.runs.capacity() - previous_capacity) * sizeof(RLEPair);
     }
    }
   }
   begin = end;
  }
  auto& component = component_for(physical.source);
  CoconutRecoveryImage recovery{physical.image_id, 0, {}};
  if (recovery_)
   recovery_->apply(physical.source, record, static_cast<unsigned>(width), static_cast<unsigned>(height), std::span(support_).first(record.segments.size()), recovery, request_.cancellation, &support_capacity_bytes_);
  auto& index = component.index;
  NormalizedImage image{physical.image_id, index.boxes.size(), 0, static_cast<unsigned>(width), static_cast<unsigned>(height), physical.shard, 0};
  for (std::size_t ordinal = 0; ordinal < record.segments.size(); ++ordinal) {
   if (ordinal > UINT64_MAX - record.first_segment_ordinal) invalid("source ordinal overflow");
   const auto& segment = record.segments[ordinal];
   const auto& support = support_[ordinal];
   ++index.rejected.raw_records;
   if (!segment.isthing) continue;
   const auto& categories = coconut_categories().target_by_id;
   if (segment.category_id >= categories.size() || categories[segment.category_id] < 0) invalid("unknown COCO80 thing category " + std::to_string(segment.category_id));
   if (segment.area && (!std::isfinite(*segment.area) || *segment.area < 0)) invalid("invalid supplied area");
   NormalizedBox box;
   if (support.recovered) {
    box.x1 = support.recovered->x1;
    box.y1 = support.recovered->y1;
    box.x2 = support.recovered->x2;
    box.y2 = support.recovered->y2;
   } else if (segment.bbox) {
    const auto& supplied = *segment.bbox;
    if (!std::ranges::all_of(supplied, [](double value) { return std::isfinite(value); }) || supplied[2] <= 0 || supplied[3] <= 0 || !std::isfinite(supplied[0] + supplied[2]) ||
        !std::isfinite(supplied[1] + supplied[3]))
     invalid("invalid authoritative COCO bbox");
    box.x1 = static_cast<float>(supplied[0] / width);
    box.y1 = static_cast<float>(supplied[1] / height);
    box.x2 = static_cast<float>((supplied[0] + supplied[2]) / width);
    box.y2 = static_cast<float>((supplied[1] + supplied[3]) / height);
   } else {
    if (support.area == 0) {
     ++index.rejected.degenerate_boxes;
     ++recovery.unresolved;
     if (component.recovery_policy) recovery.omissions.push_back({segment.id, record.first_segment_ordinal + ordinal, segment.category_id, 0});
     if (request_.rejected_object) {
      try {
       request_.rejected_object(physical, record, segment, "thing segment has neither mask pixels nor an authoritative bbox");
      } catch (...) {
       // Reporting a discarded object cannot interrupt compilation.
      }
     }
     continue;
    }
    box.x1 = static_cast<float>(static_cast<double>(support.bounds.min_x) / width);
    box.y1 = static_cast<float>(static_cast<double>(support.bounds.min_y) / height);
    box.x2 = static_cast<float>(static_cast<double>(support.bounds.max_x) / width);
    box.y2 = static_cast<float>(static_cast<double>(support.bounds.max_y) / height);
   }
   if (!std::isfinite(box.x1) || !std::isfinite(box.y1) || !std::isfinite(box.x2) || !std::isfinite(box.y2) || box.x2 <= box.x1 || box.y2 <= box.y1)
    invalid("normalized box coordinates are not representable");
   if (support.runs.size() > UINT32_MAX) invalid("normalized RLE count overflow");
   box.mask_rle_offset = index.mask_rle_pairs.size();
   box.mask_rle_pairs = static_cast<std::uint32_t>(support.runs.size());
   box.class_id = static_cast<std::uint8_t>(categories[segment.category_id]);
   box.flags = kAnnotationMask | kAnnotationId | kAnnotationCategory | (segment.crowd ? kAnnotationCrowd : 0U) | (segment.ignore ? kAnnotationIgnore : 0U);
   box.original_area = support.recovered ? support.recovered->original_area : support.carved ? static_cast<double>(support.area) : segment.area.value_or(static_cast<double>(support.area));
   box.annotation_id = segment.id;
   box.source_category_id = segment.category_id;
   box.source_ordinal = record.first_segment_ordinal + ordinal;
   index.mask_rle_pairs.insert(index.mask_rle_pairs.end(), support.runs.begin(), support.runs.end());
   index.boxes.push_back(box);
   ++image.box_count;
  }
  index.images.push_back(image);
  component.inventory.push_back({physical, record.image_id, record.source_ordinal});
  if (component.recovery_policy) component.recovery.push_back(std::move(recovery));
 }
 const CoconutImportRequest& request_;
 bool group_ = false;
 std::unique_ptr<CoconutMaskRecovery> recovery_;
 std::mutex groups_mutex_;
 std::vector<std::unique_ptr<Importer>> groups_;
 std::span<const CoconutRecordGroup> group_prefixes_;
 std::uint64_t support_capacity_bytes_ = 0;
 std::unordered_set<PhysicalKey, PhysicalKeyHash> offered_;
 std::map<CoconutImageNamespace, CoconutComponentBuilder> components_;
 struct SegmentEntry { std::uint32_t id = 0; std::size_t index = 0; };
 SegmentEntry& segment_entry(std::uint32_t id) {
  auto slot = (static_cast<std::size_t>(id) * 2654435761U) & (segment_by_id_.size() - 1);
  while (segment_by_id_[slot].id && segment_by_id_[slot].id != id) slot = (slot + 1) & (segment_by_id_.size() - 1);
  return segment_by_id_[slot];
 }
 std::vector<SegmentEntry> segment_by_id_;
 std::vector<CoconutSegmentSupport> support_;
 std::uint64_t rows_ = 0;
};
std::uint32_t dimension(const JsonAtom& field, const CoconutImportLimits& limits) {
 if (!field || field.is_null()) return 0;
 const auto value = field.unsigned_integer();
 if (value == 0 || value > limits.max_dimension) invalid("image dimension exceeds admission");
 return static_cast<std::uint32_t>(value);
}
void json_rows(const CoconutImportRequest& request, const PaddedMappedFile& input, std::span<const ByteRange> rows,
 std::vector<JsonParser>& parsers, BenchmarkCompilePipeline::Workspace* workspace,
 const std::function<void(const JsonRow&)>& consume, const std::function<void()>& ready = {}) {
 // Source/lane parser capacity survives bounded callbacks under IdleScratch
 // custody. The typed row borrows it only during this synchronous callback.
 for (std::size_t first = 0; first < rows.size();) {
  auto end = first + 1;
  while (end < rows.size() && rows[end].end - rows[first].begin <= (256U << 10)) ++end;
  const auto parse = [&](std::size_t) {
   auto& storage = parsers[request.execution ? request.execution->current_lane() : 0];
   for (auto i = first; i < end; ++i) {
    throw_if_benchmark_cancelled(request.cancellation);
    const auto range = rows[i];
    auto document = storage.parser.iterate(simdjson::padded_string_view(input.data() + range.begin, range.end - range.begin, input.capacity_from(range.begin)));
    JsonConsumption admission{0, static_cast<std::size_t>(request.limits.max_segments) * 32U + 128U, true};
    auto& row = storage.row;
    row.reset();
    consume_json_value(document.get_value().value(), admission, 2, nullptr, &row);
    if (!row.object) invalid("panoptic row must be an object");
    consume(row);
    if (request.execution && (i - first + 1) % 32 == 0) request.execution->cooperate();
   }
  };
  const auto bytes = mmltk::common::math::checked_add(std::uint64_t{65536}, mmltk::common::math::checked_multiply(std::uint64_t{rows[end - 1].end - rows[first].begin}, 32U, "COCONut JSON workspace overflow"), "COCONut JSON workspace overflow");
  if (request.execution) request.execution->for_each(BenchmarkStage::Metadata, 1, [bytes](std::size_t) { return BenchmarkResources{bytes, 0}; }, parse, *workspace);
  else parse(0);
  if (ready) ready();
  first = end;
 }
}
std::vector<CoconutRecord> json_records(const CoconutImportRequest& request) {
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
 if (request.execution) parser_workspace.emplace(*request.execution, [&](std::size_t lane) noexcept { parsers[lane] = {}; });
 auto* workspace = parser_workspace ? &*parser_workspace : nullptr;
 constexpr std::array<std::string_view, 3> fields{"categories", "images", "annotations"};
 const auto category_row = [&](const JsonRow& row) {
   std::optional<std::uint32_t> id;
   std::optional<std::string> name;
   if (row.get<&CoconutJsonRow::id>()) id = mmltk::common::math::checked_cast<std::uint32_t>(row.get<&CoconutJsonRow::id>().unsigned_integer(), "category ID overflow");
   if (row.get<&CoconutJsonRow::name>()) name = std::string(row.get<&CoconutJsonRow::name>().text());
   category_admission.observe(id, name ? std::optional<std::string_view>(*name) : std::nullopt);
 };
 const auto image_row = [&](const JsonRow& row) {
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
 std::vector<CoconutRecord> result;
 std::vector<bool> joined_images;
 std::uint64_t segment_ordinal = 0;
 std::size_t ready_records = 0;
 const auto annotation_row = [&](const JsonRow& annotation) {
  throw_if_benchmark_cancelled(request.cancellation);
  CoconutRecord record;
  record.source_ordinal = result.size();
  record.first_segment_ordinal = segment_ordinal;
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
   if (joined_images[*image_index]) invalid("multiple annotations join one image row: " + record.file_name);
   joined_images[*image_index] = true;
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
  if (record.segments.size() > UINT64_MAX - segment_ordinal) invalid("segment ordinal overflow");
  segment_ordinal += record.segments.size();
  result.push_back(std::move(record));
 };
 const auto ready = [&] {
  // A ready chunk can start its physical images before later annotation rows
  // parse. Resolution may wait on sources, so it runs after CPU custody ends.
  while (ready_records < result.size())
   (void)request.physical_membership->resolve(request.edition, request.input_identity, result[ready_records++], request.parent_allowance);
 };
 bool categories_complete = false, images_complete = false;
 std::vector<ByteRange> pending_annotations;
 discover_json_arrays(input, fields, true, [&](std::size_t field, std::span<const ByteRange> rows, bool complete) {
  if (field == 0) {
   json_rows(request, input, rows, parsers, workspace, category_row);
   if (complete) { category_admission.complete(); categories_complete = true; }
  } else if (field == 1) {
   json_rows(request, input, rows, parsers, workspace, image_row);
   if (complete) { joined_images.assign(images.size(), false); images_complete = true; }
  } else if (categories_complete && images_complete) json_rows(request, input, rows, parsers, workspace, annotation_row, ready);
  else pending_annotations.insert(pending_annotations.end(), rows.begin(), rows.end());
  if (categories_complete && images_complete && !pending_annotations.empty()) {
   json_rows(request, input, pending_annotations, parsers, workspace, annotation_row, ready);
   std::vector<ByteRange>().swap(pending_annotations);
  }
 }, request.cancellation, request.execution);
 for (std::size_t begin = 0; begin < images.size(); begin += 256) {
  const auto check_joins = [&](std::size_t) {
   for (auto i = begin; i < std::min(images.size(), begin + 256); ++i) {
    throw_if_benchmark_cancelled(request.cancellation);
    if (!joined_images[i]) invalid("offered JSON image has no annotation/mask join: " + images[i].file_name);
   }
  };
  if (request.execution) request.execution->run(BenchmarkStage::Metadata, {}, check_joins);
  else check_joins(0);
 }
 return result;
}
std::vector<CoconutRecord> xlarge_records(const CoconutImportRequest& request, CoconutAnnotationRecords& retained) {
 const auto consumer = request.physical_membership->input_requirement(request.edition);
 const auto workspace = mmltk::common::math::checked_add(128ULL << 20, consumer.workspace_bytes(), "COCONut discovery consumer envelope overflow");
 auto owned = std::make_shared<Archive>(request.mask_archive, request.execution, workspace, request.parent_allowance, 1, true, BenchmarkAllowance{}, 1024, consumer.continuation_descriptors());
 auto& archive = *owned;
 retained.archive = owned;
 std::map<std::string, CoconutRecord> records;
 simdjson::ondemand::parser parser;
 constexpr std::string_view prefix = "coconuts_xlarge/panseg_info/";
 while (archive.next(request.cancellation)) {
  if (!archive.regular() || !archive.member().starts_with(prefix) || !archive.member().ends_with(".json")) continue;
  const auto name = parse_coconut_objects_member(archive.member());
  if (name.source != CoconutImageNamespace::Objects365V2 || archive.member() != std::string(prefix) + name.stem + ".json") invalid("unsupported XL info member: " + archive.member());
  CoconutRecord record;
  record.image_id = name.id;
  record.physical_stem = name.stem;
  const auto bytes = archive.read(16U * 1024U * 1024U, request.cancellation);
  archive.cpu([&] {
   simdjson::padded_string padded(reinterpret_cast<const char*>(bytes.data()), bytes.size());
   auto document = parser.iterate(padded);
   JsonConsumption admission{0, 0, false};
   JsonArray<JsonSegment> segments;
   consume_json_value(document.get_value().value(), admission, 0, nullptr, nullptr, nullptr, &segments);
   segments_from_json(segments, record, request.limits);
  });
  const auto [inserted, unique] = records.emplace(name.stem, std::move(record));
  if (!unique) invalid("duplicate XL info member: " + archive.member());
  // Resolve real image work as each JSON record arrives. XL source ordering is
  // assigned below; its full physical stem is already a stable logical key.
  (void)request.physical_membership->resolve(request.edition, request.input_identity, inserted->second, archive.allowance());
 }
 std::vector<CoconutRecord> result;
 result.reserve(records.size());
 std::uint64_t ordinal = 0;
 for (auto& [name, record] : records) {
  throw_if_benchmark_cancelled(request.cancellation);
  record.source_ordinal = result.size();
  record.first_segment_ordinal = ordinal;
  if (record.segments.size() > UINT64_MAX - ordinal) invalid("segment ordinal overflow");
  ordinal += record.segments.size();
  result.push_back(std::move(record));
 }
 return result;
}
void consume_archive(const CoconutImportRequest& request, std::vector<CoconutRecord>& records, Importer& importer, std::shared_ptr<Archive> owned) {
 const std::string prefix = request.edition == CoconutEdition::XLarge ? "coconuts_xlarge/panseg/" : request.edition == CoconutEdition::Large ? "panoptic_object365/" : "panoptic_o365val_v3/";
 std::vector<std::string> wanted;
 wanted.reserve(records.size());
 std::uint64_t normalizer_workspace = 0;
 for (const auto& record : records) {
  throw_if_benchmark_cancelled(request.cancellation);
  wanted.push_back(prefix + record.physical_stem + ".png");
  normalizer_workspace = std::max(normalizer_workspace, importer.maximum_workspace(record));
 }
 // The library keeps its decompressor and encoded capacity until stream close.
 // Reserve that backing together with its largest legal synchronous consumer,
 // so a borrowed PNG never waits for the scratch needed to retire its input.
 const auto consumer = request.physical_membership->input_requirement(request.edition);
 const auto workspace = mmltk::common::math::checked_add(mmltk::common::math::checked_multiply(request.limits.max_png_bytes, 2U, "COCONut archive buffer overflow"),
  mmltk::common::math::checked_add(normalizer_workspace, consumer.workspace_bytes(), "COCONut retained physical consumer overflow"), "COCONut archive allowance overflow");
 const bool discovered = static_cast<bool>(owned);
 if (owned) owned->resume(workspace);
 else owned = std::make_shared<Archive>(request.mask_archive, request.execution, workspace, request.parent_allowance, 1, true, BenchmarkAllowance{}, 1024, consumer.continuation_descriptors());
 auto& archive = *owned;
 struct RetireConsumer {
  Importer& importer;
  ~RetireConsumer() { importer.retire_scratch(); }
 } retire_consumer{importer};
 if (request.edition == CoconutEdition::XLarge && discovered) {
  archive.visit_known(wanted, [&](std::size_t index) {
   const auto png = archive.read(request.limits.max_png_bytes, request.cancellation);
   importer.consume(records[index], png, archive.allowance(), mmltk::common::math::checked_add(archive.retained_workspace_bytes(), consumer.workspace_bytes(), "COCONut retained archive workspace overflow"));
  }, request.cancellation);
  archive.pause();
  return;
 }
 std::unordered_map<std::string_view, std::size_t> by_member;
 by_member.reserve(wanted.size());
 for (std::size_t i = 0; i < wanted.size(); ++i)
  if (!by_member.emplace(wanted[i], i).second) invalid("duplicate offered mask: " + records[i].physical_stem);
 std::vector<bool> consumed(records.size(), false);
 std::size_t remaining = wanted.size();
 while (remaining && archive.next(request.cancellation)) {
  const auto found = by_member.find(archive.member());
  if (found == by_member.end()) {
   if (archive.member().starts_with(prefix) && archive.member().ends_with(".png")) invalid("extra mask without annotation: " + archive.member());
   continue;
  }
  if (consumed[found->second]) invalid("duplicate archive mask: " + archive.member());
  const auto png = archive.read(request.limits.max_png_bytes, request.cancellation);
  importer.consume(records[found->second], png, archive.allowance(), mmltk::common::math::checked_add(archive.retained_workspace_bytes(), consumer.workspace_bytes(), "COCONut retained archive workspace overflow"));
  consumed[found->second] = true;
  --remaining;
  // Parsed canonical records remain reusable until their source generation retires.
 }
 for (std::size_t i = 0; i < consumed.size(); ++i) {
  throw_if_benchmark_cancelled(request.cancellation);
  if (!consumed[i]) invalid("missing offered mask: " + prefix + records[i].physical_stem + ".png");
 }
}
}  // namespace
CoconutComponent::CoconutComponent(std::shared_ptr<const CoconutComponentBacking> backing, NormalizedAnnotationReadView index,
 std::shared_ptr<CoconutInventorySeal> seal, bool membership) : backing_(std::move(backing)), index_(std::move(index)), seal_(std::move(seal)), membership_(membership) {}
CoconutEdition CoconutComponent::edition() const noexcept { return backing_->edition; }
CoconutImageNamespace CoconutComponent::source() const noexcept { return backing_->source; }
const std::string& CoconutComponent::input_identity() const noexcept { return backing_->input_identity; }
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
CoconutComponent CoconutComponent::membership() const { auto result = *this; result.membership_ = true; return result; }
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
CoconutComponent CoconutComponentBacking::finish(std::shared_ptr<CoconutComponentBacking> backing, NormalizedAnnotationIndex normalized, bool metadata_only,
 Cancellation cancellation, const std::filesystem::path& directory, StorageReservationPool* storage, const CoconutComponent* reuse) {
 admit_component_metadata(*backing, normalized, normalized.images.size(), backing->inventory->size(), backing->recovery.size());
 const auto expected_identity = std::exchange(normalized.annotation_sha256, {});
 normalized.completion.reset();
 backing->index = NormalizedAnnotationReadView(normalized);
 backing->metadata_only = metadata_only;
 if (reuse) {
  backing->inventory = reuse->backing_->inventory;
  backing->seal = reuse->seal_;
 } else backing->seal = seal_inventory(loaded(backing), cancellation, directory, storage);
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
  join.finish(); boxes += image.box_count;
 }
 if (boxes != index.boxes.size() || runs != index.mask_rle_pairs.size()) invalid("component has unreferenced normalized records");
 backing->inventory = std::make_shared<const std::vector<CoconutInventoryImage>>(std::move(inventory));
 backing->recovery = std::move(recovery);
 return CoconutComponentBacking::finish(std::move(backing), assembly.finish(), false, cancellation, directory, storage);
}
std::string coconut_component_input_identity(std::string_view base, CoconutImageNamespace source, const CoconutMaskRecovery* recovery) {
 const auto original = recovery ? recovery->original_identity(source) : std::string_view{};
 if (original.empty()) return std::string(base);
 const auto material = std::string(base) + "\nrecovery:" + std::to_string(kCoconutRecoveryPolicy) + "\n" + std::string(coconut_namespace_name(source)) + "\n" + std::string(original);
 return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size())));
}
void CoconutAnnotationRecords::discard() noexcept {
 archive.reset();
 parquet.reset();
 groups.clear();
 inventories.clear();
 std::vector<CoconutRecord>().swap(records);
 identity.clear();
}
std::vector<CoconutComponent> import_coconut_annotations(const CoconutImportRequest& request) {
 auto retained = request.records;
 struct DiscardFailedRecords {
  std::shared_ptr<CoconutAnnotationRecords>& records;
  int exceptions = std::uncaught_exceptions();
  ~DiscardFailedRecords() { if (records && std::uncaught_exceptions() > exceptions) records->discard(); }
 } discard_failed{retained};
 struct RetireReaders { const CoconutPhysicalMembership* physical; CoconutEdition edition; ~RetireReaders() { if (physical) physical->release_readers(edition); } } retire{request.physical_membership, request.edition};
 (void)coconut_release_component(request.edition);
 Importer importer(request);
 if (!retained) retained = std::make_shared<CoconutAnnotationRecords>();
 if (request.edition == CoconutEdition::Base || request.edition == CoconutEdition::RelabeledValidation) {
  if (retained->identity != request.input_identity) retained->discard();
  if (request.parquet_shards.empty()) invalid("missing Parquet shards");
  read_coconut_parquet(
   request.parquet_shards, request.limits, request.cancellation, [&](std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) { importer.consume_group(group, record, input); }, request.metadata_only, request.execution,
   [&](std::size_t group) { importer.retire_group(group); }, request.parent_allowance, request.physical_membership->input_requirement(request.edition), retained.get(),
   [&](const CoconutRecord& record) { return importer.maximum_workspace(record); },
   [&](const BenchmarkAllowance& producer) { request.physical_membership->release_readers(request.edition, producer); });
  importer.collect_groups(retained->groups);
  retained->identity = request.input_identity;
 } else {
  if (!retained) retained = std::make_shared<CoconutAnnotationRecords>();
  if (retained->identity != request.input_identity) {
   retained->discard();
   retained->records = request.edition == CoconutEdition::XLarge ? xlarge_records(request, *retained) : json_records(request);
   retained->identity = request.input_identity;
   // Discovery and normalization have different complete consumer envelopes.
   // Retire actual backing before obtaining the later grant; member positions
   // and parsed records remain in the same opened source generation.
   if (!request.metadata_only && retained->archive) {
    request.physical_membership->release_readers(request.edition);
    retained->archive->pause();
   }
  }
  if (request.metadata_only) {
   for (const auto& record : retained->records) importer.consume(record, {}, retained->archive ? retained->archive->allowance() : BenchmarkAllowance{});
   // Physical placement now has canonical metadata. Keep parsed records, while
   // releasing the discovery grant before independent mask/pixel work.
   if (retained->archive) retained->archive->pause();
  } else consume_archive(request, retained->records, importer, retained->archive);
 }
 request.physical_membership->release_readers(request.edition);
 return importer.finish();
}
std::uint64_t reconcile_coconut_extensions(std::vector<CoconutComponent>& components, Cancellation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 std::unordered_set<PhysicalKey, PhysicalKeyHash> large, xlarge;
 for (const auto& component : components)
  if (component.edition() == CoconutEdition::Large) {
   for (const auto& image : component.inventory()) {
    throw_if_benchmark_cancelled(cancellation);
    if (!large.insert(physical_key(image.physical.source, image.physical.image_id)).second) invalid("duplicate Large physical image");
   }
  }
 std::uint64_t removed = 0;
 for (auto& component : components)
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
 return removed;
}
std::vector<CoconutPhysicalImage> coconut_image_archive_inventory(
 const std::filesystem::path& archive_path, const std::filesystem::path& cache_path, CoconutImageNamespace source, std::uint16_t shard, std::string archive_identity, Cancellation cancellation, StorageReservationPool* storage,
 BenchmarkCompilePipeline* execution, const BenchmarkAllowance& parent) {
 if (archive_identity.empty()) invalid("archive inventory needs a physical identity");
 (void)coconut_namespace_name(source);
 throw_if_benchmark_cancelled(cancellation);
 InventoryHeader expected;
 expected.source = source;
 expected.shard = shard;
 expected.input_identity = archive_identity;
 if (!cache_path.empty() && std::filesystem::is_regular_file(cache_path)) {
  try {
   InventoryInput input(cache_path, cancellation);
   InventoryHeader header;
   input.value(header);
   validate_inventory_header(header, expected);
   if (header.count > input.maximum_records()) invalid("inventory count exceeds file extent");
   throw_if_benchmark_cancelled(cancellation);
   std::vector<CoconutPhysicalImage> result;
   result.reserve(mmltk::common::math::checked_cast<std::size_t>(header.count, "physical inventory count overflow"));
   for (std::uint64_t i = 0; i < header.count; ++i) {
    throw_if_benchmark_cancelled(cancellation);
    CoconutPhysicalImage image;
    input.value(image);
    validate_coconut_physical_image(image);
    if (image.source != source || image.shard != shard || image.archive_identity != archive_identity || (!result.empty() && image.image_id <= result.back().image_id))
     invalid("invalid cached archive inventory");
    result.push_back(std::move(image));
   }
   (void)input.finish();
   throw_if_benchmark_cancelled(cancellation);
   return result;
  } catch (const std::exception&) { throw_if_benchmark_cancelled(cancellation); }
 }
 Archive archive(archive_path, execution, 0, parent);
 std::vector<CoconutPhysicalImage> result;
 while (archive.next(cancellation)) {
  if (!archive.regular() || !archive.member().ends_with(".jpg")) continue;
  CoconutPhysicalImage image{source, 0, shard, archive.member(), archive_identity};
  if (source == CoconutImageNamespace::Objects365V1 || source == CoconutImageNamespace::Objects365V2) {
   const auto name = parse_coconut_objects_member(image.member);
   if (name.source != source) invalid("physical archive namespace mismatch: " + image.member);
   image.image_id = name.id;
  } else
   image.image_id = parse_coconut_coco_member(image.member);
  validate_coconut_physical_image(image);
  result.push_back(std::move(image));
 }
 throw_if_benchmark_cancelled(cancellation);
 archive.cpu([&] { std::ranges::sort(result, {}, &CoconutPhysicalImage::image_id); });
 throw_if_benchmark_cancelled(cancellation);
 for (std::size_t i = 0; i < result.size(); ++i) {
  throw_if_benchmark_cancelled(cancellation);
  if (i && result[i].image_id == result[i - 1].image_id) invalid("duplicate physical archive image ID");
 }
 expected.count = result.size();
 if (!cache_path.empty()) (void)store_inventory(cache_path, expected, std::span<const CoconutPhysicalImage>(result), cancellation, storage);
 throw_if_benchmark_cancelled(cancellation);
 return result;
}
void store_coconut_component(const std::filesystem::path& index_path, const CoconutComponent& component, Cancellation cancellation, StorageReservationPool* storage) {
 if (component.membership_ || component.backing_->metadata_only) invalid("membership cannot publish a full component");
 admit_coconut_component(component, cancellation);
 const auto& identity = component.seal_->identity;
 const auto inventory_path = std::filesystem::path(index_path.string() + ".inventory");
 auto& seal = *component.seal_;
 const std::lock_guard lock(seal.mutex);
 const auto directory = mmltk::common::io::ensure_parent_directory(inventory_path);
 struct stat destination{};
 if (::stat(directory.c_str(), &destination) != 0) throw mmltk::common::io::errno_error("stat COCONut inventory destination");
 struct stat current{};
 const bool published = seal.published_path == inventory_path && ::stat(inventory_path.c_str(), &current) == 0 &&
  current.st_dev == seal.device && current.st_ino == seal.inode && current.st_size >= 0 && static_cast<std::uint64_t>(current.st_size) == seal.bytes;
 if (!published) {
  bool moved = false;
  if (seal.published_path.empty() && destination.st_dev == seal.device && std::filesystem::is_regular_file(seal.staged.path())) {
   try { seal.staged.publish(inventory_path, cancellation); moved = true; }
   catch (const std::filesystem::filesystem_error& error) { if (error.code() != std::errc::cross_device_link) throw; }
  }
  if (!moved) {
   StorageReservationPool reservations(inventory_path, {}, storage);
   auto copy = BenchmarkStagedArtifact::create(reservations, inventory_path, seal.bytes, "COCONut sealed inventory copy");
   copy.preallocate(seal.bytes);
   for (std::size_t offset = 0; offset < seal.bytes;) {
    throw_if_benchmark_cancelled(cancellation);
    const auto bytes = std::min(std::size_t{65536}, seal.bytes - offset);
    copy.file().pwrite_all(seal.data().data() + offset, bytes, offset); offset += bytes;
   }
   copy.file().sync_data();
   // Retain the inode we are about to publish, so inode-number reuse cannot
   // make a later pathname stat authorize unrelated bytes. The copied bytes
   // are already sealed; this mapping transition performs no decode or hash.
   seal.capture(copy.file());
   seal.staged = {};
   copy.publish(inventory_path, cancellation);
  }
  seal.published_path = inventory_path;
 }
 throw_if_benchmark_cancelled(cancellation);
 Json manifest = Json::object();
 const CoconutCompletionFacts facts{component.edition(), component.source(), component.input_identity(), std::string(kCoconutNormalizationRevision), identity,
  component.inventory().size(), component.recovery_policy(), std::string(component.original_annotation_identity()), component.recovery().size()};
 auto extension = Json::object();
 mmltk::frameworks::reflection::visit_materialized_members<CoconutCompletionFacts>([&]<class Declaration>(const auto& field) {
  const auto& value = facts.*Declaration::pointer;
  if constexpr (mmltk::frameworks::reflection::OptionalValue<typename Declaration::member_type>::value) {
   if (value) extension[field.member_name] = *value;
  } else extension[field.member_name] = value;
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
 return mmltk::common::math::checked_add(completion->size,
  mmltk::common::math::checked_add(completion->proof_bytes, component.backing_->seal->bytes, "COCONut index storage overflow"), "COCONut index storage overflow");
}
std::optional<CoconutComponent> load_coconut_component(
 const std::filesystem::path& index_path, CoconutEdition edition, CoconutImageNamespace source, std::string_view input_identity, Cancellation cancellation, bool metadata_only) {
 throw_if_benchmark_cancelled(cancellation);
 try {
  std::uint64_t completion_bytes = 0;
  const auto manifest = read_json_file(index_path.string() + ".complete.json", &completion_bytes);
  const auto& extension = manifest.at("coconut");
  CoconutCompletionFacts facts;
  mmltk::frameworks::reflection::visit_materialized_members<CoconutCompletionFacts>([&]<class Declaration>(const auto& field) {
   if constexpr (mmltk::frameworks::reflection::OptionalValue<typename Declaration::member_type>::value) {
    if (const auto found = extension.find(field.member_name); found != extension.end())
     facts.*Declaration::pointer = found->template get<mmltk::frameworks::reflection::OptionalValueT<typename Declaration::member_type>>();
   } else extension.at(field.member_name).get_to(facts.*Declaration::pointer);
  });
  if (facts.edition != edition || facts.source != source || facts.input_identity != input_identity || facts.normalization != kCoconutNormalizationRevision) return std::nullopt;
  auto component = std::make_shared<CoconutComponentBacking>();
  component->edition = edition; component->source = source; component->input_identity = input_identity;
  auto index = load_normalized_annotation_index(index_path, index_source(source), component_split(edition, source), facts.inventory_identity, cancellation, {}, &manifest, metadata_only, completion_bytes);
  if (!index) return std::nullopt;
  component->index = NormalizedAnnotationReadView(std::move(*index));
  InventoryInput input(index_path.string() + ".inventory", cancellation);
  InventoryHeader header;
  input.value(header);
  validate_inventory_header(header, component_header(*component, component->index.image_count()));
  if (header.count > input.maximum_records() || facts.inventory_count != header.count || header.count != component->index.image_count()) invalid("invalid component inventory count");
  throw_if_benchmark_cancelled(cancellation);
  auto inventory = std::make_shared<std::vector<CoconutInventoryImage>>();
  inventory->reserve(mmltk::common::math::checked_cast<std::size_t>(header.count, "component inventory count overflow"));
  ComponentInventoryAdmission admission(source);
  for (std::size_t i = 0; i < header.count; ++i) {
   throw_if_benchmark_cancelled(cancellation);
   CoconutInventoryImage image;
   input.value(image); admission.image(image, component->index.image(i));
   inventory->push_back(std::move(image));
  }
  component->inventory = std::move(inventory);
  component->recovery_policy = facts.recovery_policy.value_or(0);
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
    input.value(fact); admit_recovery_image(fact, image);
    if (!metadata_only) {
     ComponentRecoveryAdmission join(&fact);
     if (!fact.objects.empty() || !fact.omissions.empty())
      for (const auto& box : component->index.storage().boxes.subspan(static_cast<std::size_t>(image.first_box), image.box_count)) join.box(box);
     join.finish();
    }
   }
  } else if (!facts.original_annotation_identity.value_or("").empty() || facts.recovery_images.value_or(0)) invalid("unexpected recovery completion");
  admit_component_metadata(*component, component->index, component->index.image_count(), component->inventory->size(), component->recovery.size());
  const auto identity = input.finish();
  if (facts.inventory_identity != identity) invalid("component inventory completion mismatch");
  component->seal = input.seal(); component->seal->identity = identity;
  component->completion = component->index.completion;
  if (!metadata_only) std::call_once(component->full_admission, [] {});
  throw_if_benchmark_cancelled(cancellation);
  return CoconutComponentBacking::loaded(std::move(component));
 } catch (const std::exception&) {
  throw_if_benchmark_cancelled(cancellation);
  return std::nullopt;
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
