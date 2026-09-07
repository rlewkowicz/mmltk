// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#if !defined(BASE_THREAD_LOCAL_H_)
#define BASE_THREAD_LOCAL_H_

#include "base/basictypes.h"

#  include <pthread.h>

namespace base {

struct ThreadLocalPlatform {
  typedef pthread_key_t SlotType;

  static void AllocateSlot(SlotType& slot);
  static void FreeSlot(SlotType& slot);
  static void* GetValueFromSlot(SlotType& slot);
  static void SetValueInSlot(SlotType& slot, void* value);
};

template <typename Type>
class ThreadLocalPointer {
 public:
  ThreadLocalPointer() : slot_() { ThreadLocalPlatform::AllocateSlot(slot_); }

  ThreadLocalPointer(const ThreadLocalPointer&) = delete;
  ThreadLocalPointer& operator=(const ThreadLocalPointer&) = delete;

  ~ThreadLocalPointer() { ThreadLocalPlatform::FreeSlot(slot_); }

  Type* Get() {
    return static_cast<Type*>(ThreadLocalPlatform::GetValueFromSlot(slot_));
  }

  void Set(Type* ptr) { ThreadLocalPlatform::SetValueInSlot(slot_, ptr); }

 private:
  typedef ThreadLocalPlatform::SlotType SlotType;

  SlotType slot_;
};

class ThreadLocalBoolean {
 public:
  ThreadLocalBoolean() {}

  ThreadLocalBoolean(const ThreadLocalBoolean&) = delete;
  ThreadLocalBoolean& operator=(const ThreadLocalBoolean&) = delete;

  ~ThreadLocalBoolean() {}

  bool Get() { return tlp_.Get() != NULL; }

  void Set(bool val) {
    uintptr_t intVal = val ? 1 : 0;
    tlp_.Set(reinterpret_cast<void*>(intVal));
  }

 private:
  ThreadLocalPointer<void> tlp_;
};

}  

#endif
