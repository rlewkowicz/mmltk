#include "src/backend/ml/runtime/analysis_value_count.h"
#include <stdexcept>
#include <utility>

namespace mmltk::backend::ml::runtime {
void AnalysisValueCount::Reset() noexcept {
    known_ = 0U;
    device_ = nullptr;
    host_ = nullptr;
    custody_.reset();
}
void AnalysisValueCount::SetKnown(std::size_t value, std::size_t capacity) {
    if (value > capacity) throw std::invalid_argument("analysis count exceeds output capacity");
    Reset();
    known_ = value;
}
void AnalysisValueCount::PublishPending(const std::int64_t* device, const std::int64_t* host,
                                      std::shared_ptr<void> custody, std::size_t capacity) {
    if (!device || !host || !custody || capacity == 0U)
        throw std::invalid_argument("analysis pending count requires paired views, custody and capacity");
    known_.reset();
    device_ = device;
    host_ = host;
    custody_ = std::move(custody);
}
void AnalysisValueCount::SettleAfterCompletion(std::size_t capacity) {
    if (pending()) {
        const auto count = *host_;
        if (count < 0 || static_cast<std::uint64_t>(count) > capacity)
            throw std::runtime_error("analysis survivor count exceeds output capacity");
        known_ = static_cast<std::size_t>(count);
    } else if (value() > capacity) {
        throw std::runtime_error("analysis count exceeds output capacity");
    }
}
bool AnalysisValueCount::valid(std::size_t capacity) const noexcept {
    return value() <= capacity && (!device_ || capacity != 0U);
}
}  // namespace mmltk::backend::ml::runtime
