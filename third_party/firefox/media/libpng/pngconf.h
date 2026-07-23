/* pngconf.h - machine-configurable file for libpng
 *
 * libpng version 1.6.58
 *
 * Copyright (c) 2018-2026 Cosmin Truta
 * Copyright (c) 1998-2002,2004,2006-2016,2018 Glenn Randers-Pehrson
 * Copyright (c) 1996-1997 Andreas Dilger
 * Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * Any machine specific code is near the front of this file, so if you
 * are configuring libpng for a machine, you may want to read the section
 * starting here down to where it starts to typedef png_color, png_text,
 * and png_info.
 */

#if !defined(PNGCONF_H)
#define PNGCONF_H

#if !defined(PNG_BUILDING_SYMBOL_TABLE)

#include <limits.h>
#include <stddef.h>


#if defined(PNG_STDIO_SUPPORTED)
#  include <stdio.h>
#endif

#if defined(PNG_SETJMP_SUPPORTED)
#  include <setjmp.h>
#endif

#if defined(PNG_CONVERT_tIME_SUPPORTED)
#  include <time.h>
#endif

#endif

#define PNG_CONST const /* backward compatibility only */

#if !defined(PNG_READ_INT_FUNCTIONS_SUPPORTED)
#  define PNG_USE_READ_MACROS
#endif
#if !defined(PNG_NO_USE_READ_MACROS) && !defined(PNG_USE_READ_MACROS)
#if PNG_DEFAULT_READ_MACROS
#    define PNG_USE_READ_MACROS
#endif
#endif


#if !defined(PNGARG)
#  define PNGARG(arglist) arglist
#endif



#if 0 || 0 || defined(__NT__) || \
    0
#if PNG_API_RULE == 2
#    define PNGCAPI __watcall
#endif

#if defined(__GNUC__) || (defined(_MSC_VER) && (_MSC_VER >= 800))
#    define PNGCAPI __cdecl
#if PNG_API_RULE == 1
#      define PNGAPI __stdcall
#endif
#else
#if !defined(PNGCAPI)
#      define PNGCAPI _cdecl
#endif
#if PNG_API_RULE == 1 && !defined(PNGAPI)
#      define PNGAPI _stdcall
#endif
#endif


#if defined(PNGAPI) && !defined(PNG_USER_PRIVATEBUILD)
#     error PNG_USER_PRIVATEBUILD must be defined if PNGAPI is changed
#endif

#  define PNG_DLL_EXPORT __declspec(dllexport)
#if !defined(PNG_DLL_IMPORT)
#    define PNG_DLL_IMPORT __declspec(dllimport)
#endif

#else
#if (defined(__IBMC__) || defined(__IBMCPP__)) && defined(__OS2__)
#    define PNGAPI _System
#else
#endif
#endif

#if !defined(PNGCAPI)
#  define PNGCAPI
#endif
#if !defined(PNGCBAPI)
#  define PNGCBAPI PNGCAPI
#endif
#if !defined(PNGAPI)
#  define PNGAPI PNGCAPI
#endif

#if !defined(PNG_IMPEXP)
#if defined(PNG_USE_DLL) && defined(PNG_DLL_IMPORT)
#    define PNG_IMPEXP PNG_DLL_IMPORT
#endif

#if !defined(PNG_IMPEXP)
#    define PNG_IMPEXP
#endif
#endif

#if !defined(PNG_FUNCTION)
#  define PNG_FUNCTION(type, name, args, attributes) attributes type name args
#endif

#if !defined(PNG_EXPORT_TYPE)
#  define PNG_EXPORT_TYPE(type) PNG_IMPEXP type
#endif


#if !defined(PNG_EXPORTA)
#  define PNG_EXPORTA(ordinal, type, name, args, attributes) \
      PNG_FUNCTION(PNG_EXPORT_TYPE(type), (PNGAPI name), args, \
      PNG_LINKAGE_API attributes)
#endif

#define PNG_EMPTY /*empty list*/

#define PNG_EXPORT(ordinal, type, name, args) \
   PNG_EXPORTA(ordinal, type, name, args, PNG_EMPTY)

#if !defined(PNG_REMOVED)
#  define PNG_REMOVED(ordinal, type, name, args, attributes)
#endif

