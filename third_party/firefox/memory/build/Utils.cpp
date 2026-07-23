/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozjemalloc.h"

#include <mozilla/Assertions.h>

#  include <unistd.h>

size_t GetKernelPageSize() {
  static size_t kernel_page_size = ([]() {
    long result = sysconf(_SC_PAGESIZE);
    MOZ_ASSERT(result != -1);
    return result;
  })();
  return kernel_page_size;
}
