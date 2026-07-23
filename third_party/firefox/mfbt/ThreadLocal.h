/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(mozilla_ThreadLocal_h)
#define mozilla_ThreadLocal_h

#if !0 && !defined(__wasi__)
#  include <pthread.h>
#endif

#include <type_traits>
#include <cstdint>

#include "mozilla/Assertions.h"

namespace mozilla {

namespace detail {



template <typename S>
struct Helper {
  typedef uintptr_t Type;
};

template <typename S>
struct Helper<S*> {
  typedef S* Type;
};

#if defined(__wasi__)
template <typename T>
class ThreadLocalKeyStorage {
 public:
  constexpr ThreadLocalKeyStorage() : mInited(false) {}

  inline bool initialized() const { return mInited; }

  inline void init() { mInited = true; }

  inline T get() const { return mVal; }

  inline bool set(const T aValue) {
    mVal = aValue;
    return true;
  }

 private:
  bool mInited;
  T mVal;
};
#else
template <typename T>
class ThreadLocalKeyStorage {
 public:
  constexpr ThreadLocalKeyStorage() : mKey(0), mInited(false) {}

  inline bool initialized() const { return mInited; }

  inline void init() { mInited = !pthread_key_create(&mKey, nullptr); }

  inline T get() const {
    void* h = pthread_getspecific(mKey);
    return static_cast<T>(reinterpret_cast<typename Helper<T>::Type>(h));
  }

  inline bool set(const T aValue) {
    const void* h = reinterpret_cast<const void*>(
        static_cast<typename Helper<T>::Type>(aValue));
    return !pthread_setspecific(mKey, h);
  }

 private:
  pthread_key_t mKey;
  bool mInited;
};
#endif

template <typename T>
class ThreadLocalNativeStorage {
 public:
  inline bool initialized() const { return true; }

  inline void init() {}

  inline T get() const { return mValue; }

  inline bool set(const T aValue) {
    mValue = aValue;
    return true;
  }

 private:
  T mValue;
};

template <typename T, template <typename U> class Storage>
class ThreadLocal : public Storage<T> {
 public:
  [[nodiscard]] inline bool init();

  void infallibleInit() {
    MOZ_RELEASE_ASSERT(init(), "Infallible TLS initialization failed");
  }

  inline T get() const;

  inline void set(const T aValue);

  using Type = T;
};

template <typename T, template <typename U> class Storage>
inline bool ThreadLocal<T, Storage>::init() {
  static_assert(std::is_pointer_v<T> || std::is_integral_v<T>,
                "mozilla::ThreadLocal must be used with a pointer or "
                "integral type");
  static_assert(sizeof(T) <= sizeof(void*),
                "mozilla::ThreadLocal can't be used for types larger than "
                "a pointer");

  if (!Storage<T>::initialized()) {
    Storage<T>::init();
  }
  return Storage<T>::initialized();
}

template <typename T, template <typename U> class Storage>
inline T ThreadLocal<T, Storage>::get() const {
  MOZ_ASSERT(Storage<T>::initialized());
  return Storage<T>::get();
}

template <typename T, template <typename U> class Storage>
inline void ThreadLocal<T, Storage>::set(const T aValue) {
  MOZ_ASSERT(Storage<T>::initialized());
  bool succeeded = Storage<T>::set(aValue);
  if (!succeeded) {
    MOZ_CRASH();
  }
}

#if (0 || defined(MACOSX_HAS_THREAD_LOCAL)) && \
    !defined(__MINGW32__)
#  define MOZ_THREAD_LOCAL(TYPE)                 \
    thread_local ::mozilla::detail::ThreadLocal< \
        TYPE, ::mozilla::detail::ThreadLocalNativeStorage>
#elif defined(HAVE_THREAD_TLS_KEYWORD) && !defined(MOZ_LINKER)
#  define MOZ_THREAD_LOCAL(TYPE)             \
    __thread ::mozilla::detail::ThreadLocal< \
        TYPE, ::mozilla::detail::ThreadLocalNativeStorage>
#else
#  define MOZ_THREAD_LOCAL(TYPE)         \
    ::mozilla::detail::ThreadLocal<TYPE, \
                                   ::mozilla::detail::ThreadLocalKeyStorage>
#endif

}  
}  

#endif
