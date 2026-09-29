#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/detail/benchmark_json.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_json.h"
#include <tuple>
#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_annotations.h"
#include <exception>
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
#include <condition_variable>
#include <deque>
#include <thread>
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
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
  device = status.st_dev;
  inode = status.st_ino;
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
 CoconutPhysicalDependencies dependencies;
 std::vector<std::string> image_dependencies;
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
  if (!value.object) invalid("segment must be an object");
  CoconutSegment segment;
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
  if (file_) {
   file_->resize(byte_size(), "COCONut inventory digest extent");
   file_->file().pwrite_all(digest.data(), digest.size(), offset_);
   file_->reconcile();
  }
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
  if (file_) {
   file_->resize(offset_ + used_ + 32, "COCONut inventory extent");
   file_->file().pwrite_all(buffer_.data(), used_, offset_);
   file_->reconcile();
  }
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
  seal_->directory = path.parent_path();
  seal_->published_path = path;
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
 header.component = true;
 header.input_identity = component.input_identity;
 header.edition = component.edition;
 header.source = component.source;
 header.count = count;
 return header;
}
void validate_inventory_header(const InventoryHeader& header, const InventoryHeader& expected) {
 if (header.magic != expected.magic || header.version != expected.version || header.cache_schema != expected.cache_schema || header.normalization != expected.normalization ||
     header.input_identity != expected.input_identity || header.edition != expected.edition || header.source != expected.source || header.shard != expected.shard ||
     header.component != expected.component)
  invalid("inventory identity mismatch");
}
template <class Records>
std::string encode_inventory(InventoryOutput& output, const InventoryHeader& header, const Records& records, Cancellation cancellation, std::uint32_t policy = 0, std::string_view original = {},
 const CoconutComponent* recovery = nullptr) {
 output.value(header);
 for (const auto& record : records) {
  throw_if_benchmark_cancelled(cancellation);
  output.value(record);
 }
 if (policy) {
  output.value(policy);
  output.value(std::string(original));
  output.value(static_cast<std::uint64_t>(recovery->recovery().size()));
  for (const auto& image : recovery->recovery()) {
   throw_if_benchmark_cancelled(cancellation);
   output.value(image);
  }
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
 seal->identity = encode_inventory(
  output, component_header(metadata, component.index().image_count()), component.inventory(), cancellation, component.recovery_policy(), component.original_annotation_identity(), &component);
 seal->staged.file().sync_data();
 seal->capture(seal->staged.file());
 seal->staged.close();
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
 staging.file().sync_data();
 staging.publish(path, cancellation);
 return identity;
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
 void image(const CoconutInventoryImage& row, const NormalizedImage& normalized, bool physical_admitted = false) {
  if (!physical_admitted) validate_coconut_physical_image(row.physical);
  if (row.physical.source != source_ || row.physical.image_id != normalized.source_image_id || row.physical.shard != normalized.source_shard || !ordinals_.insert(row.source_ordinal).second ||
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
 const CoconutRecord* record = nullptr;
 std::size_t position = 0;
 CoconutPhysicalImage physical;
 dataset::MaskDimensions dimensions;
 std::vector<CoconutSegmentSupport> support;
 std::shared_ptr<const CoconutNativeImage> product;
 BenchmarkSourcePublication publication;
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
  const auto pixels = record.width && record.height ? std::uint64_t{record.width} * record.height : request_.limits.max_pixels;
  return mmltk::common::math::checked_add(retained,
   mmltk::common::math::checked_multiply(
    assembly_workspace_.workspace_bytes(record, pixels, request_.limits.max_segments, request_.limits.max_png_bytes), 2U, "COCONut retained normalizer workspace overflow"),
   "COCONut retained normalizer workspace overflow");
 }
 void consume_group(std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) {
  if (input.membership && !request_.metadata_only) {
   (void)request_.physical_membership->resolve(request_.edition, request_.input_identity, record, input.allowance);
   if (request_.execution && input.allowance)
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
 bool reusable_native(const CoconutRecord& record, const BenchmarkAllowance& allowance) {
  const auto& row = request_.records->row(record.source_ordinal);
  if (!row.complete() || !row.native()) return false;
  const auto physical = request_.physical_membership->resolve(request_.edition, request_.input_identity, record, allowance);
  auto original = original_input(physical.source);
  if (!original.terminal) return false;
  const auto& saved = row.native();
  const auto& lineage = saved->lineage().component;
  const auto identity = original.originals ? original.originals->identity(physical.source) : request_.recovery ? request_.recovery->original_identity(physical.source) : std::string_view{};
  return lineage.annotation_input_identity == request_.input_identity && saved->inventory().physical == physical && lineage.original_generation == original.generation &&
         lineage.original_annotation_identity == identity;
 }
 void consume(
  const CoconutRecord& record, std::span<const std::uint8_t> png, BenchmarkAllowance allowance = {}, std::uint64_t input_live_bytes = 0, Worker* selected = nullptr, std::size_t position = SIZE_MAX) {
  if (position == SIZE_MAX) position = record.source_ordinal;
  auto& worker = selected ? *selected : archive_worker_;
  auto& workspace = worker.native;
  const auto physical = request_.physical_membership->resolve(request_.edition, request_.input_identity, record, allowance ? allowance : request_.parent_allowance);
  const bool normalize = !request_.metadata_only && std::ranges::find(request_.retained_sources, physical.source) == request_.retained_sources.end();
  const auto original = normalize ? original_input(physical.source) : CoconutOriginalInput{{}, true, 0};
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
  if (normalize && !saved && !reused && png.empty()) throw CoconutOriginalChanged(physical.source);
  dataset::MaskDimensions dimensions{};
  const auto inspect = [&](std::size_t) {
   if (!normalize || reused || saved) return;
   try {
    dimensions = workspace.admit_png(record, png);
   } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) {
    invalid(physical.member + ": " + error.what());
   }
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
       reused->recovery_policy() ? &reused->recovery_image(reused_position) : nullptr, lineage, reused->image_input_identity(reused_position));
     else {
      workspace.decode(record, png, dimensions);
      if (deferred) {
       auto native = std::make_shared<PendingNativeImage>();
       native->record = &record;
       native->position = position;
       native->physical = physical;
       native->dimensions = dimensions;
       native->publication = publication;
       native->support = workspace.take_support(record.segments.size());
       defer_native(std::move(native));
      } else
       complete = workspace.finish(record, physical, dimensions, lineage, request_.rejected_object);
     }
    }
   } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) {
    invalid(physical.member + ": " + error.what());
   }
   const std::lock_guard lock(groups_mutex_);
   ++rows_;
  };
  const auto run = [&] {
   if (request_.execution)
    request_.execution->run(request_.metadata_only ? BenchmarkStage::Metadata : BenchmarkStage::Normalize, {bytes, 0}, work, allowance);
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
 std::vector<CoconutComponent> finish() {
  {
   const std::lock_guard lock(groups_mutex_);
   input_done_ = true;
  }
  pending_changed_.notify_all();
  if (recovery_controller_.joinable()) recovery_controller_.join();
  if (recovery_failure_) std::rethrow_exception(recovery_failure_);
  if (request_.progress && completed_rows_ % kProgressQuantum != 0) request_.progress(completed_rows_);
  throw_if_benchmark_cancelled(request_.cancellation);
  if (rows_ == 0) invalid("selected release contains no offered image rows");
  if (request_.expected_rows != 0 && rows_ != request_.expected_rows) invalid("offered row count does not match the selected release");
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
  };
  std::uint64_t image_count = 0, recovery_join_boxes = 0;
  for (const auto& row : request_.records->rows())
   if (const auto& native = row.native(); !request_.metadata_only && native && std::ranges::find(request_.retained_sources, native->lineage().component.source) == request_.retained_sources.end()) {
    ++image_count;
    if (native->lineage().component.recovery_policy) recovery_join_boxes = std::max(recovery_join_boxes, std::uint64_t{native->boxes().size()});
   }
  for (const auto& [source, component] : components_) {
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
  auto merge_allowance = request_.execution ? request_.execution->reserve({workspace, 0}) : BenchmarkAllowance{};
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
   if (!request_.metadata_only)
    for (const auto& row : request_.records->rows()) {
     const auto& native = row.native();
     if (!native || std::ranges::find(request_.retained_sources, native->lineage().component.source) != request_.retained_sources.end()) continue;
     if (row.segment_ordinal() < native->segment_begin()) invalid("native chunk ordinal exceeds its canonical row");
     gather({&native->lineage().component, &native->lineage().index, &native->image(), native->boxes(), native->runs(), &native->inventory(), &native->recovery(),
             row.segment_ordinal() - native->segment_begin(), &native->input_identity()},
      native->rejected());
    }
   for (auto& [source, component] : components_) {
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
   if (request_.originals && (source == CoconutImageNamespace::CocoTrain || source == CoconutImageNamespace::CocoValidation)) {
    auto original = request_.originals(source, true, {});
    original_owner_ = std::move(original.originals);
    assembly_workspace_.originals(original_owner_);
   }
   const auto& header = *rows.front().component;
   auto sealed = std::make_shared<CoconutComponentBacking>();
   static_cast<CoconutComponentMetadata&>(*sealed) = header;
   auto inventory = std::make_shared<std::vector<CoconutInventoryImage>>();
   const CoconutComponent* reusable = nullptr;
   if (!sealed->recovery_policy && request_.records && request_.records->identity == request_.input_identity)
    for (const auto& candidate : request_.records->inventories)
     if (!candidate.index().selected() && !candidate.recovery_policy() && candidate.edition() == sealed->edition && candidate.source() == source &&
         candidate.matches_inputs(request_.input_identity, *request_.physical_membership, assembly_workspace_.recovery()) && candidate.index().image_count() == rows.size()) {
      reusable = &candidate;
      break;
     }
   NormalizedAnnotationIndex normalized;
   const auto assemble = [&](std::size_t) {
    const auto physical = [](const ImageChunk& row) -> const CoconutPhysicalImage& { return row.inventory->physical; };
    std::ranges::sort(rows, [&](const auto& left, const auto& right) { return physical(left).image_id < physical(right).image_id; });
    inventory->reserve(rows.size());
    sealed->image_dependencies.reserve(rows.size());
    if (sealed->recovery_policy) sealed->recovery.reserve(rows.size());
    ComponentInventoryAdmission inventory_admission(source);
    NormalizedAnnotationMetadata metadata = *rows.front().metadata;
    metadata.rejected = rejected[source];
    const auto [boxes, runs] = extents[source];
    std::optional<NormalizedAnnotationAssembler> assembly;
    if (!request_.metadata_only) assembly.emplace(metadata, rows.size(), boxes, runs, request_.cancellation);
    NormalizedAnnotationBuilder membership;
    static_cast<NormalizedAnnotationMetadata&>(membership) = metadata;
    if (request_.metadata_only) membership.images.reserve(rows.size());
    for (const auto& row : rows) {
     throw_if_benchmark_cancelled(request_.cancellation);
     const auto& image = *row.image;
     if (reusable && *row.inventory != reusable->inventory_image(inventory->size())) reusable = nullptr;
     inventory_admission.image(*row.inventory, image, reusable != nullptr);
     sealed->dependencies.add(row.inventory->physical);
     sealed->image_dependencies.push_back(
      row.dependency ? *row.dependency : coconut_image_input_identity(sealed->annotation_input_identity, row.inventory->physical, sealed->original_annotation_identity));
     if (sealed->recovery_policy) {
      auto recovery = *row.recovery;
      for (auto* objects : {&recovery.objects, &recovery.omissions})
       for (auto& object : *objects) object.source_ordinal = mmltk::common::math::checked_add(object.source_ordinal, row.segment_base, "COCONut recovery ordinal overflow");
      admit_recovery_image(recovery, image);
      sealed->recovery.push_back(std::move(recovery));
     }
     ComponentRecoveryAdmission recovery_admission(sealed->recovery_policy ? &sealed->recovery.back() : nullptr);
     if (request_.metadata_only)
      membership.images.push_back(image);
     else {
      assembly->begin_image(image);
      for (auto box : row.boxes) {
       box.source_ordinal = mmltk::common::math::checked_add(box.source_ordinal, row.segment_base, "COCONut segment ordinal overflow");
       recovery_admission.box(box);
       assembly->append_box(box, row.runs.subspan(static_cast<std::size_t>(box.mask_rle_offset), box.mask_rle_pairs));
      }
      recovery_admission.finish();
     }
     inventory->push_back(*row.inventory);
    }
    normalized = request_.metadata_only ? seal_normalized_annotation_metadata(std::move(membership)) : assembly->finish();
   };
   if (request_.execution)
    request_.execution->run(BenchmarkStage::Metadata, {}, assemble, merge_allowance);
   else
    assemble(0);
   sealed->input_identity = coconut_component_input_identity(request_.input_identity, source, assembly_workspace_.recovery(), request_.physical_membership, request_.edition, sealed->dependencies);
   sealed->inventory = std::move(inventory);
   auto output = request_.execution && !reusable ? request_.execution->reserve(BenchmarkResources::handles(1, false), request_.parent_allowance) : BenchmarkAllowance{};
   auto component = CoconutComponentBacking::finish(
    std::move(sealed), std::move(normalized), request_.metadata_only, request_.cancellation, request_.inventory_directory, request_.execution ? &request_.execution->storage() : nullptr, reusable);
   if (request_.records && request_.metadata_only) request_.records->inventories.push_back(component);
   result.push_back(std::move(component));
  }
  throw_if_benchmark_cancelled(request_.cancellation);
  if (!request_.metadata_only) request_.records->retire_native();
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
  if (request_.progress && completed_rows_ % kProgressQuantum == 0) request_.progress(completed_rows_);
 }
 void settle_report(const CoconutNativeImage& image) {
  // Recovery omissions are reported later by the recipe. Metadata, retained
  // products and healthy rows must not flush another producer's report bytes.
  if (image.rejected().degenerate_boxes && !image.lineage().component.recovery_policy && request_.image_terminal) request_.image_terminal();
 }
 void publish_labels(std::shared_ptr<const CoconutNativeImage> image, const BenchmarkSourcePublication& publication) {
  if (!request_.execution) return;
  const auto& physical = image->inventory().physical;
  const auto& lineage = image->lineage().component;
  request_.execution->labels_ready(publication, physical.image_id, CoconutNativeImage::labels(image), image->input_identity(), lineage.original_generation);
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
    const auto count = std::min(pending_.size(), lanes * 2);
    batch.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
     batch.push_back(std::move(pending_.front()));
     pending_.pop_front();
    }
   }
   const auto source = batch.front()->physical.source;
   const auto original = request_.originals(source, true, stop);
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
    const auto support = mmltk::common::math::checked_multiply<std::uint64_t>(native.support.size(), std::uint64_t{1024}, "COCONut recovery support overflow");
    const auto pixels = std::uint64_t{native.dimensions.width} * native.dimensions.height;
    return BenchmarkResources{
     mmltk::common::math::checked_add(
      mmltk::common::math::checked_add(mmltk::common::math::checked_add(retained, support, "COCONut retained recovery overflow"), pixels * 32U, "COCONut carving workspace overflow"),
      sizing.recovery() ? sizing.recovery()->workspace_bytes(*native.record) : 0, "COCONut recovery workspace overflow"),
     0
    };
   };
   const auto recover = [&](std::size_t i) {
    const auto lane = request_.execution ? request_.execution->current_lane() : 0;
    auto& worker = workers[lane];
    if (!worker) worker = std::make_unique<CoconutNativeWorkspace>(request_.limits, request_.cancellation);
    worker->originals(original.originals);
    auto& native = batch[i];
    worker->borrow_support(*native->record, native->support, native->dimensions, native);
    native->product = worker->finish(*native->record, native->physical, native->dimensions, lineage_for(native->physical.source, worker->recovery(), original.generation), request_.rejected_object);
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
 std::uint64_t completed_rows_ = 0, rows_ = 0;
 std::map<std::size_t, std::unique_ptr<Worker>> groups_;
 std::vector<std::unique_ptr<Worker>> idle_;
 std::unordered_set<PhysicalKey, PhysicalKeyHash> offered_;
 std::map<CoconutImageNamespace, std::shared_ptr<const CoconutNativeLineage>> lineages_;
 std::map<CoconutImageNamespace, CoconutComponentBuilder> components_;
};
std::uint32_t dimension(const JsonAtom& field, const CoconutImportLimits& limits) {
 if (!field || field.is_null()) return 0;
 const auto value = field.unsigned_integer();
 if (value == 0 || value > limits.max_dimension) invalid("image dimension exceeds admission");
 return static_cast<std::uint32_t>(value);
}
void json_rows(const CoconutImportRequest& request, const PaddedMappedFile& input, std::span<const ByteRange> rows, std::vector<JsonParser>& parsers, BenchmarkCompilePipeline::Workspace* workspace,
 const std::function<void(const JsonRow&, std::size_t)>& consume, const std::function<void(std::size_t, std::size_t)>& ready = {}, bool parallel = false,
 const std::function<bool(std::size_t)>& parsed = {}) {
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
  if (request.execution)
   request.execution->for_each(BenchmarkStage::Metadata, parallel ? end - first : 1, demand, parse, *workspace);
  else if (parallel) {
   for (auto i = first; i < end; ++i) parse(i - first);
  } else
   parse(0);
  if (ready) ready(first, end);
  first = end;
 }
}
void json_records(const CoconutImportRequest& request, CoconutAnnotationRecords& retained) {
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
   while (ready_records < base + end) (void)request.physical_membership->resolve(request.edition, request.input_identity, retained.record(ready_records++), request.parent_allowance);
  };
  json_rows(request, input, ranges, parsers, workspace, annotation_row, ready, true, [&](std::size_t i) { return retained.row(base + i).complete(); });
 };
 bool categories_complete = false, images_complete = false;
 std::vector<ByteRange> pending_annotations;
 discover_json_arrays(input, fields, true, [&](std::size_t field, std::span<const ByteRange> rows, bool complete) {
  if (field == 0) {
   json_rows(request, input, rows, parsers, workspace, category_row);
   if (complete) {
    category_admission.complete();
    categories_complete = true;
   }
  } else if (field == 1) {
   json_rows(request, input, rows, parsers, workspace, image_row);
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
 }, request.cancellation, request.execution);
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
void xlarge_records(const CoconutImportRequest& request, CoconutAnnotationRecords& retained) {
 const auto consumer = request.physical_membership->input_requirement(request.edition);
 const auto nested = mmltk::common::math::checked_multiply(
  std::min<std::uint64_t>(request.limits.max_segments, 8ULL << 20) + 1, std::uint64_t{3 * (sizeof(JsonSegment) + 4 * sizeof(JsonAtom))}, "COCONut XL nested storage overflow");
 const auto workspace = mmltk::common::math::checked_add(
  nested, mmltk::common::math::checked_add(576ULL << 20, consumer.workspace_bytes(), "COCONut discovery consumer envelope overflow"), "COCONut XL parser workspace overflow");
 auto owned = retained.archive;
 if (owned)
  owned->resume(workspace);
 else
  owned = std::make_shared<Archive>(request.mask_archive, request.execution, workspace, request.parent_allowance, 1, true, BenchmarkAllowance{}, 1024, consumer.continuation_descriptors());
 auto& archive = *owned;
 retained.archive = owned;
 std::unordered_map<std::string, std::size_t> records;
 records.reserve(retained.size());
 std::unordered_set<std::string> encountered;
 for (std::size_t i = 0; i < retained.size(); ++i) records.emplace(retained.record(i).physical_stem, i);
 simdjson::ondemand::parser parser;
 std::vector<char> padded;
 JsonArray<JsonSegment> segments;
 constexpr std::string_view prefix = "coconuts_xlarge/panseg_info/";
 while (archive.next(request.cancellation)) {
  if (!archive.regular() || !archive.member().starts_with(prefix) || !archive.member().ends_with(".json")) continue;
  const auto name = parse_coconut_objects_member(archive.member());
  if (name.source != CoconutImageNamespace::Objects365V2 || archive.member() != std::string(prefix) + name.stem + ".json") invalid("unsupported XL info member: " + archive.member());
  if (!encountered.insert(name.stem).second) invalid("duplicate XL info member: " + archive.member());
  if (const auto found = records.find(name.stem); found != records.end()) {
   (void)request.physical_membership->resolve(request.edition, request.input_identity, retained.record(found->second), archive.allowance());
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
  const auto [inserted, unique] = records.emplace(name.stem, position);
  if (!unique) invalid("duplicate XL info member: " + archive.member());
  retained.resize(position + 1);
  retained.admit_json(position, std::move(record));
  // Resolve real image work as each JSON record arrives. XL source ordering is
  // assigned below; its full physical stem is already a stable logical key.
  (void)request.physical_membership->resolve(request.edition, request.input_identity, retained.record(inserted->second), archive.allowance());
 }
 if (encountered.size() != records.size()) invalid("retained XL row count mismatch");
 throw_if_benchmark_cancelled(request.cancellation);
 retained.seal_xlarge_order();
}
void consume_archive(const CoconutImportRequest& request, const CoconutAnnotationRecords& records, Importer& importer, std::shared_ptr<Archive> owned) {
 const std::string prefix = request.edition == CoconutEdition::XLarge ? "coconuts_xlarge/panseg/" : request.edition == CoconutEdition::Large ? "panoptic_object365/" : "panoptic_o365val_v3/";
 std::vector<std::string> wanted;
 wanted.reserve(records.size());
 std::uint64_t normalizer_workspace = 0;
 for (const auto& row : records.rows()) {
  const auto& record = row.record();
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
 if (owned)
  owned->resume(workspace);
 else
  owned = std::make_shared<Archive>(request.mask_archive, request.execution, workspace, request.parent_allowance, 1, true, BenchmarkAllowance{}, 1024, consumer.continuation_descriptors());
 auto& archive = *owned;
 struct RetireConsumer {
  Importer& importer;
  ~RetireConsumer() { importer.retire_scratch(); }
 } retire_consumer{importer};
 if (request.edition == CoconutEdition::XLarge && discovered) {
  archive.visit_known(wanted, [&](std::size_t index) {
   const auto png = archive.read(request.limits.max_png_bytes, request.cancellation);
   importer.consume(records.record(index), png, archive.allowance(),
    mmltk::common::math::checked_add(archive.retained_workspace_bytes(), consumer.workspace_bytes(), "COCONut retained archive workspace overflow"), nullptr, index);
  }, request.cancellation);
  archive.pause();
  return;
 }
 std::unordered_map<std::string_view, std::size_t> by_member;
 by_member.reserve(wanted.size());
 for (std::size_t i = 0; i < wanted.size(); ++i)
  if (!by_member.emplace(wanted[i], i).second) invalid("duplicate offered mask: " + records.record(i).physical_stem);
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
  importer.consume(records.record(found->second), png, archive.allowance(),
   mmltk::common::math::checked_add(archive.retained_workspace_bytes(), consumer.workspace_bytes(), "COCONut retained archive workspace overflow"), nullptr, found->second);
  consumed[found->second] = true;
  --remaining;
  // Parsed canonical records remain reusable until their source generation retires.
 }
 for (std::size_t i = 0; i < consumed.size(); ++i) {
  throw_if_benchmark_cancelled(request.cancellation);
  if (!consumed[i]) invalid("missing offered mask: " + prefix + records.record(i).physical_stem + ".png");
 }
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
 const auto material = nlohmann::json{
  {"domain", "coconut-component-input-v2"}, {"annotations", base}, {"source", source}, {"physical", physical ? physical->dependency_identity(edition, source, dependencies, current) : std::string{}},
  {"recovery_policy", original.empty() ? 0 : kCoconutRecoveryPolicy}, {"original", original}
 }.dump();
 return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size())));
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
 if (!row.metadata_ready()) invalid("segments have no admitted image row");
 row.record_.segments = std::move(segments);
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
 if (row.native_) invalid("cannot change a published JSON ordinal");
 row.record_.first_segment_ordinal = row.segment_ordinal_ = ordinal;
}
void CoconutAnnotationRecords::seal_xlarge_order() {
 // Validate before moving any owned rows. Sorting and ordinal assignment then
 // form one nonallocating commit; cancellation cannot leave a partial move.
 std::uint64_t ordinal = 0;
 for (const auto& row : rows_) {
  if (!row.complete() || row.native_) invalid("cannot reorder an incomplete or published annotation row");
  ordinal = mmltk::common::math::checked_add(ordinal, std::uint64_t{row.record_.segments.size()}, "segment ordinal overflow");
 }
 std::ranges::sort(rows_, {}, [](const Row& row) -> const std::string& { return row.record_.physical_stem; });
 ordinal = 0;
 for (std::size_t i = 0; i < rows_.size(); ++i) {
  auto& row = rows_[i];
  row.record_.source_ordinal = i;
  row.record_.first_segment_ordinal = row.segment_ordinal_ = ordinal;
  ordinal += row.record_.segments.size();
 }
 document_complete = true;
}
void CoconutAnnotationRecords::native(std::size_t position, std::shared_ptr<const CoconutNativeImage> product) {
 auto& row = rows_.at(position);
 if (product && !row.complete()) invalid("native image has no complete annotation row");
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
std::vector<CoconutComponent> import_coconut_annotations(const CoconutImportRequest& supplied) {
 auto retained = supplied.records ? supplied.records : std::make_shared<CoconutAnnotationRecords>();
 auto request = supplied;
 request.records = retained;
 bool preserve = false;
 (void)coconut_release_component(request.edition);
 Importer importer(request);
 struct DiscardFailedRecords {
  std::shared_ptr<CoconutAnnotationRecords>& records;
  const bool& preserve;
  Cancellation cancellation;
  int exceptions = std::uncaught_exceptions();
  ~DiscardFailedRecords() {
   if (records && std::uncaught_exceptions() > exceptions && !preserve && !cancellation.requested()) records->discard();
  }
 } discard_failed{retained, preserve, request.cancellation};
 struct RetireReaders {
  const CoconutPhysicalMembership* physical;
  CoconutEdition edition;
  ~RetireReaders() {
   if (physical) physical->release_readers(edition);
  }
 } retire{request.physical_membership, request.edition};
 try {
  if (request.edition == CoconutEdition::Base || request.edition == CoconutEdition::RelabeledValidation) {
   if (retained->identity != request.input_identity) retained->discard();
   retained->identity = request.input_identity;
   if (request.parquet_shards.empty()) invalid("missing Parquet shards");
   read_coconut_parquet(request.parquet_shards, request.limits, request.cancellation,
    [&](std::size_t group, const CoconutRecord& record, const CoconutAnnotationInput& input) { importer.consume_group(group, record, input); }, request.metadata_only, request.execution,
    [&](std::size_t group) { importer.retire_group(group); }, request.parent_allowance, request.physical_membership->input_requirement(request.edition), retained.get(),
    [&](const CoconutRecord& record) { return importer.maximum_workspace(record); }, [&](const BenchmarkAllowance& producer) {
    importer.retire_input(producer);
    request.physical_membership->release_readers(request.edition, producer);
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
   if (!retained->document_complete) {
    if (request.edition == CoconutEdition::XLarge)
     xlarge_records(request, *retained);
    else
     json_records(request, *retained);
    // Discovery and normalization have different complete consumer envelopes.
    // Retire actual backing before obtaining the later grant; member positions
    // and parsed records remain in the same opened source generation.
    if (!request.metadata_only && retained->archive) {
     request.physical_membership->release_readers(request.edition);
     retained->archive->pause();
    }
   }
   if (request.metadata_only) {
    for (std::size_t i = 0; i < retained->size(); ++i) importer.consume(retained->record(i), {}, retained->archive ? retained->archive->allowance() : BenchmarkAllowance{}, 0, nullptr, i);
    // Physical placement now has canonical metadata. Keep parsed records, while
    // releasing the discovery grant before independent mask/pixel work.
    if (retained->archive) retained->archive->pause();
   } else
    consume_archive(request, *retained, importer, retained->archive);
  }
  request.physical_membership->release_readers(request.edition);
  return importer.finish();
 } catch (...) {
  try {
   throw;
  } catch (const PhysicalArchiveFailure&) { preserve = true; } catch (const CoconutPhysicalMembershipError&) {
   preserve = true;
  } catch (const CoconutOriginalChanged&) { preserve = true; } catch (...) {
  }
  request.physical_membership->release_readers(request.edition);
  if (retained->archive) retained->archive->pause();
  importer.stop_recovery();
  throw;
 }
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
std::vector<CoconutPhysicalImage> coconut_image_archive_inventory(const std::filesystem::path& archive_path, const std::filesystem::path& cache_path, CoconutImageNamespace source, std::uint16_t shard,
 std::string archive_identity, Cancellation cancellation, StorageReservationPool* storage, BenchmarkCompilePipeline* execution, const BenchmarkAllowance& parent) {
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
 const bool published = seal.published_path == inventory_path && ::stat(inventory_path.c_str(), &current) == 0 && current.st_dev == seal.device && current.st_ino == seal.inode &&
                        current.st_size >= 0 && static_cast<std::uint64_t>(current.st_size) == seal.bytes;
 if (!published) {
  bool moved = false;
  if (seal.published_path.empty() && destination.st_dev == seal.device && std::filesystem::is_regular_file(seal.staged.path())) {
   try {
    seal.staged.publish(inventory_path, cancellation);
    moved = true;
   } catch (const std::filesystem::filesystem_error& error) {
    if (error.code() != std::errc::cross_device_link) throw;
   }
  }
  if (!moved) {
   StorageReservationPool reservations(inventory_path, {}, storage);
   auto copy = BenchmarkStagedArtifact::create(reservations, inventory_path, seal.bytes, "COCONut sealed inventory copy");
   copy.preallocate(seal.bytes);
   for (std::size_t offset = 0; offset < seal.bytes;) {
    throw_if_benchmark_cancelled(cancellation);
    const auto bytes = std::min(std::size_t{65536}, seal.bytes - offset);
    copy.file().pwrite_all(seal.data().data() + offset, bytes, offset);
    offset += bytes;
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
 const CoconutCompletionFacts facts{
  component.edition(), component.source(), component.input_identity(), std::string(kCoconutNormalizationRevision), identity, component.inventory().size(), component.recovery_policy(),
  std::string(component.original_annotation_identity()), component.recovery().size()
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
 Cancellation cancellation, bool metadata_only, const CoconutPhysicalMembership* physical, const CoconutMaskRecovery* recovery, bool allow_changed_physical) {
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
   } else
    extension.at(field.member_name).get_to(facts.*Declaration::pointer);
  });
  if (facts.edition != edition || facts.source != source || (!physical && facts.input_identity != input_identity) || facts.normalization != kCoconutNormalizationRevision) return std::nullopt;
  auto component = std::make_shared<CoconutComponentBacking>();
  component->edition = edition;
  component->source = source;
  component->input_identity = facts.input_identity;
  if (physical) component->annotation_input_identity = input_identity;
  auto index =
   load_normalized_annotation_index(index_path, index_source(source), component_split(edition, source), facts.inventory_identity, cancellation, {}, &manifest, metadata_only, completion_bytes);
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
  component->image_dependencies.reserve(static_cast<std::size_t>(header.count));
  for (std::size_t i = 0; i < header.count; ++i) {
   throw_if_benchmark_cancelled(cancellation);
   CoconutInventoryImage image;
   input.value(image);
   admission.image(image, component->index.image(i));
   component->dependencies.add(image.physical);
   component->image_dependencies.push_back(coconut_image_input_identity(component->annotation_input_identity, image.physical, facts.original_annotation_identity.value_or("")));
   inventory->push_back(std::move(image));
  }
  if (physical && facts.input_identity != coconut_component_input_identity(input_identity, source, recovery, physical, edition, component->dependencies, !allow_changed_physical)) return std::nullopt;
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
 } catch (const std::exception&) {
  throw_if_benchmark_cancelled(cancellation);
  return std::nullopt;
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
