/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#if !defined(mozilla_Types_h)
#define mozilla_Types_h


#include <stddef.h>


#if defined(HAVE_VISIBILITY_ATTRIBUTE)
#    define MOZ_EXPORT __attribute__((visibility("default")))
#elif defined(__SUNPRO_C) || defined(__SUNPRO_CC)
#    define MOZ_EXPORT __global
#else
#    define MOZ_EXPORT /* nothing */
#endif

#  define MOZ_IMPORT_API MOZ_EXPORT

#  define MOZ_IMPORT_DATA MOZ_EXPORT

#if defined(IMPL_MFBT) ||                              \
    (defined(JS_STANDALONE) && !defined(MOZ_MEMORY) && \
     (defined(EXPORT_JS_API) || defined(STATIC_EXPORTABLE_JS_API)))
#  define MFBT_API MOZ_EXPORT
#  define MFBT_DATA MOZ_EXPORT
#else
#if defined(JS_STANDALONE) && !defined(MOZ_MEMORY) && defined(STATIC_JS_API)
#    define MFBT_API
#    define MFBT_DATA
#else
#if defined(MOZ_GLUE_IN_PROGRAM)
#      define MFBT_API __attribute__((weak)) MOZ_IMPORT_API
#      define MFBT_DATA __attribute__((weak)) MOZ_IMPORT_DATA
#else
#      define MFBT_API MOZ_IMPORT_API
#      define MFBT_DATA MOZ_IMPORT_DATA
#endif
#endif
#endif

#if defined(__cplusplus)
#  define MOZ_BEGIN_EXTERN_C extern "C" {
#  define MOZ_END_EXTERN_C }
#else
#  define MOZ_BEGIN_EXTERN_C
#  define MOZ_END_EXTERN_C
#endif

#endif
