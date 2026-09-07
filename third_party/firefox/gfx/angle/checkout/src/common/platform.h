// Copyright 2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#if !defined(COMMON_PLATFORM_H_)
#define COMMON_PLATFORM_H_

#if defined(__Fuchsia__)
#    define ANGLE_PLATFORM_FUCHSIA 1
#    define ANGLE_PLATFORM_POSIX 1
#elif defined(__linux__) || defined(EMSCRIPTEN)
#    define ANGLE_PLATFORM_LINUX 1
#    define ANGLE_PLATFORM_POSIX 1
#elif 0 || 0 || 0 ||              \
    0 || 0 || defined(__GLIBC__) || defined(__GNU__) || \
    defined(__QNX__) || defined(__Fuchsia__) || 0
#    define ANGLE_PLATFORM_POSIX 1
#else
#    error Unsupported platform.
#endif

#if defined(ANGLE_PLATFORM_WINDOWS)
#if !defined(STRICT)
#        define STRICT 1
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#        define WIN32_LEAN_AND_MEAN 1
#endif
#if !defined(NOMINMAX)
#        define NOMINMAX 1
#endif

#    include <intrin.h>

#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY != WINAPI_FAMILY_DESKTOP_APP)
#        define ANGLE_ENABLE_WINDOWS_UWP 1
#endif

#if defined(ANGLE_ENABLE_D3D9)
#        include <d3d9.h>
#        include <d3dcompiler.h>
#endif

#if defined(ANGLE_ENABLE_D3D11) || defined(ANGLE_ENABLE_OPENGL)
#        include <d3d10_1.h>
#        include <d3d11.h>
#        include <d3d11_3.h>
#        include <d3d11on12.h>
#        include <d3d12.h>
#        include <d3dcompiler.h>
#        include <dxgi.h>
#        include <dxgi1_2.h>
#        include <dxgi1_4.h>
#endif

#if defined(ANGLE_ENABLE_D3D9) || defined(ANGLE_ENABLE_D3D11)
#        include <wrl.h>
#endif

#if defined(ANGLE_ENABLE_WINDOWS_UWP)
#        include <dxgi1_3.h>
#if defined(_DEBUG)
#            include <DXProgrammableCapture.h>
#            include <dxgidebug.h>
#endif
#endif

#if defined(__GNUC__)
#if __GNUC__ < 10 || __GNUC__ == 10 && __GNUC_MINOR__ < 4 || \
            __GNUC__ == 11 && __GNUC_MINOR__ < 3
#            define ANGLE_USE_STATIC_THREAD_LOCAL_VARIABLES 1
#endif
#endif

#    include <windows.h>

#    undef near
#    undef far
#    undef NEAR
#    undef FAR
#    define NEAR
#    define FAR
#endif

#if defined(__mips__) || defined(__arm__) || defined(__aarch64__) || defined(__riscv)
#    include <stddef.h>
#endif

#if !defined(ANGLE_LIKELY) || !defined(ANGLE_UNLIKELY)
#if defined(__GNUC__) || defined(__clang__)
#        define ANGLE_LIKELY(x) __builtin_expect_with_probability(!!(x), 1, 0.9999)
#        define ANGLE_UNLIKELY(x) __builtin_expect_with_probability(!!(x), 0, 0.9999)
#else
#        define ANGLE_LIKELY(x) (x)
#        define ANGLE_UNLIKELY(x) (x)
#endif
#endif

#if defined(ANGLE_PLATFORM_APPLE)
#    include <AvailabilityMacros.h>
#    include <TargetConditionals.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#        define ANGLE_WITH_ASAN 1
#endif
#endif

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#        define ANGLE_WITH_MSAN 1
#endif
#endif

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#        define ANGLE_WITH_TSAN 1
#endif
#endif

#if defined(__has_feature)
#if __has_feature(undefined_behavior_sanitizer)
#        define ANGLE_WITH_UBSAN 1
#endif
#endif

#if defined(ANGLE_WITH_ASAN) || defined(ANGLE_WITH_TSAN) || defined(ANGLE_WITH_UBSAN)
#    define ANGLE_WITH_SANITIZER 1
#endif

#include <stdint.h>
#if INTPTR_MAX == INT64_MAX
#    define ANGLE_IS_64_BIT_CPU 1
#else
#    define ANGLE_IS_32_BIT_CPU 1
#endif

#endif
