#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "detail/dataset_compiler_internal.h"
#include "src/backend/data/dataset_compiler.h"

namespace mmltk::backend::data::compiler_internal {

ProgressBatch::ProgressBatch(ProgressCounter* const counter) noexcept : counter_(counter) {
    if (counter_ != nullptr) counter_->begin_worker();
}

ProgressBatch::~ProgressBatch() {
    flush();
    if (counter_ != nullptr) counter_->end_worker();
}

void ProgressBatch::increment() noexcept {
    ++pending_;
    if (pending_ == kPublishBatch) flush();
}

void ProgressBatch::flush() noexcept {
    if (counter_ != nullptr && pending_ != 0U) counter_->add_completed(pending_);
    pending_ = 0U;
}

}  // namespace mmltk::backend::data::compiler_internal
