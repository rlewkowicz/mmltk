#pragma once  // backend.data private implementation boundary
#include "src/backend/data/detail/staged_artifact.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_input.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/common/types/cancellation_observation.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <type_traits>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
class CoconutInventorySeal final {
public:
 void capture(const mmltk::common::io::FileHandle& file);
 [[nodiscard]] std::span<const std::uint8_t> data() const { return {static_cast<const std::uint8_t*>(mapping.address()), bytes}; }
 // The component publisher holds mutex through inventory and completion publication.
 void publish(const std::filesystem::path&, mmltk::common::concurrency::CancellationObservation, detail::StorageReservationPool*);
 // Custody changes are serialized; semantic fields and mapped bytes never change.
 mutable std::mutex mutex;
 detail::StagedArtifact staged;
 mmltk::common::io::MappedByteRegion mapping;
 std::string identity;
 std::size_t bytes = 0;
 std::filesystem::path directory, published_path;
 dev_t device = 0;
 ino_t inode = 0;
};
template <class T>
concept InventoryRecord = std::same_as<T, CoconutPhysicalImage> || std::same_as<T, CoconutInventoryImage> || std::same_as<T, InventoryHeader> || std::same_as<T, CoconutRecoveryImage> ||
                          std::same_as<T, CoconutRecoveredObject>;
// A single reflected little-endian encoding is used by hashing, writing and reading.
// Strings carry checked uint32 lengths. Only a fixed buffer and the current row
// are transient; records remain in their ordinary typed inventory owner.
class InventoryOutput final {
public:
 InventoryOutput(detail::StagedArtifact* file, mmltk::common::concurrency::CancellationObservation cancellation) : file_(file), cancellation_(cancellation) {}
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
   if (item.size() > 4096) throw CoconutInputError("COCONut: inventory text exceeds admission");
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
 [[nodiscard]] std::uint64_t byte_size() const;
 std::string finish();

private:
 void append(std::span<const std::uint8_t> bytes);
 void flush();
 detail::StagedArtifact* file_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 mmltk::common::io::Sha256Hasher hash_;
 std::array<std::uint8_t, 65536> buffer_{};
 std::size_t used_ = 0, offset_ = 0;
};
class InventoryInput final {
public:
 InventoryInput(const std::filesystem::path& path, mmltk::common::concurrency::CancellationObservation cancellation);
 std::shared_ptr<CoconutInventorySeal> seal() const { return seal_; }
 template <class T>
 void value(T& item) {
  if constexpr (InventoryRecord<T>) {
   mmltk::frameworks::reflection::visit_materialized_members<T>([&]<class Declaration>(const auto&) { value(item.*Declaration::pointer); });
  } else if constexpr (std::same_as<T, std::vector<CoconutRecoveredObject>>) {
   std::uint32_t count = 0;
   value(count);
   if (count > 65535U || count > maximum_records()) throw CoconutInputError("COCONut: invalid recovery object count");
   item.resize(count);
   for (auto& object : item) value(object);
  } else if constexpr (std::is_enum_v<T>) {
   std::underlying_type_t<T> number{};
   value(number);
   item = static_cast<T>(number);
  } else if constexpr (std::same_as<T, std::string>) {
   std::uint32_t count = 0;
   value(count);
   if (count > 4096) throw CoconutInputError("COCONut: inventory text exceeds admission");
   item.resize(count);
   read({reinterpret_cast<std::uint8_t*>(item.data()), count});
  } else {
   static_assert(std::is_unsigned_v<T> || std::same_as<T, bool>);
   std::array<std::uint8_t, sizeof(T)> bytes{};
   read(bytes);
   std::uint64_t number = 0;
   for (std::size_t i = 0; i < bytes.size(); ++i) number |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
   if constexpr (std::same_as<T, bool>) {
    if (number > 1) throw CoconutInputError("COCONut: invalid inventory flag");
   }
   item = static_cast<T>(number);
  }
 }
 std::uint64_t maximum_records() const { return content_size_ / 16U; }
 std::uint64_t byte_size() const { return content_size_ + 32U; }
 std::string finish();

private:
 void read(std::span<std::uint8_t> output);
 std::shared_ptr<CoconutInventorySeal> seal_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 mmltk::common::io::Sha256Hasher hash_;
 std::size_t content_size_ = 0, loaded_ = 0, cursor_ = 0, available_ = 0;
};
void validate_inventory_header(const InventoryHeader&, const InventoryHeader&);
template <class Records, class Recovery = std::span<const CoconutRecoveryImage>>
std::string encode_inventory(InventoryOutput& output, const InventoryHeader& header, const Records& records, mmltk::common::concurrency::CancellationObservation cancellation, std::uint32_t policy = 0,
 std::string_view original = {}, const Recovery& recovery = {}) {
 output.value(header);
 for (const auto& record : records) {
  throw_if_benchmark_cancelled(cancellation);
  output.value(record);
 }
 if (policy) {
  output.value(policy);
  output.value(std::string(original));
  output.value(static_cast<std::uint64_t>(recovery.size()));
  for (const auto& image : recovery) {
   throw_if_benchmark_cancelled(cancellation);
   output.value(image);
  }
 }
 return output.finish();
}
void validate_coconut_physical_image(const CoconutPhysicalImage&);
// Full archive inventory, independent of annotations/foreground selection. Cache is identity-bound.
[[nodiscard]] std::vector<CoconutPhysicalImage> load_coconut_physical_inventory(
 const std::filesystem::path&, CoconutImageNamespace, std::uint16_t, std::string_view, mmltk::common::concurrency::CancellationObservation, const std::function<void(const CoconutPhysicalImage&)>&);
void store_coconut_physical_inventory(const std::filesystem::path&, std::span<const CoconutPhysicalImage>, CoconutImageNamespace, std::uint16_t, std::string_view,
 mmltk::common::concurrency::CancellationObservation, detail::StorageReservationPool*);
}  // namespace mmltk::backend::data::benchmark_internal
