// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(SANDBOXING_COMMON_ENVIRONMENTMAP_H_)
#define SANDBOXING_COMMON_ENVIRONMENTMAP_H_

#include <map>
#include <memory>
#include <string>

namespace base {


typedef std::string NativeEnvironmentString;
typedef std::map<NativeEnvironmentString, NativeEnvironmentString>
    EnvironmentMap;

#  define ENVIRONMENT_LITERAL(x) x
#  define ENVIRONMENT_STRING(x) x

std::unique_ptr<char*[]> AlterEnvironment(const char* const* env,
                                          const EnvironmentMap& changes);


}  

#endif
