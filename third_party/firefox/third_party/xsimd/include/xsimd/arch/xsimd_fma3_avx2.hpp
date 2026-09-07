/***************************************************************************
 * Copyright (c) Johan Mabille, Sylvain Corlay, Wolf Vollprecht and         *
 * Martin Renou                                                             *
 * Copyright (c) QuantStack                                                 *
 * Copyright (c) Serge Guelton                                              *
 *                                                                          *
 * Distributed under the terms of the BSD 3-Clause License.                 *
 *                                                                          *
 * The full license is in the file LICENSE, distributed with this software. *
 ****************************************************************************/

#if !defined(XSIMD_FMA3_AVX2_HPP)
#define XSIMD_FMA3_AVX2_HPP

#include "../types/xsimd_fma3_avx2_register.hpp"

#if defined(XSIMD_FMA3_AVX_HPP)
#undef XSIMD_FMA3_AVX_HPP
#define XSIMD_FORCE_FMA3_AVX_HPP
#endif

#if !defined(XSIMD_FMA3_AVX_REGISTER_HPP)
#define XSIMD_FMA3_AVX_REGISTER_HPP
#define XSIMD_FORCE_FMA3_AVX_REGISTER_HPP
#endif

#define avx avx2
#include "./xsimd_fma3_avx.hpp"
#undef avx
#undef XSIMD_FMA3_AVX_HPP

#if defined(XSIMD_FORCE_FMA3_AVX_HPP)
#define XSIMD_FMA3_AVX_HPP
#undef XSIMD_FORCE_FMA3_AVX_HPP
#endif

#if defined(XSIMD_FORCE_FMA3_AVX_REGISTER_HPP)
#undef XSIMD_FMA3_AVX_REGISTER_HPP
#undef XSIMD_FORCE_FMA3_AVX_REGISTER_HPP
#endif

#endif
