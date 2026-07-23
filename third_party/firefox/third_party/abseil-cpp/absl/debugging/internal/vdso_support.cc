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


#include "absl/debugging/internal/vdso_support.h"
#include "absl/base/attributes.h"

#if defined(ABSL_HAVE_VDSO_SUPPORT)

#if !defined(__has_include)
#define __has_include(header) 0
#endif

#include <errno.h>
#include <fcntl.h>
#if __has_include(<syscall.h>)
#include <syscall.h>
#elif __has_include(<sys/syscall.h>)
#include <sys/syscall.h>
#endif
#include <unistd.h>

#if !defined(__UCLIBC__) && defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 16))
#define ABSL_HAVE_GETAUXVAL
#endif

#if defined(ABSL_HAVE_GETAUXVAL)
#include <sys/auxv.h>
#endif

#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/base/port.h"

#if !defined(AT_SYSINFO_EHDR)
#define AT_SYSINFO_EHDR 33  // for crosstoolv10
#endif


namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {

ABSL_CONST_INIT
std::atomic<const void *> VDSOSupport::vdso_base_(
    debugging_internal::ElfMemImage::kInvalidBase);

ABSL_CONST_INIT std::atomic<VDSOSupport::GetCpuFn> VDSOSupport::getcpu_fn_(
    &InitAndGetCPU);

VDSOSupport::VDSOSupport()
    : image_(vdso_base_.load(std::memory_order_relaxed) ==
                     debugging_internal::ElfMemImage::kInvalidBase
                 ? Init()
                 : vdso_base_.load(std::memory_order_relaxed)) {}

const void *VDSOSupport::Init() {
  const auto kInvalidBase = debugging_internal::ElfMemImage::kInvalidBase;
#if defined(ABSL_HAVE_GETAUXVAL)
  if (vdso_base_.load(std::memory_order_relaxed) == kInvalidBase) {
    errno = 0;
    const void *const sysinfo_ehdr =
        reinterpret_cast<const void *>(getauxval(AT_SYSINFO_EHDR));
    if (errno == 0) {
      vdso_base_.store(sysinfo_ehdr, std::memory_order_relaxed);
    }
  }
#endif
  if (vdso_base_.load(std::memory_order_relaxed) == kInvalidBase) {
    int fd = open("/proc/self/auxv", O_RDONLY);
    if (fd == -1) {
      vdso_base_.store(nullptr, std::memory_order_relaxed);
      getcpu_fn_.store(&GetCPUViaSyscall, std::memory_order_relaxed);
      return nullptr;
    }
    ElfW(auxv_t) aux;
    while (read(fd, &aux, sizeof(aux)) == sizeof(aux)) {
      if (aux.a_type == AT_SYSINFO_EHDR) {
        vdso_base_.store(reinterpret_cast<void *>(aux.a_un.a_val),
                         std::memory_order_relaxed);
        break;
      }
    }
    close(fd);
    if (vdso_base_.load(std::memory_order_relaxed) == kInvalidBase) {
      vdso_base_.store(nullptr, std::memory_order_relaxed);
    }
  }
  GetCpuFn fn = &GetCPUViaSyscall;  
  if (vdso_base_.load(std::memory_order_relaxed)) {
    VDSOSupport vdso;
    SymbolInfo info;
    if (vdso.LookupSymbol("__vdso_getcpu", "LINUX_2.6", STT_FUNC, &info)) {
      fn = reinterpret_cast<GetCpuFn>(const_cast<void *>(info.address));
    }
  }
  getcpu_fn_.store(fn, std::memory_order_relaxed);
  return vdso_base_.load(std::memory_order_relaxed);
}

const void *VDSOSupport::SetBase(const void *base) {
  ABSL_RAW_CHECK(base != debugging_internal::ElfMemImage::kInvalidBase,
                 "internal error");
  const void *old_base = vdso_base_.load(std::memory_order_relaxed);
  vdso_base_.store(base, std::memory_order_relaxed);
  image_.Init(base);
  getcpu_fn_.store(&InitAndGetCPU, std::memory_order_relaxed);
  return old_base;
}

bool VDSOSupport::LookupSymbol(const char *name,
                               const char *version,
                               int type,
                               SymbolInfo *info) const {
  return image_.LookupSymbol(name, version, type, info);
}

bool VDSOSupport::LookupSymbolByAddress(const void *address,
                                        SymbolInfo *info_out) const {
  return image_.LookupSymbolByAddress(address, info_out);
}

// NOLINT on 'long' because this routine mimics kernel api.
long VDSOSupport::GetCPUViaSyscall(unsigned *cpu,  // NOLINT(runtime/int)
                                   void *, void *) {
#if defined(SYS_getcpu)
  return syscall(SYS_getcpu, cpu, nullptr, nullptr);
#else
  static_cast<void>(cpu);  
  errno = ENOSYS;
  return -1;
#endif
}

long VDSOSupport::InitAndGetCPU(unsigned *cpu,  // NOLINT(runtime/int)
                                void *x, void *y) {
  Init();
  GetCpuFn fn = getcpu_fn_.load(std::memory_order_relaxed);
  ABSL_RAW_CHECK(fn != &InitAndGetCPU, "Init() did not set getcpu_fn_");
  return (*fn)(cpu, x, y);
}

ABSL_ATTRIBUTE_NO_SANITIZE_CFI
ABSL_ATTRIBUTE_NO_SANITIZE_MEMORY
int GetCPU() {
  unsigned cpu;
  long ret_code =  // NOLINT(runtime/int)
      (*VDSOSupport::getcpu_fn_)(&cpu, nullptr, nullptr);
  return ret_code == 0 ? static_cast<int>(cpu) : static_cast<int>(ret_code);
}

}  
ABSL_NAMESPACE_END
}  

#endif