#if !defined(PNG_CALLBACK)
#  define PNG_CALLBACK(type, name, args) type (PNGCBAPI name) args
#endif


#if !defined(PNG_NO_PEDANTIC_WARNINGS)
#if !defined(PNG_PEDANTIC_WARNINGS_SUPPORTED)
#    define PNG_PEDANTIC_WARNINGS_SUPPORTED
#endif
#endif

#if defined(PNG_PEDANTIC_WARNINGS_SUPPORTED)
#if defined(__clang__) && defined(__has_attribute)
#if !defined(PNG_USE_RESULT) && __has_attribute(__warn_unused_result__)
#      define PNG_USE_RESULT __attribute__((__warn_unused_result__))
#endif
#if !defined(PNG_NORETURN) && __has_attribute(__noreturn__)
#      define PNG_NORETURN __attribute__((__noreturn__))
#endif
#if !defined(PNG_ALLOCATED) && __has_attribute(__malloc__)
#      define PNG_ALLOCATED __attribute__((__malloc__))
#endif
#if !defined(PNG_DEPRECATED) && __has_attribute(__deprecated__)
#      define PNG_DEPRECATED __attribute__((__deprecated__))
#endif
#if !defined(PNG_PRIVATE)
#if defined(__has_extension)
#if __has_extension(attribute_unavailable_with_message)
#          define PNG_PRIVATE __attribute__((__unavailable__(\
             "This function is not exported by libpng.")))
#endif
#endif
#endif
#if !defined(PNG_RESTRICT)
#      define PNG_RESTRICT __restrict
#endif

#elif defined(__GNUC__)
#if !defined(PNG_USE_RESULT)
#      define PNG_USE_RESULT __attribute__((__warn_unused_result__))
#endif
#if !defined(PNG_NORETURN)
#      define PNG_NORETURN   __attribute__((__noreturn__))
#endif
#if __GNUC__ >= 3
#if !defined(PNG_ALLOCATED)
#        define PNG_ALLOCATED  __attribute__((__malloc__))
#endif
#if !defined(PNG_DEPRECATED)
#        define PNG_DEPRECATED __attribute__((__deprecated__))
#endif
#if !defined(PNG_PRIVATE)
#          define PNG_PRIVATE \
            __attribute__((__deprecated__))
#endif
#if ((__GNUC__ > 3) || !defined(__GNUC_MINOR__) || (__GNUC_MINOR__ >= 1))
#if !defined(PNG_RESTRICT)
#          define PNG_RESTRICT __restrict
#endif
#endif
#endif

#elif defined(_MSC_VER)  && (_MSC_VER >= 1300)
#if !defined(PNG_USE_RESULT)
#      define PNG_USE_RESULT /* not supported */
#endif
#if !defined(PNG_NORETURN)
#      define PNG_NORETURN   __declspec(noreturn)
#endif
#if !defined(PNG_ALLOCATED)
#if (_MSC_VER >= 1400)
#        define PNG_ALLOCATED __declspec(restrict)
#endif
#endif
#if !defined(PNG_DEPRECATED)
#      define PNG_DEPRECATED __declspec(deprecated)
#endif
#if !defined(PNG_PRIVATE)
#      define PNG_PRIVATE __declspec(deprecated)
#endif
#if !defined(PNG_RESTRICT)
#if (_MSC_VER >= 1400)
#        define PNG_RESTRICT __restrict
#endif
#endif

#elif defined(__WATCOMC__)
#if !defined(PNG_RESTRICT)
#      define PNG_RESTRICT __restrict
#endif
#endif
#endif

#if !defined(PNG_DEPRECATED)
#  define PNG_DEPRECATED  /* Use of this function is deprecated */
#endif
#if !defined(PNG_USE_RESULT)
#  define PNG_USE_RESULT  /* The result of this function must be checked */
#endif
#if !defined(PNG_NORETURN)
#  define PNG_NORETURN    /* This function does not return */
#endif
#if !defined(PNG_ALLOCATED)
#  define PNG_ALLOCATED   /* The result of the function is new memory */
#endif
#if !defined(PNG_PRIVATE)
#  define PNG_PRIVATE     /* This is a private libpng function */
#endif
#if !defined(PNG_RESTRICT)
#  define PNG_RESTRICT    /* The C99 "restrict" feature */
#endif

