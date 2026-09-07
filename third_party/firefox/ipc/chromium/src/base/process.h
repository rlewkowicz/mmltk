// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(BASE_PROCESS_H_)
#define BASE_PROCESS_H_

#include "base/basictypes.h"

#include <sys/types.h>

namespace base {

typedef pid_t ProcessHandle;
typedef pid_t ProcessId;
#  define PRIPID "d"

const ProcessHandle kInvalidProcessHandle = -1;
const ProcessId kInvalidProcessId = -1;

}  

#endif
