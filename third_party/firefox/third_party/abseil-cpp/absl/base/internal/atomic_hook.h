// Copyright 2017 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_BASE_INTERNAL_ATOMIC_HOOK_H_
#define ABSL_BASE_INTERNAL_ATOMIC_HOOK_H_

#include <atomic>
#include <cassert>
#include <cstdint>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/config.h"

#if defined(_MSC_VER) && !defined(__clang__)
#define ABSL_HAVE_WORKING_CONSTEXPR_STATIC_INIT 0
#else
#define ABSL_HAVE_WORKING_CONSTEXPR_STATIC_INIT 1
#endif

#if defined(_MSC_VER)
#define ABSL_HAVE_WORKING_ATOMIC_POINTER 0
#else
#define ABSL_HAVE_WORKING_ATOMIC_POINTER 1
#endif

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace base_internal {

template <typename T>
class AtomicHook;

#if ABSL_HAVE_WORKING_CONSTEXPR_STATIC_INIT
#define ABSL_INTERNAL_ATOMIC_HOOK_ATTRIBUTES ABSL_CONST_INIT
#else
#define ABSL_INTERNAL_ATOMIC_HOOK_ATTRIBUTES
#endif

template <typename ReturnType, typename... Args>
class AtomicHook<ReturnType (*)(Args...)> {
 public:
  using FnPtr = ReturnType (*)(Args...);

  constexpr AtomicHook() : AtomicHook(DummyFunction) {}

#if ABSL_HAVE_WORKING_ATOMIC_POINTER && ABSL_HAVE_WORKING_CONSTEXPR_STATIC_INIT
  explicit constexpr AtomicHook(FnPtr default_fn)
      : hook_(default_fn), default_fn_(default_fn) {}
#elif ABSL_HAVE_WORKING_CONSTEXPR_STATIC_INIT
  explicit constexpr AtomicHook(FnPtr default_fn)
      : hook_(kUninitialized), default_fn_(default_fn) {}
#else
  explicit constexpr AtomicHook(FnPtr default_fn)
      :  default_fn_(default_fn) {
    static_assert(kUninitialized == 0, "here we rely on zero-initialization");
  }
#endif

  void Store(FnPtr fn) {
    bool success = DoStore(fn);
    static_cast<void>(success);
    assert(success);
  }

  template <typename... CallArgs>
  ReturnType operator()(CallArgs&&... args) const {
    return DoLoad()(std::forward<CallArgs>(args)...);
  }

  FnPtr Load() const {
    FnPtr ptr = DoLoad();
    return (ptr == DummyFunction) ? nullptr : ptr;
  }

 private:
  static ReturnType DummyFunction(Args...) {
    return ReturnType();
  }

#if ABSL_HAVE_WORKING_ATOMIC_POINTER
  FnPtr DoLoad() const { return hook_.load(std::memory_order_acquire); }

  bool DoStore(FnPtr fn) {
    assert(fn);
    FnPtr expected = default_fn_;
    const bool store_succeeded = hook_.compare_exchange_strong(
        expected, fn, std::memory_order_acq_rel, std::memory_order_acquire);
    const bool same_value_already_stored = (expected == fn);
    return store_succeeded || same_value_already_stored;
  }

  std::atomic<FnPtr> hook_;
#else  // !ABSL_HAVE_WORKING_ATOMIC_POINTER
  static constexpr intptr_t kUninitialized = 0;

  static_assert(sizeof(intptr_t) >= sizeof(FnPtr),
                "intptr_t can't contain a function pointer");

  FnPtr DoLoad() const {
    const intptr_t value = hook_.load(std::memory_order_acquire);
    if (value == kUninitialized) {
      return default_fn_;
    }
    return reinterpret_cast<FnPtr>(value);
  }

  bool DoStore(FnPtr fn) {
    assert(fn);
    const auto value = reinterpret_cast<intptr_t>(fn);
    intptr_t expected = kUninitialized;
    const bool store_succeeded = hook_.compare_exchange_strong(
        expected, value, std::memory_order_acq_rel, std::memory_order_acquire);
    const bool same_value_already_stored = (expected == value);
    return store_succeeded || same_value_already_stored;
  }

  std::atomic<intptr_t> hook_;
#endif

  const FnPtr default_fn_;
};

#undef ABSL_HAVE_WORKING_ATOMIC_POINTER
#undef ABSL_HAVE_WORKING_CONSTEXPR_STATIC_INIT

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_BASE_INTERNAL_ATOMIC_HOOK_H_