#if !defined(PNG_FP_EXPORT)
#if defined(PNG_FLOATING_POINT_SUPPORTED)
#     define PNG_FP_EXPORT(ordinal, type, name, args)\
         PNG_EXPORT(ordinal, type, name, args);
#else
#     define PNG_FP_EXPORT(ordinal, type, name, args)
#endif
#endif
#if !defined(PNG_FIXED_EXPORT)
#if defined(PNG_FIXED_POINT_SUPPORTED)
#     define PNG_FIXED_EXPORT(ordinal, type, name, args)\
         PNG_EXPORT(ordinal, type, name, args);
#else
#     define PNG_FIXED_EXPORT(ordinal, type, name, args)
#endif
#endif

#if !defined(PNG_BUILDING_SYMBOL_TABLE)
#if CHAR_BIT == 8 && UCHAR_MAX == 255
   typedef unsigned char png_byte;
#else
#  error libpng requires 8-bit bytes
#endif

#if INT_MIN == -32768 && INT_MAX == 32767
   typedef int png_int_16;
#elif SHRT_MIN == -32768 && SHRT_MAX == 32767
   typedef short png_int_16;
#else
#  error libpng requires a signed 16-bit integer type
#endif

#if UINT_MAX == 65535
   typedef unsigned int png_uint_16;
#elif USHRT_MAX == 65535
   typedef unsigned short png_uint_16;
#else
#  error libpng requires an unsigned 16-bit integer type
#endif

#if INT_MIN < -2147483646 && INT_MAX > 2147483646
   typedef int png_int_32;
#elif LONG_MIN < -2147483646 && LONG_MAX > 2147483646
   typedef long int png_int_32;
#else
#  error libpng requires a signed 32-bit (or longer) integer type
#endif

#if UINT_MAX > 4294967294U
   typedef unsigned int png_uint_32;
#elif ULONG_MAX > 4294967294U
   typedef unsigned long int png_uint_32;
#else
#  error libpng requires an unsigned 32-bit (or longer) integer type
#endif

typedef size_t png_size_t;
typedef ptrdiff_t png_ptrdiff_t;

#if !defined(PNG_SMALL_SIZE_T)
#if (defined(__TURBOC__) && !defined(__FLAT__)) ||\
   (defined(_MSC_VER) && defined(MAXSEG_64K))
#     define PNG_SMALL_SIZE_T
#endif
#endif

#if defined(PNG_SMALL_SIZE_T)
   typedef png_uint_32 png_alloc_size_t;
#else
   typedef size_t png_alloc_size_t;
#endif


typedef png_int_32 png_fixed_point;

typedef void                  * png_voidp;
typedef const void            * png_const_voidp;
typedef png_byte              * png_bytep;
typedef const png_byte        * png_const_bytep;
typedef png_uint_32           * png_uint_32p;
typedef const png_uint_32     * png_const_uint_32p;
typedef png_int_32            * png_int_32p;
typedef const png_int_32      * png_const_int_32p;
typedef png_uint_16           * png_uint_16p;
typedef const png_uint_16     * png_const_uint_16p;
typedef png_int_16            * png_int_16p;
typedef const png_int_16      * png_const_int_16p;
typedef char                  * png_charp;
typedef const char            * png_const_charp;
typedef png_fixed_point       * png_fixed_point_p;
typedef const png_fixed_point * png_const_fixed_point_p;
typedef size_t                * png_size_tp;
typedef const size_t          * png_const_size_tp;

#if defined(PNG_FLOATING_POINT_SUPPORTED)
typedef double       * png_doublep;
typedef const double * png_const_doublep;
#endif

typedef png_byte        * * png_bytepp;
typedef png_uint_32     * * png_uint_32pp;
typedef png_int_32      * * png_int_32pp;
typedef png_uint_16     * * png_uint_16pp;
typedef png_int_16      * * png_int_16pp;
typedef const char      * * png_const_charpp;
typedef char            * * png_charpp;
typedef png_fixed_point * * png_fixed_point_pp;
#if defined(PNG_FLOATING_POINT_SUPPORTED)
typedef double          * * png_doublepp;
#endif

typedef char            * * * png_charppp;

#if defined(PNG_STDIO_SUPPORTED)
typedef FILE            * png_FILE_p; 
#endif

#endif

#endif
