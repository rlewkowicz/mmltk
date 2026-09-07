/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(nscore_h_)
#define nscore_h_

#if !defined(NS_NO_XPCOM) && !defined(MOZ_NO_MOZALLOC)
#  include "mozilla/mozalloc.h"
#endif

#include <stdint.h>  // IWYU pragma: export

#include "mozilla/RefCountType.h"
#include "mozilla/SEH.h"



#if defined(HAVE_VISIBILITY_HIDDEN_ATTRIBUTE)
#  define NS_VISIBILITY_HIDDEN __attribute__((visibility("hidden")))
#else
#  define NS_VISIBILITY_HIDDEN
#endif

#if defined(HAVE_VISIBILITY_ATTRIBUTE)
#  define NS_VISIBILITY_DEFAULT __attribute__((visibility("default")))
#elif defined(__SUNPRO_C) || defined(__SUNPRO_CC)
#  define NS_VISIBILITY_DEFAULT __global
#else
#  define NS_VISIBILITY_DEFAULT
#endif

#define NS_HIDDEN_(type) NS_VISIBILITY_HIDDEN type
#define NS_EXTERNAL_VIS_(type) NS_VISIBILITY_DEFAULT type

#define NS_HIDDEN NS_VISIBILITY_HIDDEN
#define NS_EXTERNAL_VIS NS_VISIBILITY_DEFAULT


#if defined(__i386__) && defined(__GNUC__)
#  define NS_FASTCALL __attribute__((regparm(3), stdcall))
#  define NS_CONSTRUCTOR_FASTCALL __attribute__((regparm(3), stdcall))
#else
#  define NS_FASTCALL
#  define NS_CONSTRUCTOR_FASTCALL
#endif



#  define NS_IMPORT NS_EXTERNAL_VIS
#  define NS_IMPORT_(type) NS_EXTERNAL_VIS_(type)
#  define NS_EXPORT NS_EXTERNAL_VIS
#  define NS_EXPORT_(type) NS_EXTERNAL_VIS_(type)
#  define NS_IMETHOD_(type) virtual type
#  define NS_IMETHODIMP_(type) type
#  define NS_METHOD_(type) type
#  define NS_CALLBACK_(_type, _name) _type(*_name)
#  define NS_STDCALL
#  define NS_FROZENCALL


#define NS_IMETHOD NS_IMETHOD_(nsresult)
#define NS_IMETHODIMP NS_IMETHODIMP_(nsresult)


#define EXPORT_XPCOM_API(type) type
#define IMPORT_XPCOM_API(type) type
#define GLUE_XPCOM_API(type) type

#if defined(__cplusplus)
#  define NS_EXTERN_C extern "C"
#else
#  define NS_EXTERN_C
#endif

#define XPCOM_API(type) NS_EXTERN_C type

#if !defined(NS_FREE_PERMANENT_DATA)
#if defined(NS_BUILD_REFCNT_LOGGING) || defined(MOZ_VALGRIND) ||            \
      defined(MOZ_ASAN) || defined(MOZ_TSAN) || defined(MOZ_CODE_COVERAGE) || \
      defined(MOZ_PROFILE_GENERATE) || defined(JS_STRUCTURED_SPEW)
#    define NS_FREE_PERMANENT_DATA
#endif
#endif

#if defined(NS_NO_VTABLE)
#  undef NS_NO_VTABLE
#endif
#if defined(_MSC_VER)
#  define NS_NO_VTABLE __declspec(novtable)
#else
#  define NS_NO_VTABLE
#endif

#include "nsError.h"  // IWYU pragma: export

typedef MozRefCountType nsrefcnt;

namespace mozilla {

namespace detail {
template <typename T>
struct UnusedZero;
template <>
struct UnusedZero<nsresult> {
  using StorageType = nsresult;

  static constexpr bool value = true;
  static constexpr StorageType nullValue = NS_OK;

  static constexpr void AssertValid(StorageType aValue) {}
  static constexpr const nsresult& Inspect(const StorageType& aValue) {
    return aValue;
  }
  static constexpr nsresult Unwrap(StorageType aValue) { return aValue; }
  static constexpr StorageType Store(nsresult aValue) { return aValue; }
};
}  

template <typename T>
class GenericErrorResult;
template <>
class GenericErrorResult<nsresult>;

struct Ok;
template <typename V, typename E>
class Result;

template <typename E = nsresult>
inline Result<Ok, E> ToResult(nsresult aValue);
}  


#define NS_PTR_TO_INT32(x) ((int32_t)(intptr_t)(x))
#define NS_PTR_TO_UINT32(x) ((uint32_t)(intptr_t)(x))
#define NS_INT32_TO_PTR(x) ((void*)(intptr_t)(x))

#if defined(XPCOM_GLUE) && !defined(XPCOM_GLUE_USE_NSPR)
#  define XPCOM_GLUE_AVOID_NSPR
#endif

#endif
