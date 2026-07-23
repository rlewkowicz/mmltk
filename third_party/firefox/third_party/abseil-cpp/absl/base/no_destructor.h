// Copyright 2023 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef ABSL_BASE_NO_DESTRUCTOR_H_
#define ABSL_BASE_NO_DESTRUCTOR_H_

#include <new>
#include <type_traits>
#include <utility>

#include "absl/base/config.h"
#include "absl/base/nullability.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

template <typename T>
class NoDestructor {
 public:
  template <typename... Ts,
            std::enable_if_t<!std::is_same_v<void(std::decay_t<Ts>&...),
                                             void(NoDestructor&)>,
                             int> = 0>
  explicit constexpr NoDestructor(Ts&&... args)
      : impl_(std::forward<Ts>(args)...) {}

  explicit constexpr NoDestructor(const T& x) : impl_(x) {}
  explicit constexpr NoDestructor(T&& x)
      : impl_(std::move(x)) {}

  NoDestructor(const NoDestructor&) = delete;
  NoDestructor& operator=(const NoDestructor&) = delete;

  T& operator*() { return *get(); }
  T* absl_nonnull operator->() { return get(); }
  T* absl_nonnull get() { return impl_.get(); }
  const T& operator*() const { return *get(); }
  const T* absl_nonnull operator->() const { return get(); }
  const T* absl_nonnull get() const { return impl_.get(); }

 private:
  class DirectImpl {
   public:
    template <typename... Args>
    explicit constexpr DirectImpl(Args&&... args)
        : value_(std::forward<Args>(args)...) {}
    const T* absl_nonnull get() const { return &value_; }
    T* absl_nonnull get() { return &value_; }

   private:
    T value_;
  };

  class PlacementImpl {
   public:
    template <typename... Args>
    explicit PlacementImpl(Args&&... args) {
      new (&space_) T(std::forward<Args>(args)...);
    }
    const T* absl_nonnull get() const {
      return std::launder(reinterpret_cast<const T*>(&space_));
    }
    T* absl_nonnull get() {
      return std::launder(reinterpret_cast<T*>(&space_));
    }

   private:
    alignas(T) unsigned char space_[sizeof(T)];
  };

  std::conditional_t<std::is_trivially_destructible_v<T>, DirectImpl,
                     PlacementImpl>
      impl_;
};

template <typename T>
NoDestructor(T) -> NoDestructor<T>;

ABSL_NAMESPACE_END
}  

#endif  // ABSL_BASE_NO_DESTRUCTOR_H_
