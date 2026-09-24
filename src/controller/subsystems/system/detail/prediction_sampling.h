#pragma once
#include <cstdint>
#include <cstddef>
#include <optional>
#include <random>
#include <unordered_set>
namespace mmltk::controller::detail {
// Known populations use Floyd's uniform subset algorithm and retain whichever
// of the selected/excluded sets is smaller. Selection never limits inference.
class PredictionSelection final {
public:
 PredictionSelection(std::uint64_t population, std::uint64_t count, std::mt19937_64&);
 [[nodiscard]] bool Contains(std::uint64_t index) const;
 [[nodiscard]] std::size_t retained_indices() const noexcept { return indices_.size(); }
 [[nodiscard]] static std::uint64_t Percent(std::uint64_t population, unsigned percent);
 [[nodiscard]] static std::uint64_t Eligible(std::uint64_t population, std::uint64_t limit, std::uint64_t count);

private:
 std::uint64_t population_;
 bool excluded_;
 std::unordered_set<std::uint64_t> indices_;
};
// The caller owns disk-backed slots, and settles a replacement before requesting
// the next decision. Neither frame pixels nor a duration-sized index is retained.
class PredictionReservoir final {
public:
 explicit PredictionReservoir(std::uint64_t capacity);
 [[nodiscard]] std::optional<std::uint64_t> Observe(std::mt19937_64&);
 [[nodiscard]] std::uint64_t observed() const noexcept { return observed_; }
 [[nodiscard]] std::uint64_t selected() const noexcept;

private:
 std::uint64_t capacity_, observed_ = 0;
};
}  // namespace mmltk::controller::detail
