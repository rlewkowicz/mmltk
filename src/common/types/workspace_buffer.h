#pragma once
#include <algorithm>
#include <cstddef>
#include <span>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
namespace mmltk::common::types {
// Trivial working arrays can use ordinary owning storage or a prepared span.
// Binding is explicit; an exhausted supplied span never falls back to the heap.
template<class T> class WorkspaceBuffer final {
 static_assert(std::is_trivially_destructible_v<T>);
 std::vector<T> owned_;
 std::span<T> supplied_;
 std::size_t size_ = 0;
 bool bound_ = false;
public:
 using value_type = T;
 WorkspaceBuffer() = default;
 void bind(std::span<T> memory) {
  if (owned_.capacity()) throw std::logic_error("workspace binding requires unallocated storage");
  supplied_ = memory; size_ = 0; bound_ = true;
 }
 [[nodiscard]] bool bound() const noexcept { return bound_; }
 [[nodiscard]] std::size_t size() const noexcept { return bound_ ? size_ : owned_.size(); }
 [[nodiscard]] std::size_t capacity() const noexcept { return bound_ ? supplied_.size() : owned_.capacity(); }
 [[nodiscard]] bool empty() const noexcept { return size() == 0; }
 T* data() noexcept { return bound_ ? supplied_.data() : owned_.data(); }
 const T* data() const noexcept { return bound_ ? supplied_.data() : owned_.data(); }
 T* begin() noexcept { return data(); }
 const T* begin() const noexcept { return data(); }
 T* end() noexcept { return size() ? data() + size() : data(); }
 const T* end() const noexcept { return size() ? data() + size() : data(); }
 T& operator[](std::size_t i) noexcept { return data()[i]; }
 const T& operator[](std::size_t i) const noexcept { return data()[i]; }
 T& front() noexcept { return *data(); }
 const T& front() const noexcept { return *data(); }
 T& back() noexcept { return data()[size()-1]; }
 const T& back() const noexcept { return data()[size()-1]; }
 void reserve(std::size_t count) {
  if (bound_) { if (count > capacity()) throw std::length_error("supplied workspace capacity exceeded"); }
  else owned_.reserve(count);
 }
 void resize(std::size_t count) {
  reserve(count);
  if (bound_) { if (count > size_) std::uninitialized_value_construct_n(data()+size_,count-size_); size_ = count; }
  else owned_.resize(count);
 }
 void clear() noexcept { if (bound_) size_ = 0; else owned_.clear(); }
 void push_back(T value) { if (bound_) { reserve(size_+1); std::construct_at(data()+size_,value); ++size_; } else owned_.push_back(value); }
 template<class... Args> void emplace_back(Args&&... args) { push_back(T(std::forward<Args>(args)...)); }
};
// Adapt an existing growable array or fixed workspace without duplicating the
// run projection/rollback algorithm. Storage ownership stays with the caller.
template<class T> class WorkspaceOutput final {
 void* value_;
 T* (*data_)(void*);
 std::size_t (*size_)(void*);
 std::size_t (*capacity_)(void*);
 void (*reserve_)(void*, std::size_t);
 void (*resize_)(void*, std::size_t);
 void (*append_)(void*, T);
public:
 template<class V> requires (!std::is_same_v<std::remove_cvref_t<V>,WorkspaceOutput>) WorkspaceOutput(V& v) : value_(&v), data_([](void* p){ return static_cast<V*>(p)->data(); }),
 size_([](void* p){ return static_cast<V*>(p)->size(); }), capacity_([](void* p){ return static_cast<V*>(p)->capacity(); }),
 reserve_([](void* p,std::size_t n){ static_cast<V*>(p)->reserve(n); }), resize_([](void* p,std::size_t n){ static_cast<V*>(p)->resize(n); }),
 append_([](void* p,T x){ static_cast<V*>(p)->push_back(x); }) {}
 std::size_t size() const { return size_(value_); }
 std::size_t capacity() const { return capacity_(value_); }
 T& back() { return data_(value_)[size()-1]; }
 void reserve(std::size_t n) { reserve_(value_,n); }
 void resize(std::size_t n) { resize_(value_,n); }
 void push_back(T value) { append_(value_,value); }
};
}
