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


#include "absl/debugging/internal/address_is_readable.h"

#if !defined(__linux__) || 0

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {

bool AddressIsReadable(const void* ) { return true; }

}  
ABSL_NAMESPACE_END
}  

#else

#include <stdint.h>
#include <syscall.h>
#include <unistd.h>

#include "absl/base/internal/errno_saver.h"
#include "absl/base/internal/raw_logging.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {

bool AddressIsReadable(const void *addr) {
  const uintptr_t u_addr = reinterpret_cast<uintptr_t>(addr) & ~uintptr_t{7};
  addr = reinterpret_cast<const void *>(u_addr);

  if (addr == nullptr) return false;

  absl::base_internal::ErrnoSaver errno_saver;

  ABSL_RAW_CHECK(syscall(SYS_rt_sigprocmask, ~0, addr, nullptr,
                          8) == -1,
                 "unexpected success");
  ABSL_RAW_CHECK(errno == EFAULT || errno == EINVAL, "unexpected errno");
  return errno != EFAULT;
}

}  
ABSL_NAMESPACE_END
}  

#endif
