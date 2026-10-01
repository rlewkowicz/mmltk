#pragma once
#include "src/backend/data/detail/storage_reservation.h"
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
namespace mmltk::backend::data::benchmark_internal {
// One generation appends into fixed allocated blocks. Published ranges never
// move or change; their block survives independently of this append owner.
class CoconutStaging final {
 struct Block;
public:
 struct Bytes {
  std::shared_ptr<const void> owner;
  std::span<const std::byte> values;
 };
 template<class T> struct Range {
  std::shared_ptr<const void> owner;
  std::span<const T> values;
 };
 explicit CoconutStaging(const std::filesystem::path& = {}, detail::StorageReservationPool* = nullptr, std::size_t block_bytes = 4U << 20);
 void retire() noexcept;
 [[nodiscard]] Bytes append(std::size_t bytes, std::size_t alignment, const std::function<void(std::span<std::byte>)>&);
 template<class T> [[nodiscard]] Range<T> copy(std::span<const T> values) {
  static_assert(std::is_trivially_copyable_v<T>);
  auto range = append(values.size_bytes(), alignof(T), [&](auto target) { if (!target.empty()) std::memcpy(target.data(), values.data(), target.size()); });
  return {std::move(range.owner), {reinterpret_cast<const T*>(range.values.data()), values.size()}};
 }
private:
 std::filesystem::path directory_;
 detail::StorageReservationPool storage_;
 std::size_t block_bytes_;
 std::mutex mutex_;
 std::shared_ptr<Block> active_;
};
}
