#pragma once
#include <cstddef>
#include <memory_resource>
#include <vector>
namespace mmltk::common::system {
struct MemoryPolicy final {
 int mode = 0;
 std::vector<unsigned long> mask;
};
[[nodiscard]] MemoryPolicy capture_memory_policy();
void restore_memory_policy(const MemoryPolicy&);
void bind_memory_node(int node);
[[nodiscard]] int bound_memory_node(const MemoryPolicy&);
[[nodiscard]] std::size_t host_page_size();
[[nodiscard]] std::size_t page_rounded_bytes(std::size_t);
// No allocator-cache pages: each extent is anonymous, bound before prefault,
// and verified before publication. Growth is transactional and high-water.
class NumaMemory final {
public:
 explicit NumaMemory(int node);
 ~NumaMemory();
 NumaMemory(const NumaMemory&) = delete;
 NumaMemory& operator=(const NumaMemory&) = delete;
 NumaMemory(NumaMemory&&) noexcept;
 NumaMemory& operator=(NumaMemory&&) noexcept;
 void ensure_bytes(std::size_t);
 void reset() noexcept;
 void verify() const;
 [[nodiscard]] void* data() const noexcept { return data_; }
 [[nodiscard]] std::size_t capacity_bytes() const noexcept { return bytes_; }
 [[nodiscard]] int node() const noexcept { return node_; }

private:
 int node_;
 void* data_ = nullptr;
 std::size_t bytes_ = 0;
};
class NumaMemoryResource final : public std::pmr::memory_resource {
public:
 explicit NumaMemoryResource(int node);
 [[nodiscard]] int node() const noexcept { return node_; }

private:
 void* do_allocate(std::size_t, std::size_t) override;
 void do_deallocate(void*, std::size_t, std::size_t) override;
 bool do_is_equal(const std::pmr::memory_resource&) const noexcept override;
 int node_;
};
}  // namespace mmltk::common::system
