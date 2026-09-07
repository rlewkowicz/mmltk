/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/glue/Debug.h"
#include "mozilla/Sprintf.h"

#include <stdarg.h>
#include <stdio.h>



static void vprintf_stderr_buffered(const char* aFmt, va_list aArgs) {
  char buffer[1024];
  va_list argsCpy;
  va_copy(argsCpy, aArgs);
  int n = VsprintfLiteral(buffer, aFmt, aArgs);
  if (n < int(sizeof(buffer))) {
    fputs(buffer, stderr);
  } else {
    vfprintf(stderr, aFmt, argsCpy);
  }
  va_end(argsCpy);
  fflush(stderr);
}

MFBT_API void vprintf_stderr(const char* aFmt, va_list aArgs) {
  vprintf_stderr_buffered(aFmt, aArgs);
}

MFBT_API void printf_stderr(const char* aFmt, ...) {
  va_list args;
  va_start(args, aFmt);
  vprintf_stderr(aFmt, args);
  va_end(args);
}

MFBT_API void fprintf_stderr(FILE* aFile, const char* aFmt, ...) {
  va_list args;
  va_start(args, aFmt);
  if (aFile == stderr) {
    vprintf_stderr(aFmt, args);
  } else {
    vfprintf(aFile, aFmt, args);
  }
  va_end(args);
}

MFBT_API void print_stderr(const std::string& aStr) {
  printf_stderr("%s", aStr.c_str());
}

MFBT_API void fprint_stderr(FILE* aFile, const std::string& aStr) {
  fprintf_stderr(aFile, "%s", aStr.c_str());
}

MFBT_API void print_stderr(std::stringstream& aStr) {
  print_stderr(aStr.str());
}

MFBT_API void fprint_stderr(FILE* aFile, std::stringstream& aStr) {
  if (aFile == stderr) {
    print_stderr(aStr);
  } else {
    fprint_stderr(aFile, aStr.str());
  }
}
