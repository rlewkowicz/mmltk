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


#include "absl/random/internal/randen_detect.h"


#include <cstdint>
#include <cstring>
#include <optional>  // IWYU pragma: keep

#include "absl/random/internal/platform.h"

#if !defined(__UCLIBC__) && defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 16))
#define ABSL_HAVE_GETAUXVAL
#endif

#if defined(ABSL_ARCH_X86_64)
#define ABSL_INTERNAL_USE_X86_CPUID
#elif defined(ABSL_ARCH_PPC) || defined(ABSL_ARCH_ARM) || \
    defined(ABSL_ARCH_AARCH64)
#if defined(__linux__) && defined(ABSL_HAVE_GETAUXVAL)
#define ABSL_INTERNAL_USE_LINUX_GETAUXVAL
#define ABSL_INTERNAL_USE_GETAUXVAL
#endif
#endif

#if defined(ABSL_INTERNAL_USE_X86_CPUID)
#if ABSL_HAVE_BUILTIN(__cpuid)
extern void __cpuid(int[4], int);
#else
static void __cpuid(int cpu_info[4], int info_type) {
  __asm__ volatile("cpuid \n\t"
                   : "=a"(cpu_info[0]), "=b"(cpu_info[1]), "=c"(cpu_info[2]),
                     "=d"(cpu_info[3])
                   : "a"(info_type), "c"(0));
}
#endif
#endif

#if defined(ABSL_INTERNAL_USE_LINUX_GETAUXVAL)

#include <sys/auxv.h>

static uint32_t GetAuxval(uint32_t hwcap_type) {
  return static_cast<uint32_t>(getauxval(hwcap_type));
}

#endif

#if defined(ABSL_INTERNAL_USE_ANDROID_GETAUXVAL)
#include <dlfcn.h>

static uint32_t GetAuxval(uint32_t hwcap_type) {
  // NOLINTNEXTLINE(runtime/int)
  typedef unsigned long (*getauxval_func_t)(unsigned long);

  dlerror();  
  void* libc_handle = dlopen("libc.so", RTLD_NOW);
  if (!libc_handle) {
    return 0;
  }
  uint32_t result = 0;
  void* sym = dlsym(libc_handle, "getauxval");
  if (sym) {
    getauxval_func_t func;
    memcpy(&func, &sym, sizeof(func));
    result = static_cast<uint32_t>((*func)(hwcap_type));
  }
  dlclose(libc_handle);
  return result;
}

#endif


namespace absl {
ABSL_NAMESPACE_BEGIN
namespace random_internal {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunreachable-code-return"
#endif


bool CPUSupportsRandenHwAes() {
#if defined(ABSL_INTERNAL_USE_X86_CPUID)
  int regs[4];
  __cpuid(reinterpret_cast<int*>(regs), 1);
  return regs[2] & (1 << 25);  

#elif defined(ABSL_INTERNAL_USE_GETAUXVAL)

#define AT_HWCAP 16
#define AT_HWCAP2 26
#if defined(ABSL_ARCH_PPC)
  static const uint32_t kVCRYPTO = 0x02000000;
  const uint32_t hwcap = GetAuxval(AT_HWCAP2);
  return (hwcap & kVCRYPTO) != 0;

#elif defined(ABSL_ARCH_ARM)
  static const uint32_t kNEON = 1 << 12;
  uint32_t hwcap = GetAuxval(AT_HWCAP);
  if ((hwcap & kNEON) == 0) {
    return false;
  }

  static const uint32_t kAES = 1 << 0;
  const uint32_t hwcap2 = GetAuxval(AT_HWCAP2);
  return (hwcap2 & kAES) != 0;

#elif defined(ABSL_ARCH_AARCH64)
  static const uint32_t kNEON = 1 << 1;
  static const uint32_t kAES = 1 << 3;
  const uint32_t hwcap = GetAuxval(AT_HWCAP);
  return ((hwcap & kNEON) != 0) && ((hwcap & kAES) != 0);
#endif

#else
  return ABSL_HAVE_ACCELERATED_AES ? true : false;

#endif
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

}  
ABSL_NAMESPACE_END
}  
