#include "src/pch_std.h"
#include "src/backend/data/compiler/detail/dataset_compiler_internal.h"
#include "src/backend/data/compiler/dataset_compiler.h"
namespace mmltk::backend::data::compiler_internal {
ProgressBatch::ProgressBatch(ProgressCounter* const counter) noexcept : counter_(counter) {
 if (counter_ != nullptr) counter_->begin_worker();
}
ProgressBatch::~ProgressBatch() {
 if (counter_ != nullptr) counter_->end_worker();
}
void ProgressBatch::increment() noexcept {
 if (counter_ != nullptr) counter_->add_completed(1);
}
}  // namespace mmltk::backend::data::compiler_internal
