/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_CmdLineAndEnvUtils_h)
#define mozilla_CmdLineAndEnvUtils_h


#if defined(MOZILLA_INTERNAL_API)
#  include "prenv.h"
#  include "prprf.h"
#  include <string.h>
#endif


#include "mozilla/Maybe.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/TypedEnumBits.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>

#if !defined(NS_NO_XPCOM)
#  include "nsIFile.h"
#  include "mozilla/AlreadyAddRefed.h"
#endif

#undef None

namespace mozilla {

enum ArgResult {
  ARG_NONE = 0,
  ARG_FOUND = 1,
  ARG_BAD = 2  
};

template <typename CharT>
inline void RemoveArg(int& argc, CharT** argv) {
  do {
    *argv = *(argv + 1);
    ++argv;
  } while (*argv);

  --argc;
}

namespace internal {

#if 'a' == '\x61'
static inline constexpr bool isValidOptionCharacter(char c) {
  return ('0' <= c && c <= '9') || ('a' <= c && c <= 'z') || c == '-';
};

static inline constexpr char toLowercase(char c) {
  return ('A' <= c && c <= 'Z') ? char(c | ' ') : c;
};

template <typename CharT>
static inline constexpr char toNarrow(CharT c) {
  return (c & static_cast<CharT>(0xff)) == c ? c : 0xff;
};
#else
#  error Character conversion functions not implemented for this platform.
#endif

template <typename CharT>
static inline bool ArgStartsWith(const CharT* mixedstr, const char* lowerstr) {
  while (*lowerstr) {
    if (!*mixedstr) {
      return false;  
    }

    if (!isValidOptionCharacter(*lowerstr)) {
      return false;
    }

    if (toLowercase(toNarrow(*mixedstr)) != *lowerstr) {
      return false;  
    }

    ++lowerstr;
    ++mixedstr;
  }

  return true;
}

template <typename CharT>
static inline bool strimatch(const char* lowerstr, const CharT* mixedstr) {
  if (ArgStartsWith(mixedstr, lowerstr)) {
    return *(mixedstr + strlen(lowerstr)) == '\0';
  }

  return false;
}

template <typename CharT>
mozilla::Maybe<const CharT*> ReadAsOption(const CharT* str) {
  if (!str) {
    return Nothing();
  }
  if (*str == '-') {
    str++;
    if (*str == '-') {
      str++;
    }
    return Some(str);
  }
  return Nothing();
}

}  

using internal::ArgStartsWith;
using internal::strimatch;

const wchar_t kCommandLineDelimiter[] = L" \t";

enum class CheckArgFlag : uint32_t {
  None = 0,
  RemoveArg = (1 << 1)  
};

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(CheckArgFlag)

template <typename CharT>
inline ArgResult CheckArg(int& aArgc, CharT** aArgv, const char* aArg,
                          const CharT** aParam = nullptr,
                          CheckArgFlag aFlags = CheckArgFlag::RemoveArg) {
  using internal::ReadAsOption;
  MOZ_ASSERT(aArgv && aArg);

  CharT** curarg = aArgv + 1;  
  ArgResult ar = ARG_NONE;

  while (*curarg) {
    if (const auto arg = ReadAsOption(*curarg)) {
      if (ArgStartsWith(arg.value(), aArg)) {
        const auto nextChar = arg.value() + strlen(aArg);
        if (*nextChar == 0) {
          if (aFlags & CheckArgFlag::RemoveArg) {
            RemoveArg(aArgc, curarg);
          } else {
            ++curarg;
          }

          if (!aParam) {
            ar = ARG_FOUND;
            break;
          }

          if (*curarg) {
            if (ReadAsOption(*curarg)) {
              return ARG_BAD;
            }

            *aParam = *curarg;

            if (aFlags & CheckArgFlag::RemoveArg) {
              RemoveArg(aArgc, curarg);
            }

            ar = ARG_FOUND;
            break;
          }

          return ARG_BAD;
        }

        if (*nextChar == '=') {
          if (aParam) {
            *aParam = nextChar + 1;  

            if (aFlags & CheckArgFlag::RemoveArg) {
              RemoveArg(aArgc, curarg);
            }

            ar = ARG_FOUND;
          } else {
            ar = ARG_BAD;
          }

          break;
        }
      }
    }

    ++curarg;
  }

  return ar;
}

template <typename CharT>
inline ArgResult CheckArg(int& aArgc, CharT** aArgv, const char* aArg,
                          std::nullptr_t,
                          CheckArgFlag aFlags = CheckArgFlag::RemoveArg) {
  return CheckArg<CharT>(aArgc, aArgv, aArg,
                         static_cast<const CharT**>(nullptr), aFlags);
}

namespace internal {

template <typename CharT, typename ListT>
static bool MatchesAnyOf(CharT const* unknown, ListT const& known) {
  for (const char* k : known) {
    if (strimatch(k, unknown)) {
      return true;
    }
  }
  return false;
}

template <typename CharT, typename ReqContainerT, typename OptContainerT>
inline bool EnsureCommandlineSafeImpl(int aArgc, CharT** aArgv,
                                      ReqContainerT const& requiredParams,
                                      OptContainerT const& optionalParams) {

  static constexpr const char* osintLit = "osint";

  if (CheckArg(aArgc, aArgv, osintLit, nullptr, CheckArgFlag::None) !=
      ARG_FOUND) {
    return true;
  }

  if (aArgc < 4) {
    return false;
  }

  const auto arg1 = ReadAsOption(aArgv[1]);
  if (!arg1) return false;
  if (!strimatch(osintLit, arg1.value())) {
    return false;
  }
  int pos = 2;
  while (true) {
    if (pos >= aArgc) return false;

    auto const arg = ReadAsOption(aArgv[pos]);
    if (!arg) return false;

    if (MatchesAnyOf(arg.value(), optionalParams)) {
      ++pos;
      continue;
    }

    if (MatchesAnyOf(arg.value(), requiredParams)) {
      ++pos;
      break;
    }

    return false;
  }

  if (pos + 1 != aArgc) return false;
  if (ReadAsOption(aArgv[pos])) {
    return false;
  }

  return true;
}

template <typename CharT, typename ReqContainerT>
inline bool EnsureCommandlineSafeImpl(int aArgc, CharT** aArgv,
                                      ReqContainerT const& requiredParams,
                                      std::nullptr_t _ = nullptr) {
  struct {
    inline const char** begin() const { return nullptr; }
    inline const char** end() const { return nullptr; }
  } emptyContainer;
  return EnsureCommandlineSafeImpl(aArgc, aArgv, requiredParams,
                                   emptyContainer);
}
}  

template <typename CharT, typename ReqContainerT,
          typename OptContainerT = std::nullptr_t>
inline void EnsureCommandlineSafe(
    int aArgc, CharT** aArgv, ReqContainerT const& requiredParams,
    OptContainerT const& optionalParams = nullptr) {
  if (!internal::EnsureCommandlineSafeImpl(aArgc, aArgv, requiredParams,
                                           optionalParams)) {
    exit(127);
  }
}


#if defined(MOZILLA_INTERNAL_API) || 0

MOZ_NEVER_INLINE inline void SaveToEnv(const char* aEnvString) {
#if defined(MOZILLA_INTERNAL_API)
  char* expr = strdup(aEnvString);
  if (expr) {
    PR_SetEnv(expr);
  }

  MOZ_LSAN_INTENTIONALLY_LEAK_OBJECT(expr);
#endif
}

inline bool EnvHasValue(const char* aVarName) {
#if defined(MOZILLA_INTERNAL_API)
  const char* val = PR_GetEnv(aVarName);
  return val && *val;
#endif
}

#endif

#if !defined(NS_NO_XPCOM)
already_AddRefed<nsIFile> GetFileFromEnv(const char* name);
#endif

}  

#endif
