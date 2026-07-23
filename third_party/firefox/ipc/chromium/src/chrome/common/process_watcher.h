// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(CHROME_COMMON_PROCESS_WATCHER_H_)
#define CHROME_COMMON_PROCESS_WATCHER_H_

#include "base/basictypes.h"
#include "base/process_util.h"
#if defined(XP_UNIX)
#  include "mozilla/UniquePtrExtensions.h"
#endif

class ProcessWatcher {
 public:
#if defined(NS_FREE_PERMANENT_DATA)
  static constexpr bool kDefaultForce = false;
#else
  static constexpr bool kDefaultForce = true;
#endif

  static void EnsureProcessTerminated(base::ProcessHandle process_handle,
                                      bool force = kDefaultForce);

#if defined(XP_UNIX)
  static mozilla::UniqueFileHandle GetSignalPipe();
#endif

 private:
  ProcessWatcher();

  DISALLOW_COPY_AND_ASSIGN(ProcessWatcher);
};

#endif
