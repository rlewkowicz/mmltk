#include "src/common/system/time_utils.h"
#include <chrono>
namespace mmltk::common::system {
std::uint64_t steady_clock_now_ns() noexcept {
 return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace mmltk::common::system
