#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/pch_std.h"
namespace mmltk::backend::data::benchmark_internal {
namespace {
[[noreturn]] void invalid(std::string_view detail) { throw CoconutInputError("COCONut: " + std::string(detail)); }
std::uint64_t decimal(std::string_view value) {
 std::uint64_t id = 0;
 const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
 if (value.empty() || error != std::errc{} || end != value.data() + value.size() || value.front() == '+' || value.front() == '-') invalid("invalid decimal image identity: " + std::string(value));
 return id;
}
}  // namespace
CoconutPhysicalName parse_coconut_objects_member(std::string_view name) {
 const std::filesystem::path path(canonical_benchmark_archive_member(name));
 const auto extension = path.extension().string();
 if (!extension.empty() && extension != ".png" && extension != ".jpg" && extension != ".json") invalid("unsupported Objects365 member: " + std::string(name));
 const auto stem = path.stem().string();
 constexpr std::string_view v1 = "objects365_v1_", v2 = "objects365_v2_";
 const auto source = stem.starts_with(v1) ? CoconutImageNamespace::Objects365V1 : CoconutImageNamespace::Objects365V2;
 const auto prefix = source == CoconutImageNamespace::Objects365V1 ? v1 : v2;
 if (!stem.starts_with(prefix) || stem.size() != prefix.size() + 8U) invalid("invalid full Objects365 namespace/member: " + std::string(name));
 return {source, decimal(std::string_view(stem).substr(prefix.size())), stem};
}
std::uint64_t parse_coconut_coco_member(std::string_view name) {
 const std::filesystem::path path(canonical_benchmark_archive_member(name));
 if ((path.extension() != ".jpg" && path.extension() != ".png") || path.stem().string().size() != 12U) invalid("invalid COCO filename: " + std::string(name));
 return decimal(path.stem().string());
}
void CoconutSegments::writable() {
 if (!backing_) return;
 values_.assign(mapped_.begin(), mapped_.end());
 mapped_ = {};
 backing_.reset();
}
void CoconutSegments::stage(const std::filesystem::path& directory, StorageReservationPool* storage) {
 if (backing_ || values_.empty() || directory.empty()) return;
 static_assert(std::is_trivially_copyable_v<CoconutSegment>);
 const auto bytes = mmltk::common::math::checked_multiply(values_.size(), sizeof(CoconutSegment), "COCONut segment staging overflow");
 std::filesystem::create_directories(directory);
 StorageReservationPool reservations(directory, {}, storage);
 auto owner = std::make_shared<BenchmarkMappedArtifact>(reservations, directory / ".native-segments", bytes, "COCONut segment metadata");
 owner->write(values_.data(), bytes, 0);
 const void* address = owner->map_readonly();
 mapped_ = {static_cast<const CoconutSegment*>(address), values_.size()};
 backing_ = std::move(owner);
 std::vector<CoconutSegment>().swap(values_);
}
}  // namespace mmltk::backend::data::benchmark_internal
