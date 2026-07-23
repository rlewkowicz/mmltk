/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(mozilla_UniquePtrExtensions_h)
#define mozilla_UniquePtrExtensions_h

#include <cstdlib>
#include <type_traits>

#include "mozilla/Attributes.h"
#include "mozilla/fallible.h"
#include "mozilla/Types.h"
#include "mozilla/UniquePtr.h"

#if defined(XP_UNIX) && (defined(DEBUG) || 0)
#  include "mozilla/Assertions.h"
#endif

namespace mozilla {

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::SingleObject MakeUniqueFallible(
    Args&&... aArgs) {
  return UniquePtr<T>(new (fallible) T(std::forward<Args>(aArgs)...));
}

template <typename T>
typename detail::UniqueSelector<T>::UnknownBound MakeUniqueFallible(
    decltype(sizeof(int)) aN) {
  using ArrayType = std::remove_extent_t<T>;
  return UniquePtr<T>(new (fallible) ArrayType[aN]());
}

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::KnownBound MakeUniqueFallible(
    Args&&... aArgs) = delete;

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::SingleObject MakeUniqueForOverwrite() {
  return UniquePtr<T>(new T);
}

template <typename T>
typename detail::UniqueSelector<T>::UnknownBound MakeUniqueForOverwrite(
    decltype(sizeof(int)) aN) {
  using ArrayType = std::remove_extent_t<T>;
  return UniquePtr<T>(new ArrayType[aN]);
}

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::KnownBound MakeUniqueForOverwrite(
    Args&&... aArgs) = delete;

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::SingleObject
MakeUniqueForOverwriteFallible() {
  return UniquePtr<T>(new (fallible) T);
}

template <typename T>
typename detail::UniqueSelector<T>::UnknownBound MakeUniqueForOverwriteFallible(
    decltype(sizeof(int)) aN) {
  using ArrayType = std::remove_extent_t<T>;
  return UniquePtr<T>(new (fallible) ArrayType[aN]);
}

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::KnownBound MakeUniqueForOverwriteFallible(
    Args&&... aArgs) = delete;

namespace detail {

template <typename T>
struct FreePolicy {
  void operator()(const void* ptr) { free(const_cast<void*>(ptr)); }
};

#if defined(XP_UNIX)
typedef int FileHandleType;
#else
#  error "Unsupported OS?"
#endif

struct FileHandleHelper {
  MOZ_IMPLICIT FileHandleHelper(FileHandleType aHandle) : mHandle(aHandle) {
#if defined(XP_UNIX) && (defined(DEBUG) || 0)
    MOZ_RELEASE_ASSERT(aHandle == kInvalidHandle || aHandle > 2);
#endif
  }

  MOZ_IMPLICIT constexpr FileHandleHelper() : mHandle(kInvalidHandle) {}

  MOZ_IMPLICIT constexpr FileHandleHelper(std::nullptr_t)
      : mHandle(kInvalidHandle) {}

  bool operator!=(std::nullptr_t) const {
    return mHandle != kInvalidHandle;
  }

  operator FileHandleType() const { return mHandle; }


  bool operator==(const FileHandleHelper& aOther) const {
    return mHandle == aOther.mHandle;
  }

 private:
  FileHandleType mHandle;

  static constexpr FileHandleType kInvalidHandle = -1;
};

struct FileHandleDeleter {
  using pointer = FileHandleHelper;
  using receiver = FileHandleType;
  MFBT_API void operator()(FileHandleHelper aHelper);
};


}  

template <typename T>
using UniqueFreePtr = UniquePtr<T, detail::FreePolicy<T>>;

using UniqueFileHandle =
    UniquePtr<detail::FileHandleType, detail::FileHandleDeleter>;

#if !defined(__wasm__)
MFBT_API UniqueFileHandle DuplicateFileHandle(detail::FileHandleType aFile);
inline UniqueFileHandle DuplicateFileHandle(const UniqueFileHandle& aFile) {
  return DuplicateFileHandle(aFile.get());
}
#endif

#if defined(XP_UNIX)
MFBT_API void SetCloseOnExec(detail::FileHandleType aFile);
inline void SetCloseOnExec(const UniqueFileHandle& aFile) {
  SetCloseOnExec(aFile.get());
}
#endif


namespace detail {

template <typename T, typename D, typename = void>
struct PointerType {
  using type = T*;
};

template <typename T, typename D>
struct PointerType<T, D,
                   std::void_t<typename std::remove_reference_t<D>::pointer>> {
  using type = typename std::remove_reference_t<D>::pointer;
};

template <typename T, typename D, typename = void>
struct ReceiverType : PointerType<T, D> {};

template <typename T, typename D>
struct ReceiverType<
    T, D, std::void_t<typename std::remove_reference_t<D>::receiver>> {
  using type = typename std::remove_reference_t<D>::receiver;
};

template <typename T, typename D>
class MOZ_TEMPORARY_CLASS UniquePtrGetterTransfers {
 public:
  using Ptr = UniquePtr<T, D>;
  using Receiver = typename ReceiverType<T, D>::type;

  explicit UniquePtrGetterTransfers(Ptr& p)
      : mPtr(p), mReceiver(typename Ptr::pointer(nullptr)) {}
  ~UniquePtrGetterTransfers() { mPtr.reset(mReceiver); }

  operator Receiver*() { return &mReceiver; }
  Receiver& operator*() { return mReceiver; }

  template <typename U = Receiver,
            typename = std::enable_if_t<
                std::is_pointer_v<U> && std::is_same_v<U, Receiver>, void>>
  operator void**() {
    return reinterpret_cast<void**>(&mReceiver);
  }

 private:
  Ptr& mPtr;
  Receiver mReceiver;
};

}  

template <typename T, typename D>
auto getter_Transfers(UniquePtr<T, D>& up) {
  return detail::UniquePtrGetterTransfers<T, D>(up);
}

}  

#endif
