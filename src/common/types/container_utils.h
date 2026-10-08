#pragma once
#include <algorithm>
#include <vector>
namespace mmltk::common {
template <class Value, class Allocator>
void sort_unique(std::vector<Value, Allocator>& values) {
 std::ranges::sort(values);
 values.erase(std::unique(values.begin(), values.end()), values.end());
}
}  // namespace mmltk::common
