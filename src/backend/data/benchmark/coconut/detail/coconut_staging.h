#pragma once
#include "src/backend/data/detail/compile_resources.h"
#include "src/backend/data/detail/storage_reservation.h"
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <utility>
namespace mmltk::backend::data::detail { class FileWorkQueue; }
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
 // A failed fill leaves an unpublished hole. Claim and preparation never borrow
 // the supplied payload; its owning job retains that storage until filling ends.
 class Request final {
 public:
  Request() = default;
  Request(std::size_t bytes, std::size_t alignment);
  [[nodiscard]] bool ready() const noexcept { return !bytes_ || bool(owner_); }
  [[nodiscard]] std::span<std::byte> target() const;
  [[nodiscard]] Bytes bytes() const { return {owner_, target()}; }
 private:
  friend class CoconutStaging;
  std::size_t bytes_ = 0, alignment_ = 1;
  std::shared_ptr<const void> owner_;
  std::span<std::byte> target_;
 };
 explicit CoconutStaging(const std::filesystem::path& = {}, detail::StorageReservationPool* = nullptr, std::size_t block_bytes = 4U << 20);
 void retire() noexcept;
 // Only the short aligned claim holds the append mutex. Physical preparation
 // belongs to a FileWorkQueue operation, retaining its admitted descriptor.
 [[nodiscard]] bool try_claim(Request&);
 void prepare(Request&, const detail::CompileAllowance& descriptor);
 // Controller/standalone entry point. Execution-backed callers supply their
 // existing file service; no CPU lane may wait for preparation or admission.
 void prepare(Request&, detail::CompileExecution* = nullptr, detail::FileWorkQueue* = nullptr, const detail::CompileAllowance& parent = {});
 template<class T> [[nodiscard]] Range<T> copy(std::span<const T> values, detail::CompileExecution* execution = nullptr,
  detail::FileWorkQueue* files = nullptr, const detail::CompileAllowance& parent = {}) {
  static_assert(std::is_trivially_copyable_v<T>);
  Request request(values.size_bytes(), alignof(T));
  prepare(request, execution, files, parent);
  const auto target = request.target();
  if (!target.empty()) std::memcpy(target.data(), values.data(), target.size());
  auto range = request.bytes();
  return {std::move(range.owner), {reinterpret_cast<const T*>(range.values.data()), values.size()}};
 }
private:
 std::filesystem::path directory_;
 detail::StorageReservationPool storage_;
 std::size_t block_bytes_;
 std::mutex mutex_, preparation_mutex_;
 std::shared_ptr<Block> active_;
};
}
