/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_glue_Debug_h
#define mozilla_glue_Debug_h

#include "mozilla/Attributes.h"  // For MOZ_FORMAT_PRINTF
#include "mozilla/Types.h"       // For MFBT_API
#include "fmt/format.h"

#include <cstdarg>
#include <sstream>


#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

MFBT_API void printf_stderr(const char* aFmt, ...) MOZ_FORMAT_PRINTF(1, 2);

MFBT_API void vprintf_stderr(const char* aFmt, va_list aArgs)
    MOZ_FORMAT_PRINTF(1, 0);

MFBT_API void fprintf_stderr(FILE* aFile, const char* aFmt, ...)
    MOZ_FORMAT_PRINTF(2, 3);

#ifdef __cplusplus
}
#endif  // __cplusplus

#ifdef __cplusplus
MFBT_API void print_stderr(std::stringstream&);
MFBT_API void fprint_stderr(FILE*, std::stringstream&);
MFBT_API void print_stderr(const std::string&);
MFBT_API void fprint_stderr(FILE*, const std::string&);

template <typename... Args>
inline void print_stderr(fmt::format_string<std::type_identity_t<Args>...> aFmt,
                         Args&&... aArgs) {
  print_stderr(fmt::format(aFmt, std::forward<Args>(aArgs)...));
}
template <typename... Args>
inline void fprint_stderr(
    FILE* aFile, fmt::format_string<std::type_identity_t<Args>...> aFmt,
    Args&&... aArgs) {
  fprint_stderr(aFile, fmt::format(aFmt, std::forward<Args>(aArgs)...));
}
#endif

#endif  // mozilla_glue_Debug_h
