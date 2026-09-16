#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
namespace mmltk::backend::media::live {
class LiveStateSignal {
   public:
    using Listener = std::function<void()>;
    void set_listener(Listener listener);
    void notify() const noexcept;

   private:
    std::atomic<std::shared_ptr<const Listener>> listener_{};
};
}  // namespace mmltk::backend::media::live
