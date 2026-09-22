#include "detail/coconut_annotations.h"
#include "detail/coconut_mask_recovery.h"
#include "detail/coconut_inventory.h"
#include "detail/benchmark_storage.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "detail/staging_file_cleanup.h"
#include "src/frameworks/serialization/json_scalar.h"
#include <concepts>
#include <type_traits>
#include <archive.h>
#include <archive_entry.h>
#include <stb_image.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <climits>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
namespace {
using Json = nlohmann::json;
using Cancellation = mmltk::common::concurrency::CancellationObservation;
[[noreturn]] void invalid(std::string_view detail) { throw std::runtime_error("COCONut: " + std::string(detail)); }
std::uint64_t decimal(std::string_view value) {
 std::uint64_t id = 0;
 const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
 if (value.empty() || error != std::errc{} || end != value.data() + value.size() || value.front() == '+' || value.front() == '-')
  invalid("invalid decimal image identity: " + std::string(value));
 return id;
}
struct PhysicalName {
 CoconutImageNamespace source;
 std::uint64_t id;
 std::string stem;
};
PhysicalName objects_name(std::string_view name) {
 const std::filesystem::path path(canonical_coconut_archive_member(name));
 const auto extension = path.extension().string();
 if (!extension.empty() && extension != ".png" && extension != ".jpg" && extension != ".json") invalid("unsupported Objects365 member: " + std::string(name));
 const auto stem = path.stem().string();
 constexpr std::string_view v1 = "objects365_v1_", v2 = "objects365_v2_";
 const auto source = stem.starts_with(v1) ? CoconutImageNamespace::Objects365V1 : CoconutImageNamespace::Objects365V2;
 const auto prefix = source == CoconutImageNamespace::Objects365V1 ? v1 : v2;
 if (!stem.starts_with(prefix) || stem.size() != prefix.size() + 8U) invalid("invalid full Objects365 namespace/member: " + std::string(name));
 return {source, decimal(std::string_view(stem).substr(prefix.size())), stem};
}
std::uint64_t coco_name(std::string_view name) {
 const std::filesystem::path path(canonical_coconut_archive_member(name));
 if ((path.extension() != ".jpg" && path.extension() != ".png") || path.stem().string().size() != 12U) invalid("invalid COCO filename: " + std::string(name));
 return decimal(path.stem().string());
}
struct PhysicalKey {
 CoconutImageNamespace source;
 std::uint64_t id;
 bool operator==(const PhysicalKey&) const = default;
};
struct PhysicalKeyHash {
 std::size_t operator()(PhysicalKey key) const noexcept {
  return std::hash<std::uint64_t>{}(key.id) ^ (static_cast<std::size_t>(key.source) * 0x9e3779b97f4a7c15ULL);
 }
};
PhysicalKey physical_key(CoconutImageNamespace source, std::uint64_t id) { return {source, id}; }
const CategoryLookup& coconut_categories() {
 static const auto lookup = make_numeric_lookup(coco_category_mappings());
 return lookup;
}
void validate_physical(const CoconutPhysicalImage& image) {
 if (image.archive_identity.empty() || image.member != canonical_coconut_archive_member(image.member)) invalid("invalid physical inventory identity");
 if (std::filesystem::path(image.member).extension() != ".jpg") invalid("physical member is not JPEG: " + image.member);
 if (image.source == CoconutImageNamespace::Objects365V1 || image.source == CoconutImageNamespace::Objects365V2) {
  const auto name = objects_name(image.member);
  if (name.source != image.source || name.id != image.image_id) invalid("contradictory Objects365 physical identity: " + image.member);
 } else {
  if (coco_name(image.member) != image.image_id) invalid("contradictory COCO physical identity: " + image.member);
  const std::string_view directory = image.source == CoconutImageNamespace::CocoTrain       ? "train2017/"
                                     : image.source == CoconutImageNamespace::CocoUnlabeled ? "unlabeled2017/"
                                                                                            : "val2017/";
  if (!image.member.starts_with(directory)) invalid("COCO physical subset disagrees with archive member: " + image.member);
 }
}
class Archive final {
public:
 explicit Archive(const std::filesystem::path& path) : handle_(archive_read_new(), archive_read_free) {
  if (!handle_) invalid("cannot allocate archive reader");
  archive_read_support_filter_all(handle_.get());
  archive_read_support_format_tar(handle_.get());
  archive_read_support_format_zip(handle_.get());
  if (archive_read_open_filename(handle_.get(), path.c_str(), 128U * 1024U) != ARCHIVE_OK) fail(path.string());
 }
 bool next(Cancellation cancellation) {
  throw_if_benchmark_cancelled(cancellation);
  const int status = archive_read_next_header(handle_.get(), &entry_);
  if (status == ARCHIVE_EOF) return false;
  if (status != ARCHIVE_OK) fail("reading archive header");
  const char* name = archive_entry_pathname(entry_);
  if (!name) invalid("archive member has no name");
  member_ = archive_entry_filetype(entry_) == AE_IFDIR && (std::string_view(name) == "." || std::string_view(name) == "./")
             ? "."
             : canonical_coconut_archive_member(name);
  if (archive_entry_symlink(entry_) || archive_entry_hardlink(entry_) ||
      (archive_entry_filetype(entry_) != AE_IFREG && archive_entry_filetype(entry_) != AE_IFDIR))
   invalid("unsupported archive entry: " + member_);
  return true;
 }
 const std::string& member() const { return member_; }
 bool regular() const { return archive_entry_filetype(entry_) == AE_IFREG; }
 std::span<const std::uint8_t> read(std::uint64_t limit, Cancellation cancellation) {
  const auto size = archive_entry_size(entry_);
  if (size < 0 || static_cast<std::uint64_t>(size) > limit || static_cast<std::uint64_t>(size) > std::numeric_limits<std::size_t>::max())
   invalid("archive entry exceeds admission: " + member_);
  bytes_.resize(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < bytes_.size()) {
   throw_if_benchmark_cancelled(cancellation);
   const auto count = archive_read_data(handle_.get(), bytes_.data() + offset, std::min<std::size_t>(bytes_.size() - offset, 128U * 1024U));
   if (count <= 0) fail("truncated archive member " + member_);
   offset += static_cast<std::size_t>(count);
  }
  return bytes_;
 }

private:
 [[noreturn]] void fail(const std::string& context) const {
  const char* detail = archive_error_string(handle_.get());
  invalid(context + ": " + (detail ? detail : "archive failure"));
 }
 std::unique_ptr<archive, decltype(&archive_read_free)> handle_;
 archive_entry* entry_ = nullptr;
 std::string member_;
 std::vector<std::uint8_t> bytes_;
};
std::uint64_t unsigned_field(const Json& value, std::string_view name) {
 return mmltk::frameworks::serialization::decode_json_integer_exact<std::uint64_t>(value.at(std::string(name)));
}
bool flag(const Json& object, std::string_view name, bool required = false) {
 auto field = object.find(std::string(name));
 if (field == object.end()) {
  if (required) invalid("missing flag " + std::string(name));
  return false;
 }
 if (field->is_boolean()) return field->get<bool>();
 if (!field->is_number_integer() || (*field != 0 && *field != 1)) invalid("invalid flag " + std::string(name));
 return *field == 1;
}
void segments_from_json(const Json& input, CoconutRecord& record, const CoconutImportLimits& limits) {
 if (!input.is_array() || input.size() > limits.max_segments) invalid("segment list exceeds admission");
 record.segments.reserve(input.size());
 for (const auto& value : input) {
  CoconutSegment segment;
  const auto id = unsigned_field(value, "id");
  if (id == 0 || id > 0xffffffU) invalid("segment ID exceeds RGB24");
  segment.id = static_cast<std::uint32_t>(id);
  segment.category_id = unsigned_field(value, "category_id");
  segment.isthing = flag(value, "isthing", true);
  segment.crowd = flag(value, "iscrowd");
  segment.ignore = flag(value, "ignore");
  if (value.contains("area") && !value.at("area").is_null()) {
   if (!value.at("area").is_number()) invalid("area must be numeric or null");
   segment.area = value.at("area").get<double>();
   if (!std::isfinite(*segment.area) || *segment.area < 0) invalid("invalid supplied segment area");
  }
  if (value.contains("bbox")) {
   const auto& box = value.at("bbox");
   if (!box.is_array() || box.size() != 4) invalid("invalid supplied COCO bbox");
   segment.bbox = box.get<std::array<double, 4>>();
  }
  record.segments.push_back(segment);
 }
}
template <class T>
concept InventoryRecord = std::same_as<T, CoconutPhysicalImage> || std::same_as<T, CoconutInventoryImage> || std::same_as<T, InventoryHeader> ||
                          std::same_as<T, CoconutRecoveryImage> || std::same_as<T, CoconutRecoveredObject>;
// A single reflected little-endian encoding is used by hashing, writing and reading.
// Strings carry checked uint32 lengths. Only a fixed buffer and the current row
// are transient; records remain in their ordinary typed inventory owner.
class InventoryOutput final {
public:
 InventoryOutput(mmltk::common::io::FileHandle* file, Cancellation cancellation, bool count_only = false)
     : file_(file), cancellation_(cancellation), count_only_(count_only) {}
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
  if (count_only_) return {};
  flush();
  const auto digest = hash_.Finish();
  if (file_) file_->pwrite_all(digest.data(), digest.size(), offset_);
  throw_if_benchmark_cancelled(cancellation_);
  return mmltk::common::io::sha256_hex(digest);
 }

private:
 void append(std::span<const std::uint8_t> bytes) {
  if (count_only_) {
   offset_ = mmltk::common::math::checked_add(offset_, bytes.size(), "inventory extent overflow");
   return;
  }
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
  if (file_) file_->pwrite_all(buffer_.data(), used_, offset_);
  offset_ += used_;
  used_ = 0;
 }
 mmltk::common::io::FileHandle* file_;
 Cancellation cancellation_;
 bool count_only_ = false;
 mmltk::common::io::Sha256Hasher hash_;
 std::array<std::uint8_t, 65536> buffer_{};
 std::size_t used_ = 0, offset_ = 0;
};
class InventoryInput final {
public:
 InventoryInput(const std::filesystem::path& path, Cancellation cancellation)
     : file_(mmltk::common::io::FileHandle::open_readonly(path.string())), cancellation_(cancellation) {
  const auto size = file_.size();
  if (size < 32) invalid("truncated inventory");
  content_size_ = size - 32;
 }
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
 std::string finish() {
  throw_if_benchmark_cancelled(cancellation_);
  if (loaded_ != content_size_ || cursor_ != available_) invalid("inventory has trailing records");
  mmltk::common::io::Sha256Digest expected{};
  file_.pread_all(expected.data(), expected.size(), content_size_);
  const auto actual = hash_.Finish();
  if (actual != expected) invalid("inventory checksum mismatch");
  return mmltk::common::io::sha256_hex(actual);
 }

private:
 void read(std::span<std::uint8_t> output) {
  while (!output.empty()) {
   if (cursor_ == available_) {
    throw_if_benchmark_cancelled(cancellation_);
    available_ = std::min(buffer_.size(), content_size_ - loaded_);
    cursor_ = 0;
    if (!available_) invalid("truncated inventory record");
    file_.pread_all(buffer_.data(), available_, loaded_);
    hash_.Update(std::span(buffer_.data(), available_));
    loaded_ += available_;
   }
   const auto count = std::min(output.size(), available_ - cursor_);
   std::memcpy(output.data(), buffer_.data() + cursor_, count);
   cursor_ += count;
   output = output.subspan(count);
  }
 }
 mmltk::common::io::FileHandle file_;
 Cancellation cancellation_;
 mmltk::common::io::Sha256Hasher hash_;
 std::array<std::uint8_t, 65536> buffer_{};
 std::size_t content_size_ = 0, loaded_ = 0, cursor_ = 0, available_ = 0;
};
InventoryHeader component_header(const CoconutComponent& component) {
 InventoryHeader header;
 header.component = true;
 header.input_identity = component.input_identity;
 header.edition = component.edition;
 header.source = component.source;
 header.count = component.inventory.size();
 return header;
}
void validate_inventory_header(const InventoryHeader& header, const InventoryHeader& expected) {
 if (header.magic != expected.magic || header.version != expected.version || header.cache_schema != expected.cache_schema ||
     header.normalization != expected.normalization || header.input_identity != expected.input_identity || header.edition != expected.edition ||
     header.source != expected.source || header.shard != expected.shard || header.component != expected.component)
  invalid("inventory identity mismatch");
}
template <InventoryRecord T>
std::string encode_inventory(InventoryOutput& output, const InventoryHeader& header, std::span<const T> records, Cancellation cancellation, const CoconutComponent* recovery = nullptr) {
 output.value(header);
 for (const auto& record : records) {
  throw_if_benchmark_cancelled(cancellation);
  output.value(record);
 }
 if (recovery && recovery->recovery_policy) {
  output.value(recovery->recovery_policy);
  output.value(recovery->original_annotation_identity);
  output.value(static_cast<std::uint64_t>(recovery->recovery.size()));
  for (const auto& image : recovery->recovery) {
   throw_if_benchmark_cancelled(cancellation);
   output.value(image);
  }
 }
 return output.finish();
}
std::string component_identity(const CoconutComponent& component, Cancellation cancellation) {
 InventoryOutput output(nullptr, cancellation);
 return encode_inventory(output, component_header(component), std::span(component.inventory), cancellation, &component);
}
template <InventoryRecord T>
std::string store_inventory(const std::filesystem::path& path, const InventoryHeader& header, std::span<const T> records, std::string_view expected_identity,
                            Cancellation cancellation, const CoconutComponent* recovery = nullptr) {
 throw_if_benchmark_cancelled(cancellation);
 (void)mmltk::common::io::ensure_parent_directory(path);
 InventoryOutput extent(nullptr, cancellation, true);
 (void)encode_inventory(extent, header, records, cancellation, recovery);
 require_storage(path, extent.byte_size(), "COCONut inventory staging", {});
 std::string temporary = path.string() + ".tmp.XXXXXX";
 auto file = mmltk::common::io::FileHandle::create_unique_output(temporary, 0);
 StagingFileCleanup cleanup(temporary);
 InventoryOutput output(&file, cancellation);
 const auto identity = encode_inventory(output, header, records, cancellation, recovery);
 if (!expected_identity.empty() && identity != expected_identity) invalid("component inventory/index identity mismatch");
 file.sync_data();
 file = mmltk::common::io::FileHandle{};
 throw_if_benchmark_cancelled(cancellation);
 mmltk::common::io::publish_staged_path_atomically(temporary, path, true);
 cleanup.published();
 return identity;
}
BenchmarkDatasetSource index_source(CoconutImageNamespace source) {
 return source == CoconutImageNamespace::Objects365V1 || source == CoconutImageNamespace::Objects365V2 ? BenchmarkDatasetSource::kObjects365V2
                                                                                                       : BenchmarkDatasetSource::kCoco2017;
}
std::string component_split(CoconutEdition edition, CoconutImageNamespace source) {
 return "coconut-" + std::to_string(static_cast<unsigned>(edition)) + "-" + std::to_string(static_cast<unsigned>(source));
}
// The admitted order applies to each aligned metadata collection. Move owned
// strings and object vectors only after normalized slice admission succeeds.
template <class Row>
void retain_inventory_rows(std::vector<Row>& rows, std::span<const std::size_t> order, bool increasing, Cancellation cancellation) {
 if (increasing) {
  for (std::size_t i = 0; i < order.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   if (i != order[i]) rows[i] = std::move(rows[order[i]]);
  }
  rows.resize(order.size());
 } else {
  std::vector<Row> retained;
  retained.reserve(order.size());
  for (const auto position : order) {
   throw_if_benchmark_cancelled(cancellation);
   retained.push_back(std::move(rows[position]));
  }
  rows = std::move(retained);
 }
}
// Inventory owns physical joins; normalized storage owns slice movement.
void retain_images(CoconutComponent& component, std::span<const std::size_t> order, Cancellation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 if (component.inventory.size() != component.index.images.size()) invalid("component inventory/image count mismatch");
 if (component.recovery_policy ? component.recovery.size() != component.inventory.size() : !component.recovery.empty())
  invalid("component recovery/image count mismatch");
 bool increasing = true;
 for (std::size_t i = 0; i < order.size(); ++i) {
  throw_if_benchmark_cancelled(cancellation);
  if (order[i] >= component.inventory.size()) invalid("component retained image position is invalid");
  increasing = increasing && (i == 0 || order[i - 1] < order[i]);
 }
 // Invalidate admission before destructive compaction or identity settlement.
 component.index.annotation_sha256.clear();
 try {
  retain_normalized_image_slices(component.index, order, cancellation);
  retain_inventory_rows(component.inventory, order, increasing, cancellation);
  if (component.recovery_policy) retain_inventory_rows(component.recovery, order, increasing, cancellation);
  component.index.annotation_sha256 = component_identity(component, cancellation);
 } catch (...) {
  component.index.annotation_sha256.clear();
  throw;
 }
}
class Importer final {
public:
 explicit Importer(const CoconutImportRequest& request) : request_(request) {
  if (request.input_identity.empty()) invalid("missing pinned input identity");
  if (!request.physical_membership) invalid("missing physical membership admission");
  if (request_.progress) request_.progress(0);
 }
 void consume(const CoconutRecord& record, std::span<const std::uint8_t> png) {
  throw_if_benchmark_cancelled(request_.cancellation);
  const auto& physical = resolve(record);
  const auto key = physical_key(physical.source, physical.image_id);
  if (!offered_.insert(key).second) invalid("duplicate offered physical member: " + physical.member);
  try {
   normalize(record, physical, png);
  } catch (const std::exception& error) { invalid(physical.member + ": " + error.what()); }
  ++rows_;
  if (request_.progress && rows_ % kProgressQuantum == 0) request_.progress(rows_);
 }
 std::vector<CoconutComponent> finish() {
  if (request_.progress && rows_ % kProgressQuantum != 0) request_.progress(rows_);
  throw_if_benchmark_cancelled(request_.cancellation);
  if (rows_ == 0) invalid("selected release contains no offered image rows");
  if (request_.expected_rows != 0 && rows_ != request_.expected_rows) invalid("offered row count does not match the selected release");
  std::vector<CoconutComponent> result;
  for (auto& [source, component] : components_) {
   std::vector<std::size_t> order(component.inventory.size());
   for (std::size_t i = 0; i < order.size(); ++i) {
    throw_if_benchmark_cancelled(request_.cancellation);
    order[i] = i;
   }
   throw_if_benchmark_cancelled(request_.cancellation);
   std::ranges::sort(order, [&](auto left, auto right) { return component.inventory[left].physical.image_id < component.inventory[right].physical.image_id; });
   throw_if_benchmark_cancelled(request_.cancellation);
   retain_images(component, order, request_.cancellation);
   result.push_back(std::move(component));
  }
  throw_if_benchmark_cancelled(request_.cancellation);
  return result;
 }

private:
 static constexpr std::uint64_t kProgressQuantum = 64;
 const CoconutPhysicalImage& resolve(const CoconutRecord& record) const {
  if (request_.edition == CoconutEdition::Base || request_.edition == CoconutEdition::RelabeledValidation) {
   if (coco_name(record.file_name) != record.image_id) invalid("COCO row filename/image_id mismatch: " + record.file_name);
   const CoconutPhysicalImage* match = nullptr;
   for (auto source : {CoconutImageNamespace::CocoTrain, CoconutImageNamespace::CocoUnlabeled, CoconutImageNamespace::CocoValidation}) {
    if ((source == CoconutImageNamespace::CocoValidation) != (request_.edition == CoconutEdition::RelabeledValidation)) continue;
    if (const auto* found = request_.physical_membership->find(source, record.image_id)) {
     if (match) invalid("ambiguous COCO train/unlabeled membership: " + record.file_name);
     match = found;
    }
   }
   if (!match)
    throw CoconutPhysicalMembershipError(request_.edition == CoconutEdition::Base ? CoconutImageNamespace::CocoTrain : CoconutImageNamespace::CocoValidation,
                                         record.image_id, "missing physical COCO archive member: " + record.file_name);
   return *match;
  }
  const auto name = objects_name(record.physical_stem);
  if (request_.edition != CoconutEdition::ObjectsValidation && name.source != CoconutImageNamespace::Objects365V2)
   invalid("training extension requires Objects365 v2");
  const auto* found = request_.physical_membership->find(name.source, name.id);
  if (!found) throw CoconutPhysicalMembershipError(name.source, name.id, "missing physical Objects365 archive member: " + record.physical_stem);
  return *found;
 }
 void normalize(const CoconutRecord& record, const CoconutPhysicalImage& physical, std::span<const std::uint8_t> png) {
  const auto& limits = request_.limits;
  if (png.size() > limits.max_png_bytes || png.size() > INT_MAX || png.size() < 26U || std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) != 0 || png[24] != 8 ||
      png[25] != 2)
   invalid("expected bounded 8-bit RGB panoptic PNG");
  int width = 0, height = 0, channels = 0;
  if (!stbi_info_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels) || width <= 0 || height <= 0 || channels != 3 ||
      static_cast<unsigned>(width) > limits.max_dimension || static_cast<unsigned>(height) > limits.max_dimension ||
      static_cast<std::uint64_t>(width) * height > limits.max_pixels || static_cast<std::uint64_t>(width) * height > UINT32_MAX)
   invalid("PNG dimensions exceed admission");
  if ((record.width && record.width != static_cast<unsigned>(width)) || (record.height && record.height != static_cast<unsigned>(height)))
   invalid("declared and PNG dimensions disagree");
  if (record.segments.size() > limits.max_segments) invalid("segment count exceeds admission");
  throw_if_benchmark_cancelled(request_.cancellation);
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels, 3),
                                                              stbi_image_free);
  if (!pixels) invalid("cannot decode panoptic PNG");
  segment_by_id_.clear();
  if (support_.size() < record.segments.size()) support_.resize(record.segments.size());
  for (std::size_t i = 0; i < record.segments.size(); ++i) {
   const auto& segment = record.segments[i];
   if (segment.id == 0 || segment.id > 0xffffffU || !segment_by_id_.emplace(segment.id, i).second) invalid("duplicate/invalid segment ID");
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
    const auto found = segment_by_id_.find(id);
    if (found == segment_by_id_.end()) invalid("PNG references undeclared segment " + std::to_string(id));
    auto& support = support_[found->second];
    support.area += end - begin;
    dataset::include_row_major_mask_run(&support.bounds, begin, end, static_cast<std::uint32_t>(width));
    if (record.segments[found->second].isthing) {
     if (!support.runs.empty() && support.runs.back().start + support.runs.back().length == begin)
      support.runs.back().length += end - begin;
     else
      support.runs.push_back({begin, end - begin});
    }
   }
   begin = end;
  }
  auto [entry, inserted] = components_.try_emplace(physical.source);
  auto& component = entry->second;
  if (inserted) {
   component.edition = request_.edition;
   component.source = physical.source;
   component.input_identity = coconut_component_input_identity(request_.input_identity, physical.source, request_.recovery);
   component.index.source = index_source(physical.source);
   component.index.split = component_split(request_.edition, physical.source);
   if (request_.recovery) {
    component.original_annotation_identity = request_.recovery->original_identity(physical.source);
    if (!component.original_annotation_identity.empty()) component.recovery_policy = kCoconutRecoveryPolicy;
   }
  }
  CoconutRecoveryImage recovery{physical.image_id, 0, {}};
  if (request_.recovery)
   request_.recovery->apply(physical.source, record, static_cast<unsigned>(width), static_cast<unsigned>(height),
                            std::span(support_).first(record.segments.size()), recovery, request_.cancellation);
  auto& index = component.index;
  NormalizedImage image{physical.image_id, index.boxes.size(), 0, static_cast<unsigned>(width), static_cast<unsigned>(height), physical.shard, 0};
  for (std::size_t ordinal = 0; ordinal < record.segments.size(); ++ordinal) {
   if (ordinal > UINT64_MAX - record.first_segment_ordinal) invalid("source ordinal overflow");
   const auto& segment = record.segments[ordinal];
   const auto& support = support_[ordinal];
   ++index.rejected.raw_records;
   if (!segment.isthing) continue;
   const auto& categories = coconut_categories().target_by_id;
   if (segment.category_id >= categories.size() || categories[segment.category_id] < 0)
    invalid("unknown COCO80 thing category " + std::to_string(segment.category_id));
   if (segment.area && (!std::isfinite(*segment.area) || *segment.area < 0)) invalid("invalid supplied area");
   NormalizedBox box;
   if (support.recovered) {
    box.x1 = support.recovered->x1;
    box.y1 = support.recovered->y1;
    box.x2 = support.recovered->x2;
    box.y2 = support.recovered->y2;
   } else if (segment.bbox) {
    const auto& supplied = *segment.bbox;
    if (!std::ranges::all_of(supplied, [](double value) { return std::isfinite(value); }) || supplied[2] <= 0 || supplied[3] <= 0 ||
        !std::isfinite(supplied[0] + supplied[2]) || !std::isfinite(supplied[1] + supplied[3]))
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
   box.original_area = support.recovered ? support.recovered->original_area
                                        : support.carved ? static_cast<double>(support.area) : segment.area.value_or(static_cast<double>(support.area));
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
 std::unordered_set<PhysicalKey, PhysicalKeyHash> offered_;
 std::map<CoconutImageNamespace, CoconutComponent> components_;
 std::unordered_map<std::uint32_t, std::size_t> segment_by_id_;
 std::vector<CoconutSegmentSupport> support_;
 std::uint64_t rows_ = 0;
};
std::uint32_t dimension(const Json& image, std::string_view field, const CoconutImportLimits& limits) {
 auto found = image.find(std::string(field));
 if (found == image.end() || found->is_null()) return 0;
 auto value = unsigned_field(image, field);
 if (value == 0 || value > limits.max_dimension) invalid("image dimension exceeds admission");
 return static_cast<std::uint32_t>(value);
}
// Parse one selected envelope row at a time. Returning false at object_end
// immediately discards that row from the parser's array. Two sequential passes
// permit either top-level field order without a release-sized JSON DOM.
void json_rows(const CoconutImportRequest& request, std::span<const std::string_view> fields,
               const std::function<void(std::string_view, const Json&)>& consume) {
 std::ifstream input(request.annotation_json);
 if (!input) invalid("cannot open annotation JSON: " + request.annotation_json.string());
 std::size_t selected = fields.size();
 std::vector<bool> found(fields.size(), false);
 std::size_t record_events = 0;
 const auto parsed = Json::parse(input, [&](int depth, Json::parse_event_t event, Json& value) {
  if (depth > 16) invalid("annotation JSON nesting exceeds admission");
  if (depth == 0 && event == Json::parse_event_t::array_start) invalid("panoptic JSON requires an object envelope");
  if (depth == 1 && event == Json::parse_event_t::key) {
   const auto& name = value.get_ref<const std::string&>();
   const auto match = std::ranges::find(fields, name);
   selected = static_cast<std::size_t>(match - fields.begin());
   if (selected < fields.size()) {
    if (found[selected]) invalid("duplicate panoptic envelope field");
    found[selected] = true;
   }
   return selected < fields.size();
  }
  if (depth == 2 && event == Json::parse_event_t::object_start) throw_if_benchmark_cancelled(request.cancellation);
  if (selected == fields.size()) return true;
  if (depth == 1 && (event == Json::parse_event_t::object_start || event == Json::parse_event_t::value)) invalid("panoptic envelope field must be an array");
  if (depth == 2 && (event == Json::parse_event_t::array_start || event == Json::parse_event_t::value)) invalid("panoptic row must be an object");
  if (depth == 2 && event == Json::parse_event_t::object_start) {
   throw_if_benchmark_cancelled(request.cancellation);
   record_events = 0;
  }
  if (depth >= 2 && ++record_events > static_cast<std::size_t>(request.limits.max_segments) * 32U + 128U) invalid("panoptic row exceeds segment admission");
  if (event == Json::parse_event_t::value && value.is_string() && value.get_ref<const std::string&>().size() > 4096U)
   invalid("panoptic text exceeds admission");
  if (depth == 2 && event == Json::parse_event_t::object_end) {
   consume(fields[selected], value);
   return false;
  }
  return true;
 });
 for (std::size_t i = 0; i < fields.size(); ++i) {
  if (!parsed.is_object() || !found[i]) invalid("missing panoptic envelope field " + std::string(fields[i]));
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
 const auto physical_stem = [](const Json& row) {
  std::string stem;
  for (const auto field : {"object365_file_name", "object365_name", "file_name"}) {
   const auto found = row.find(field);
   if (found == row.end() || found->is_null()) continue;
   const auto& name = found->get_ref<const std::string&>();
   if (name.empty() || (std::string_view(field) == "file_name" && !std::filesystem::path(name).filename().string().starts_with("objects365_"))) continue;
   const auto parsed = objects_name(name);
   if (!stem.empty() && stem != parsed.stem) invalid("contradictory declared Objects365 members");
   stem = parsed.stem;
  }
  return stem;
 };
 constexpr std::array<std::string_view, 2> preparation_fields{"categories", "images"};
 json_rows(request, preparation_fields, [&](std::string_view field, const Json& row) {
  if (field == "categories") {
   std::optional<std::uint32_t> id;
   std::optional<std::string> name;
   if (row.contains("id")) id = mmltk::frameworks::serialization::decode_json_integer_exact<std::uint32_t>(row.at("id"));
   if (row.contains("name")) name = row.at("name").get<std::string>();
   category_admission.observe(id, name ? std::optional<std::string_view>(*name) : std::nullopt);
   return;
  }
  ImageRow image;
  if (row.contains("id")) {
   image.id = unsigned_field(row, "id");
   if (!by_id.emplace(*image.id, images.size()).second) invalid("duplicate JSON image ID");
  }
  if (row.contains("file_name")) {
   image.file_name = row.at("file_name").get<std::string>();
   if (!by_file.emplace(image.file_name, images.size()).second) invalid("duplicate JSON image filename");
  }
  image.physical_stem = physical_stem(row);
  if (!image.physical_stem.empty() && !by_physical_stem.emplace(image.physical_stem, images.size()).second)
   invalid("duplicate JSON physical image: " + image.physical_stem);
  image.width = dimension(row, "width", request.limits);
  image.height = dimension(row, "height", request.limits);
  images.push_back(std::move(image));
 });
 category_admission.complete();
 std::vector<CoconutRecord> result;
 std::vector<bool> joined_images(images.size(), false);
 std::uint64_t segment_ordinal = 0;
 constexpr std::array<std::string_view, 1> annotation_fields{"annotations"};
 json_rows(request, annotation_fields, [&](std::string_view, const Json& annotation) {
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
  if (annotation.contains("image_id")) {
   record.image_id = unsigned_field(annotation, "image_id");
   join(by_id, record.image_id);
  }
  if (annotation.contains("file_name")) {
   record.file_name = annotation.at("file_name").get<std::string>();
   join(by_file, record.file_name);
  }
  record.physical_stem = physical_stem(annotation);
  if (!record.physical_stem.empty()) join(by_physical_stem, record.physical_stem);
  if (!image_index && request.edition == CoconutEdition::ObjectsValidation) invalid("validation annotation has no image-record join: " + record.file_name);
  if (image_index) {
   const auto& image = images[*image_index];
   if (joined_images[*image_index]) invalid("multiple annotations join one image row: " + record.file_name);
   joined_images[*image_index] = true;
   if (image.id && annotation.contains("image_id") && *image.id != record.image_id) invalid("annotation/image ID disagreement");
   if (!image.physical_stem.empty()) {
    if (!record.physical_stem.empty() && record.physical_stem != image.physical_stem) invalid("contradictory declared Objects365 members");
    record.physical_stem = image.physical_stem;
   }
   record.width = image.width;
   record.height = image.height;
  }
  if (record.physical_stem.empty()) invalid("unresolved offered JSON annotation: " + record.file_name);
  if (!annotation.contains("image_id")) {
   const auto declared_id = image_index ? images[*image_index].id : std::nullopt;
   record.image_id = declared_id ? *declared_id : objects_name(record.physical_stem).id;
  }
  segments_from_json(annotation.at("segments_info"), record, request.limits);
  if (record.segments.size() > UINT64_MAX - segment_ordinal) invalid("segment ordinal overflow");
  segment_ordinal += record.segments.size();
  result.push_back(std::move(record));
 });
 for (std::size_t i = 0; i < images.size(); ++i) {
  throw_if_benchmark_cancelled(request.cancellation);
  if (!joined_images[i]) invalid("offered JSON image has no annotation/mask join: " + images[i].file_name);
 }
 return result;
}
std::vector<CoconutRecord> xlarge_records(const CoconutImportRequest& request) {
 Archive archive(request.mask_archive);
 std::map<std::string, CoconutRecord> records;
 constexpr std::string_view prefix = "coconuts_xlarge/panseg_info/";
 while (archive.next(request.cancellation)) {
  if (!archive.regular() || !archive.member().starts_with(prefix) || !archive.member().ends_with(".json")) continue;
  const auto name = objects_name(archive.member());
  if (name.source != CoconutImageNamespace::Objects365V2 || archive.member() != std::string(prefix) + name.stem + ".json")
   invalid("unsupported XL info member: " + archive.member());
  CoconutRecord record;
  record.image_id = name.id;
  record.physical_stem = name.stem;
  const auto bytes = archive.read(16U * 1024U * 1024U, request.cancellation);
  segments_from_json(Json::parse(bytes.begin(), bytes.end()), record, request.limits);
  if (!records.emplace(name.stem, std::move(record)).second) invalid("duplicate XL info member: " + archive.member());
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
void consume_archive(const CoconutImportRequest& request, std::vector<CoconutRecord>& records, Importer& importer) {
 const std::string prefix = request.edition == CoconutEdition::XLarge  ? "coconuts_xlarge/panseg/"
                            : request.edition == CoconutEdition::Large ? "panoptic_object365/"
                                                                       : "panoptic_o365val_v3/";
 std::unordered_map<std::string, std::size_t> wanted;
 for (std::size_t i = 0; i < records.size(); ++i) {
  throw_if_benchmark_cancelled(request.cancellation);
  if (!wanted.emplace(prefix + records[i].physical_stem + ".png", i).second) invalid("duplicate offered mask: " + records[i].physical_stem);
 }
 std::vector<bool> consumed(records.size(), false);
 Archive archive(request.mask_archive);
 while (archive.next(request.cancellation)) {
  if (!archive.regular()) continue;
  const auto found = wanted.find(archive.member());
  if (found == wanted.end()) {
   if (archive.member().starts_with(prefix) && archive.member().ends_with(".png")) invalid("extra mask without annotation: " + archive.member());
   continue;
  }
  if (consumed[found->second]) invalid("duplicate archive mask: " + archive.member());
  const auto png = archive.read(request.limits.max_png_bytes, request.cancellation);
  importer.consume(records[found->second], png);
  consumed[found->second] = true;
  std::vector<CoconutSegment>().swap(records[found->second].segments);
 }
 for (std::size_t i = 0; i < consumed.size(); ++i) {
  throw_if_benchmark_cancelled(request.cancellation);
  if (!consumed[i]) invalid("missing offered mask: " + prefix + records[i].physical_stem + ".png");
 }
}
void validate_component(const CoconutComponent& component, Cancellation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 (void)coconut_namespace_name(component.source);
 (void)coconut_release_component(component.edition);
 if (component.input_identity.empty() || component.index.images.size() != component.inventory.size() ||
     component.index.source != index_source(component.source) || component.index.split != component_split(component.edition, component.source))
  invalid("component index/inventory admission mismatch");
 (void)mmltk::common::io::parse_sha256_hex(component.index.annotation_sha256);
 if (component.recovery_policy) {
  if (component.recovery_policy != kCoconutRecoveryPolicy ||
      (component.source != CoconutImageNamespace::CocoTrain && component.source != CoconutImageNamespace::CocoValidation) ||
      component.recovery.size() != component.inventory.size()) invalid("invalid recovery source or image count");
  (void)mmltk::common::io::parse_sha256_hex(component.original_annotation_identity);
  for (std::size_t i = 0; i < component.recovery.size(); ++i) {
   throw_if_benchmark_cancelled(cancellation);
   const auto& fact = component.recovery[i];
   const auto& image = component.index.images[i];
   if (fact.image_id != image.source_image_id || fact.unresolved > 65535U || fact.unresolved != fact.omissions.size() ||
       fact.objects.size() > image.box_count || fact.objects.size() > 65535U - fact.omissions.size())
    invalid("invalid image recovery facts");
   if (fact.objects.empty() && fact.omissions.empty()) continue;
   std::unordered_set<std::uint64_t> originals, annotations;
   if (image.first_box > component.index.boxes.size() || image.box_count > component.index.boxes.size() - image.first_box)
    invalid("invalid recovery image box extent");
   const auto boxes = std::span(component.index.boxes).subspan(static_cast<std::size_t>(image.first_box), image.box_count);
   std::unordered_map<std::uint64_t, const NormalizedBox*> by_annotation;
   by_annotation.reserve(boxes.size());
   for (const auto& box : boxes) by_annotation.emplace(box.annotation_id, &box);
   for (const auto& object : fact.omissions) {
    if (object.original_annotation_id != 0 || object.source_category_id == 0 ||
        !annotations.insert(object.annotation_id).second ||
        by_annotation.contains(object.annotation_id))
     invalid("invalid omitted object identity");
   }
   for (const auto& object : fact.objects) {
    const auto found = by_annotation.find(object.annotation_id);
    if (!originals.insert(object.original_annotation_id).second || !annotations.insert(object.annotation_id).second ||
        found == by_annotation.end() || found->second->source_ordinal != object.source_ordinal ||
        found->second->source_category_id != object.source_category_id) invalid("invalid recovered object identity");
   }
  }
 } else if (!component.original_annotation_identity.empty() || !component.recovery.empty()) invalid("unexpected recovery facts");
 std::unordered_set<std::uint64_t> ordinals;
 for (std::size_t i = 0; i < component.inventory.size(); ++i) {
  throw_if_benchmark_cancelled(cancellation);
  const auto& image = component.inventory[i];
  validate_physical(image.physical);
  if (image.physical.source != component.source || image.physical.image_id != component.index.images[i].source_image_id ||
      image.physical.shard != component.index.images[i].source_shard || !ordinals.insert(image.source_ordinal).second ||
      (i && image.physical.image_id <= component.inventory[i - 1].physical.image_id))
   invalid("invalid component image inventory");
 }
}
}  // namespace
std::string coconut_component_input_identity(std::string_view base, CoconutImageNamespace source, const CoconutMaskRecovery* recovery) {
 const auto original = recovery ? recovery->original_identity(source) : std::string_view{};
 if (original.empty()) return std::string(base);
 const auto material = std::string(base) + "\nrecovery:" + std::to_string(kCoconutRecoveryPolicy) + "\n" +
                       std::string(coconut_namespace_name(source)) + "\n" + std::string(original);
 return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(
  std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size())));
}
std::string canonical_coconut_archive_member(std::string_view raw) {
 while (raw.starts_with("./")) raw.remove_prefix(2);
 if (raw.empty() || raw.front() == '/' || raw.find('\\') != std::string_view::npos || raw.find('\0') != std::string_view::npos)
  invalid("unsafe archive member: " + std::string(raw));
 std::filesystem::path path(raw);
 for (const auto& part : path)
  if (part == "..") invalid("traversing archive member: " + std::string(raw));
 return path.lexically_normal().generic_string();
}
CoconutPhysicalMembership::CoconutPhysicalMembership(std::span<const CoconutPhysicalImage> images, Cancellation cancellation) {
 std::unordered_map<CoconutImageNamespace, std::size_t> counts;
 for (const auto& image : images) {
  throw_if_benchmark_cancelled(cancellation);
  ++counts[image.source];
 }
 for (const auto& [source, count] : counts) {
  (void)coconut_namespace_name(source);
  namespaces_[source].reserve(count);
 }
 for (const auto& image : images) {
  throw_if_benchmark_cancelled(cancellation);
  validate_physical(image);
  if (!namespaces_.at(image.source).emplace(image.image_id, &image).second) invalid("duplicate physical member: " + image.member);
 }
}
const CoconutPhysicalImage* CoconutPhysicalMembership::find(CoconutImageNamespace source, std::uint64_t id) const noexcept {
 const auto space = namespaces_.find(source);
 if (space == namespaces_.end()) return nullptr;
 const auto found = space->second.find(id);
 return found == space->second.end() ? nullptr : found->second;
}
std::vector<CoconutComponent> import_coconut_annotations(const CoconutImportRequest& request) {
 (void)coconut_release_component(request.edition);
 Importer importer(request);
 if (request.edition == CoconutEdition::Base || request.edition == CoconutEdition::RelabeledValidation) {
  if (request.parquet_shards.empty()) invalid("missing Parquet shards");
  read_coconut_parquet(request.parquet_shards, request.limits, request.cancellation,
                       [&](const CoconutRecord& record, std::span<const std::uint8_t> png) { importer.consume(record, png); });
 } else {
  auto records = request.edition == CoconutEdition::XLarge ? xlarge_records(request) : json_records(request);
  consume_archive(request, records, importer);
 }
 return importer.finish();
}
std::uint64_t reconcile_coconut_extensions(std::vector<CoconutComponent>& components, Cancellation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 std::unordered_set<PhysicalKey, PhysicalKeyHash> large, xlarge;
 for (const auto& component : components)
  if (component.edition == CoconutEdition::Large) {
   for (const auto& image : component.inventory) {
    throw_if_benchmark_cancelled(cancellation);
    if (!large.insert(physical_key(image.physical.source, image.physical.image_id)).second) invalid("duplicate Large physical image");
   }
  }
 std::uint64_t removed = 0;
 for (auto& component : components)
  if (component.edition == CoconutEdition::XLarge) {
   std::vector<std::size_t> retained;
   for (std::size_t i = 0; i < component.inventory.size(); ++i) {
    throw_if_benchmark_cancelled(cancellation);
    const auto& image = component.inventory[i].physical;
    const auto key = physical_key(image.source, image.image_id);
    if (!xlarge.insert(key).second) invalid("duplicate XL physical image");
    if (large.contains(key))
     ++removed;
    else
     retained.push_back(i);
   }
   if (retained.size() != component.inventory.size()) retain_images(component, retained, cancellation);
  }
 throw_if_benchmark_cancelled(cancellation);
 return removed;
}
std::vector<CoconutPhysicalImage> coconut_image_archive_inventory(const std::filesystem::path& archive_path, const std::filesystem::path& cache_path,
                                                                  CoconutImageNamespace source, std::uint16_t shard, std::string archive_identity,
                                                                  Cancellation cancellation) {
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
    validate_physical(image);
    if (image.source != source || image.shard != shard || image.archive_identity != archive_identity ||
        (!result.empty() && image.image_id <= result.back().image_id))
     invalid("invalid cached archive inventory");
    result.push_back(std::move(image));
   }
   (void)input.finish();
   throw_if_benchmark_cancelled(cancellation);
   return result;
  } catch (const std::exception&) { throw_if_benchmark_cancelled(cancellation); }
 }
 Archive archive(archive_path);
 std::vector<CoconutPhysicalImage> result;
 while (archive.next(cancellation)) {
  if (!archive.regular() || !archive.member().ends_with(".jpg")) continue;
  CoconutPhysicalImage image{source, 0, shard, archive.member(), archive_identity};
  if (source == CoconutImageNamespace::Objects365V1 || source == CoconutImageNamespace::Objects365V2) {
   const auto name = objects_name(image.member);
   if (name.source != source) invalid("physical archive namespace mismatch: " + image.member);
   image.image_id = name.id;
  } else
   image.image_id = coco_name(image.member);
  validate_physical(image);
  result.push_back(std::move(image));
 }
 throw_if_benchmark_cancelled(cancellation);
 std::ranges::sort(result, {}, &CoconutPhysicalImage::image_id);
 throw_if_benchmark_cancelled(cancellation);
 for (std::size_t i = 0; i < result.size(); ++i) {
  throw_if_benchmark_cancelled(cancellation);
  if (i && result[i].image_id == result[i - 1].image_id) invalid("duplicate physical archive image ID");
 }
 expected.count = result.size();
 if (!cache_path.empty()) (void)store_inventory(cache_path, expected, std::span<const CoconutPhysicalImage>(result), {}, cancellation);
 throw_if_benchmark_cancelled(cancellation);
 return result;
}
void store_coconut_component(const std::filesystem::path& index_path, const CoconutComponent& component, Cancellation cancellation) {
 validate_component(component, cancellation);
 const auto identity = store_inventory(index_path.string() + ".inventory", component_header(component), std::span(component.inventory),
                                       component.index.annotation_sha256, cancellation, &component);
 throw_if_benchmark_cancelled(cancellation);
 store_normalized_annotation_index(index_path, component.index, cancellation);
 // The existing index completion jointly admits the binary inventory. A base
 // completion alone cannot be admitted if publication is interrupted here.
 const auto manifest_path = std::filesystem::path(index_path.string() + ".complete.json");
 auto manifest = read_json_file(manifest_path);
 manifest["coconut"] = {{"edition", component.edition},
                        {"source", component.source},
                        {"input_identity", component.input_identity},
                        {"normalization", kCoconutNormalizationRevision},
                        {"inventory_identity", identity},
                        {"inventory_count", component.inventory.size()},
                        {"recovery_policy", component.recovery_policy},
                        {"original_annotation_identity", component.original_annotation_identity},
                        {"recovery_images", component.recovery.size()}};
 throw_if_benchmark_cancelled(cancellation);
 write_json_atomically(manifest_path, manifest, cancellation);
}
std::optional<CoconutComponent> load_coconut_component(const std::filesystem::path& index_path, CoconutEdition edition, CoconutImageNamespace source,
                                                       std::string_view input_identity, Cancellation cancellation) {
 throw_if_benchmark_cancelled(cancellation);
 try {
  const auto manifest = read_json_file(index_path.string() + ".complete.json");
  const auto& facts = manifest.at("coconut");
  if (facts.at("edition") != edition || facts.at("source") != source || facts.at("input_identity") != input_identity ||
      facts.at("normalization") != kCoconutNormalizationRevision)
   return std::nullopt;
  CoconutComponent component;
  component.edition = edition;
  component.source = source;
  component.input_identity = input_identity;
  InventoryInput input(index_path.string() + ".inventory", cancellation);
  InventoryHeader header;
  input.value(header);
  validate_inventory_header(header, component_header(component));
  if (header.count > input.maximum_records() || facts.at("inventory_count") != header.count) invalid("invalid component inventory count");
  throw_if_benchmark_cancelled(cancellation);
  component.inventory.reserve(mmltk::common::math::checked_cast<std::size_t>(header.count, "component inventory count overflow"));
  for (std::uint64_t i = 0; i < header.count; ++i) {
   throw_if_benchmark_cancelled(cancellation);
   CoconutInventoryImage image;
   input.value(image);
   component.inventory.push_back(std::move(image));
  }
  component.recovery_policy = facts.value("recovery_policy", 0U);
  if (component.recovery_policy) {
   std::uint32_t policy = 0;
   input.value(policy);
   if (policy != component.recovery_policy || policy != kCoconutRecoveryPolicy) invalid("invalid recovery policy");
   input.value(component.original_annotation_identity);
   std::uint64_t count = 0;
   input.value(count);
   if (count != header.count || facts.at("recovery_images") != count ||
       facts.at("original_annotation_identity") != component.original_annotation_identity) invalid("invalid recovery completion");
   component.recovery.resize(static_cast<std::size_t>(count));
   for (auto& image : component.recovery) { throw_if_benchmark_cancelled(cancellation); input.value(image); }
  }
  const auto identity = input.finish();
  if (facts.at("inventory_identity") != identity) invalid("component inventory completion mismatch");
  auto index = load_normalized_annotation_index(index_path, index_source(source), component_split(edition, source), identity, cancellation);
  if (!index) return std::nullopt;
  component.index = std::move(*index);
  validate_component(component, cancellation);
  throw_if_benchmark_cancelled(cancellation);
  return component;
 } catch (const std::exception&) {
  throw_if_benchmark_cancelled(cancellation);
  return std::nullopt;
 }
}
}  // namespace mmltk::backend::data::benchmark_internal
