// Copyright (c) 2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(BASE_SCOPED_NSAUTORELEASE_POOL_H_)
#define BASE_SCOPED_NSAUTORELEASE_POOL_H_

#include "base/basictypes.h"


namespace base {

class ScopedNSAutoreleasePool {
 public:
  ScopedNSAutoreleasePool() {}
  void Recycle() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(ScopedNSAutoreleasePool);
};

}  

#endif
