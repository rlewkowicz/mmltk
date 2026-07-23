/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_cxxalloc_h)
#define mozilla_cxxalloc_h


#if !defined(MOZALLOC_EXPORT_NEW)
#  define MOZALLOC_EXPORT_NEW MFBT_API
#endif

MOZALLOC_EXPORT_NEW void* operator new(size_t size) noexcept(false) {
  return moz_xmalloc(size);
}

MOZALLOC_EXPORT_NEW void* operator new(size_t size,
                                       const std::nothrow_t&) noexcept(true) {
  return malloc_impl(size);
}

MOZALLOC_EXPORT_NEW void* operator new[](size_t size) noexcept(false) {
  return moz_xmalloc(size);
}

MOZALLOC_EXPORT_NEW void* operator new[](size_t size,
                                         const std::nothrow_t&) noexcept(true) {
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Walloc-size-larger-than="
#endif

  return malloc_impl(size);

#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif
}

MOZALLOC_EXPORT_NEW void operator delete(void* ptr) noexcept(true) {
  return free_impl(ptr);
}

MOZALLOC_EXPORT_NEW void operator delete(void* ptr,
                                         const std::nothrow_t&) noexcept(true) {
  return free_impl(ptr);
}

MOZALLOC_EXPORT_NEW void operator delete[](void* ptr) noexcept(true) {
  return free_impl(ptr);
}

MOZALLOC_EXPORT_NEW void operator delete[](
    void* ptr, const std::nothrow_t&) noexcept(true) {
  return free_impl(ptr);
}


#endif
