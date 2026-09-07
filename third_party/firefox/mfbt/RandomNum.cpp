/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "mozilla/RandomNum.h"

#if defined(XP_UNIX)
#  include <fcntl.h>
#  include <unistd.h>
#endif


#if 0 || 0 || 0 ||    \
    0 || 0 || 0 || \
    defined(__wasi__)
#  define USE_ARC4RANDOM
#endif

#if defined(__linux__)
#  include <linux/random.h>  // For GRND_NONBLOCK.
#  include <sys/syscall.h>   // For SYS_getrandom.

#if defined(__x86_64__)
#    define GETRANDOM_NR 318
#elif defined(__i386__)
#    define GETRANDOM_NR 355
#elif defined(__aarch64__)
#    define GETRANDOM_NR 278
#elif defined(__arm__)
#    define GETRANDOM_NR 384
#elif defined(__powerpc__)
#    define GETRANDOM_NR 359
#elif defined(__s390__)
#    define GETRANDOM_NR 349
#elif defined(__mips__)
#    include <sgidefs.h>
#if _MIPS_SIM == _MIPS_SIM_ABI32
#      define GETRANDOM_NR 4353
#elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define GETRANDOM_NR 5313
#elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define GETRANDOM_NR 6317
#endif
#endif

#if defined(SYS_getrandom)
#if defined(GETRANDOM_NR)
static_assert(GETRANDOM_NR == SYS_getrandom,
              "GETRANDOM_NR should match the actual SYS_getrandom value");
#endif
#else
#    define SYS_getrandom GETRANDOM_NR
#endif

#if defined(GRND_NONBLOCK)
static_assert(GRND_NONBLOCK == 1,
              "If GRND_NONBLOCK is not 1 the #define below is wrong");
#else
#    define GRND_NONBLOCK 1
#endif

#endif

namespace mozilla {

MFBT_API bool GenerateRandomBytesFromOS(void* aBuffer, size_t aLength) {
  MOZ_ASSERT(aBuffer);
  MOZ_ASSERT(aLength > 0);

#if defined(USE_ARC4RANDOM)  // 0

  arc4random_buf(aBuffer, aLength);
  return true;

#elif defined(XP_UNIX)  // defined(USE_ARC4RANDOM)

#if defined(__linux__)

  long bytesGenerated = syscall(SYS_getrandom, aBuffer, aLength, GRND_NONBLOCK);

  if (static_cast<unsigned long>(bytesGenerated) == aLength) {
    return true;
  }


#endif

  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    return false;
  }

  ssize_t bytesRead = read(fd, aBuffer, aLength);

  close(fd);

  return (static_cast<size_t>(bytesRead) == aLength);

#else
#  error "Platform needs to implement GenerateRandomBytesFromOS()"
#endif
}

MFBT_API Maybe<uint64_t> RandomUint64() {
  uint64_t randomNum;
  if (!GenerateRandomBytesFromOS(&randomNum, sizeof(randomNum))) {
    return Nothing();
  }

  return Some(randomNum);
}

MFBT_API uint64_t RandomUint64OrDie() {
  uint64_t randomNum;
  MOZ_RELEASE_ASSERT(GenerateRandomBytesFromOS(&randomNum, sizeof(randomNum)));
  return randomNum;
}

}  
