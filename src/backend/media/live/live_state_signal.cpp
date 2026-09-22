#include "detail/live_state_signal.h"
namespace mmltk::backend::media::live {
void LiveStateSignal::set_listener(Listener listener) { listener_.store(listener ? std::make_shared<const Listener>(std::move(listener)) : nullptr, std::memory_order_release); }
void LiveStateSignal::notify() const noexcept {
 const auto listener = listener_.load(std::memory_order_acquire);
 if (listener == nullptr || !*listener) return;
 try {
  (*listener)();
 } catch (...) {}
}
}  // namespace mmltk::backend::media::live
