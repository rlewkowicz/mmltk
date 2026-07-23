/*
 *  Copyright (c) 2020 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#if !defined(VPX_VPX_PORTS_COMPILER_ATTRIBUTES_H_)
#define VPX_VPX_PORTS_COMPILER_ATTRIBUTES_H_

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

#if !defined(__has_attribute)
#define __has_attribute(x) 0
#endif


#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define VPX_WITH_ASAN 1
#else
#define VPX_WITH_ASAN 0
#endif

#if defined(__clang__) && __has_attribute(no_sanitize)
#define VPX_NO_UNSIGNED_OVERFLOW_CHECK \
  __attribute__((no_sanitize("unsigned-integer-overflow")))
#if __clang_major__ >= 12
#define VPX_NO_UNSIGNED_SHIFT_CHECK \
  __attribute__((no_sanitize("unsigned-shift-base")))
#endif
#endif

#if !defined(VPX_NO_UNSIGNED_OVERFLOW_CHECK)
#define VPX_NO_UNSIGNED_OVERFLOW_CHECK
#endif
#if !defined(VPX_NO_UNSIGNED_SHIFT_CHECK)
#define VPX_NO_UNSIGNED_SHIFT_CHECK
#endif


#if __has_attribute(uninitialized)
#define VPX_UNINITIALIZED __attribute__((uninitialized))
#else
#define VPX_UNINITIALIZED
#endif

#endif
