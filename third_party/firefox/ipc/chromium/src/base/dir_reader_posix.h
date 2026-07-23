// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(BASE_DIR_READER_POSIX_H_)
#define BASE_DIR_READER_POSIX_H_
#pragma once



#if defined(XP_LINUX)
#  include "base/dir_reader_linux.h"
#else
#  include "base/dir_reader_fallback.h"
#endif

namespace base {

#if defined(XP_LINUX)
typedef DirReaderLinux DirReaderPosix;
#else
typedef DirReaderFallback DirReaderPosix;
#endif

}  

#endif
