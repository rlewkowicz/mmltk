// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !defined(MOJO_CORE_PORTS_MESSAGE_FILTER_H_)
#define MOJO_CORE_PORTS_MESSAGE_FILTER_H_

namespace mojo {
namespace core {
namespace ports {

class UserMessageEvent;

class MessageFilter {
 public:
  virtual ~MessageFilter() = default;

  virtual bool Match(const UserMessageEvent& message) = 0;
};

}  
}  
}  

#endif
