#include "allocation_tracking.h"
#include <cstdlib>
#include <new>
namespace native_gallery_allocations {
thread_local bool enabled = false;
thread_local std::size_t count = 0U;
Scope::Scope() {
 count = 0U;
 enabled = true;
}
Scope::~Scope() { enabled = false; }
[[nodiscard]] void* Allocate(std::size_t bytes, const std::size_t alignment) {
 if (enabled) ++count;
 if (bytes == 0U) bytes = 1U;
 void* result = nullptr;
 if (alignment <= alignof(std::max_align_t))
  result = std::malloc(bytes);
 else if (::posix_memalign(&result, alignment, bytes) != 0)
  result = nullptr;
 if (!result) throw std::bad_alloc{};
 return result;
}
}  // namespace native_gallery_allocations
[[gnu::noinline]] void* operator new(std::size_t bytes) { return native_gallery_allocations::Allocate(bytes, alignof(std::max_align_t)); }
[[gnu::noinline]] void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
[[gnu::noinline]] void* operator new(std::size_t bytes, std::align_val_t alignment) { return native_gallery_allocations::Allocate(bytes, static_cast<std::size_t>(alignment)); }
[[gnu::noinline]] void* operator new[](std::size_t bytes, std::align_val_t alignment) { return ::operator new(bytes, alignment); }
[[gnu::noinline]] void operator delete(void* value) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void* value) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete(void* value, std::size_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete(void* value, std::align_val_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void* value, std::align_val_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete(void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }
[[gnu::noinline]] void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }
