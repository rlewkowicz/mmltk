#pragma once
#include <ATen/autocast_mode.h>
namespace mmltk::backend::ml::cuda {
class TorchAutocastScope final {
public:
 TorchAutocastScope(const bool enabled, const at::ScalarType precision)
     : previous_enabled_(at::autocast::is_autocast_enabled(at::kCUDA)),
       previous_precision_(at::autocast::get_autocast_dtype(at::kCUDA)),
       previous_cache_enabled_(at::autocast::is_autocast_cache_enabled()) {
  at::autocast::set_autocast_enabled(at::kCUDA, enabled);
  if (enabled) at::autocast::set_autocast_dtype(at::kCUDA, precision);
  at::autocast::set_autocast_cache_enabled(enabled);
  at::autocast::increment_nesting();
 }
 ~TorchAutocastScope() {
  at::autocast::decrement_nesting();
  at::autocast::set_autocast_enabled(at::kCUDA, previous_enabled_);
  at::autocast::set_autocast_dtype(at::kCUDA, previous_precision_);
  at::autocast::set_autocast_cache_enabled(previous_cache_enabled_);
  if (!previous_enabled_) at::autocast::clear_cache();
 }
 TorchAutocastScope(const TorchAutocastScope&) = delete;
 TorchAutocastScope& operator=(const TorchAutocastScope&) = delete;

private:
 bool previous_enabled_;
 at::ScalarType previous_precision_;
 bool previous_cache_enabled_;
};
}  // namespace mmltk::backend::ml::cuda
