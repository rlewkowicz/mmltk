#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
namespace mmltk::backend::ml::runtime {
// Copies retain the exact count allocation. The execution owner must settle
// physical GPU work before resetting or replacing its final retained copy.
class AnalysisValueCount final {
   public:
    AnalysisValueCount() = default;
    // Rvalue transfers also copy: every surviving view keeps its custody.
    AnalysisValueCount(const AnalysisValueCount&) = default;
    AnalysisValueCount& operator=(const AnalysisValueCount&) = default;
    void Reset() noexcept;
    void SetKnown(std::size_t value, std::size_t capacity);
    void PublishPending(const std::int64_t* device, const std::int64_t* host, std::shared_ptr<void> custody, std::size_t capacity);
    // Call only after the producer's existing physical completion boundary.
    void SettleAfterCompletion(std::size_t capacity);
    [[nodiscard]] std::size_t value() const noexcept { return known_.value_or(0U); }
    [[nodiscard]] const std::int64_t* device_view() const noexcept { return device_; }
    [[nodiscard]] bool pending() const noexcept { return !known_.has_value(); }
    [[nodiscard]] bool empty() const noexcept { return known_ == 0U && device_ == nullptr; }
    [[nodiscard]] bool valid(std::size_t capacity) const noexcept;

   private:
    std::optional<std::size_t> known_{0U};
    const std::int64_t* device_ = nullptr;
    const std::int64_t* host_ = nullptr;
    std::shared_ptr<void> custody_;
};
}  // namespace mmltk::backend::ml::runtime
