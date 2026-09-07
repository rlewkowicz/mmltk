/*
** Copyright 2014-2026 The Khronos Group Inc.
**
** SPDX-License-Identifier: Apache-2.0
*/


#if !defined(VK_PLATFORM_H_)
#define VK_PLATFORM_H_

#if defined(__cplusplus)
extern "C"
{
#endif


    #define VKAPI_ATTR
    #define VKAPI_CALL
    #define VKAPI_PTR

#if !defined(VK_NO_STDDEF_H)
    #include <stddef.h>
#endif

#if !defined(VK_NO_STDINT_H)
    #if defined(_MSC_VER) && (_MSC_VER < 1600)
        typedef signed   __int8  int8_t;
        typedef unsigned __int8  uint8_t;
        typedef signed   __int16 int16_t;
        typedef unsigned __int16 uint16_t;
        typedef signed   __int32 int32_t;
        typedef unsigned __int32 uint32_t;
        typedef signed   __int64 int64_t;
        typedef unsigned __int64 uint64_t;
    #else
        #include <stdint.h>
    #endif
#endif

#if defined(__cplusplus)
} 
#endif

#endif
