/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef nsTraceRefcnt_h
#define nsTraceRefcnt_h

#include "nscore.h"

class nsTraceRefcnt {
 public:
  static void Shutdown();

  static nsresult DumpStatistics();

  static void ResetStatistics();

  static void SetActivityIsLegal(bool aLegal);

  static void StartLoggingClass(const char* aClass);

  static void EarlyInit();

#ifdef MOZ_ENABLE_FORKSERVER
  static void CloseLogFilesAfterFork();
  static void ReopenLogFilesAfterFork(const char* aProcType = nullptr);
#endif
};

#endif  // nsTraceRefcnt_h
