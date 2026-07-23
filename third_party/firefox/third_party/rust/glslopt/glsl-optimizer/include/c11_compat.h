/* Copyright 2019 Intel Corporation */
/* SPDX-License-Identifier: MIT */

#include "no_extern_c.h"

#ifndef _C11_COMPAT_H_
#define _C11_COMPAT_H_

#if defined(__cplusplus)
#elif (__STDC_VERSION__ >= 201112L)
#else


#ifndef static_assert
#define static_assert _Static_assert
#endif


#endif /* !C++ && !C11 */

#endif /* _C11_COMPAT_H_ */
