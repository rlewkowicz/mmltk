#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>
namespace mmltk::backend::data::detail {
class InsufficientStorage final : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
// Physical allocation promises are shared by filesystem, independently of recipes.
// A reservation has one mutation owner. Each filesystem serializes capacity
// observation and accounting; unrelated filesystems share only registration.
// Physical mutations retain their promises until allocation reconciliation.
// Diagnostic callbacks observe completed transactions outside ledger locks.
class StorageReservationPool final {
 struct Ledger;
 struct Destination;

public:
 struct Observation {
  const std::filesystem::path& path;
  std::string_view description;
  std::uint64_t required, outstanding, available;
  std::uint64_t capacity_samples = 0, retries = 0, duration_ns = 0;
  bool failed = false;
 };
 using Observer = std::function<void(const Observation&)>;
 class Reservation final {
 public:
  Reservation() = default;
  Reservation(const Reservation&) = delete;
  Reservation& operator=(const Reservation&) = delete;
  Reservation(Reservation&&) noexcept;
  Reservation& operator=(Reservation&&) noexcept;
  ~Reservation();
  void reconcile(int descriptor);
  void reconcile_allocated(std::uint64_t);
  void withdraw_allocation();
  void resize(std::uint64_t promised, std::string_view description);
  void grow(std::uint64_t promised, std::string_view description);
  // Commit freshly observed backing and an additional physical growth promise
  // in one capacity transaction. Rejection preserves both previous values.
  void reserve_growth(std::uint64_t allocated, std::uint64_t additional, std::string_view description);
  void release() noexcept;

 private:
  friend class StorageReservationPool;
  Reservation(std::shared_ptr<Destination>, std::uint64_t promised, std::uint64_t allocated);
  [[nodiscard]] std::uint64_t outstanding() const noexcept;
  void replace(std::uint64_t promised, std::uint64_t allocated, std::string_view description, bool sample);
  std::shared_ptr<Destination> destination_;
  std::uint64_t promised_ = 0, allocated_ = 0;
 };
 StorageReservationPool(std::filesystem::path, Observer = {}, StorageReservationPool* shared = nullptr);
 static void require_capacity(const std::filesystem::path&, std::uint64_t required, std::string_view description, Observer = {});
 [[nodiscard]] Reservation reserve(std::uint64_t promised, std::string_view description, std::uint64_t allocated = 0);
 [[nodiscard]] std::uint64_t outstanding() const;
 [[nodiscard]] static std::uint64_t allocated_bytes(int descriptor);

private:
 std::shared_ptr<Destination> destination_;
};
}  // namespace mmltk::backend::data::detail
