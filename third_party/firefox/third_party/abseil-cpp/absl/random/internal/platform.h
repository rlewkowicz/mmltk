// Copyright 2017 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#if !defined(ABSL_RANDOM_INTERNAL_PLATFORM_H_)
#define ABSL_RANDOM_INTERNAL_PLATFORM_H_






#if defined(__x86_64__) || defined(__x86_64) || defined(_M_AMD64) || \
    defined(_M_X64)
#define ABSL_ARCH_X86_64
#elif defined(__i386) || defined(_M_IX86)
#define ABSL_ARCH_X86_32
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define ABSL_ARCH_AARCH64
#elif defined(__arm__) || defined(__ARMEL__) || defined(_M_ARM)
#define ABSL_ARCH_ARM
#elif defined(__powerpc64__) || defined(__PPC64__) || defined(__powerpc__) || \
    defined(__ppc__) || defined(__PPC__)
#define ABSL_ARCH_PPC
#else
#endif


#if defined(__clang__) || defined(__GNUC__)
#define ABSL_RANDOM_INTERNAL_RESTRICT __restrict__
#elif defined(_MSC_VER)
#define ABSL_RANDOM_INTERNAL_RESTRICT __restrict
#else
#define ABSL_RANDOM_INTERNAL_RESTRICT
#endif

#define ABSL_HAVE_ACCELERATED_AES 0

#if defined(ABSL_ARCH_X86_64)

#if defined(__AES__) || defined(__AVX__)
#undef ABSL_HAVE_ACCELERATED_AES
#define ABSL_HAVE_ACCELERATED_AES 1
#endif

#elif defined(ABSL_ARCH_PPC)

#if (defined(__VEC__) || defined(__ALTIVEC__)) && defined(__VSX__) && \
    defined(__CRYPTO__)
#undef ABSL_HAVE_ACCELERATED_AES
#define ABSL_HAVE_ACCELERATED_AES 1
#endif

#elif defined(ABSL_ARCH_ARM) || defined(ABSL_ARCH_AARCH64)

#if defined(__ARM_NEON) && defined(__ARM_FEATURE_CRYPTO)
#undef ABSL_HAVE_ACCELERATED_AES
#define ABSL_HAVE_ACCELERATED_AES 1
#endif

#endif

#define ABSL_RANDOM_INTERNAL_AES_DISPATCH 0

#if defined(ABSL_ARCH_X86_64)
#undef ABSL_RANDOM_INTERNAL_AES_DISPATCH
#define ABSL_RANDOM_INTERNAL_AES_DISPATCH 1
#elif defined(__linux__) && defined(ABSL_ARCH_PPC)
#undef ABSL_RANDOM_INTERNAL_AES_DISPATCH
#define ABSL_RANDOM_INTERNAL_AES_DISPATCH 1
#elif (defined(__linux__) || 0) && defined(ABSL_ARCH_AARCH64)
#undef ABSL_RANDOM_INTERNAL_AES_DISPATCH
#define ABSL_RANDOM_INTERNAL_AES_DISPATCH 1
#elif defined(__linux__) && defined(ABSL_ARCH_ARM) && (__ARM_ARCH >= 8)
#undef ABSL_RANDOM_INTERNAL_AES_DISPATCH
#define ABSL_RANDOM_INTERNAL_AES_DISPATCH 1
#endif

#endif
