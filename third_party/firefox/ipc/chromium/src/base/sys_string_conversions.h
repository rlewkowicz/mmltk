// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(BASE_SYS_STRING_CONVERSIONS_H_)
#define BASE_SYS_STRING_CONVERSIONS_H_


#include <string>
#include "base/basictypes.h"
#include "base/string16.h"

class StringPiece;

namespace base {

std::string SysWideToUTF8(const std::wstring& wide);
std::wstring SysUTF8ToWide(const StringPiece& utf8);

std::string SysWideToNativeMB(const std::wstring& wide);
std::wstring SysNativeMBToWide(const StringPiece& native_mb);

}  

#endif
