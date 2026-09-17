#pragma once
#include <cstddef>
namespace native_gallery_allocations {
extern thread_local bool enabled;
extern thread_local std::size_t count;
struct Scope final {
    Scope();
    ~Scope();
};
}  // namespace native_gallery_allocations
