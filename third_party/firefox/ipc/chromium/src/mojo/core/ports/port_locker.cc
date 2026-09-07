// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/core/ports/port_locker.h"

#include <algorithm>

#include "mojo/core/ports/port.h"

#if defined(DEBUG)
#  include "base/thread_local.h"
#endif

namespace mojo {
namespace core {
namespace ports {

namespace {

#if defined(DEBUG)
void UpdateTLS(PortLocker* old_locker, PortLocker* new_locker) {
  static auto* tls = new base::ThreadLocalPointer<PortLocker>();
  DCHECK_EQ(old_locker, tls->Get());
  tls->Set(new_locker);
}
#endif

}  

PortLocker::PortLocker(const PortRef** port_refs, size_t num_ports)
    : port_refs_(port_refs), num_ports_(num_ports) {
#if defined(DEBUG)
  UpdateTLS(nullptr, this);
#endif

#if defined(MOZ_USE_SINGLETON_PORT_MUTEX)
  detail::PortMutex::sSingleton.Lock();
#endif

  std::sort(
      port_refs_, port_refs_ + num_ports_,
      [](const PortRef* a, const PortRef* b) { return a->port() < b->port(); });
  for (size_t i = 0; i < num_ports_; ++i) {
    CHECK(port_refs_[i]->port());
    port_refs_[i]->port()->lock_.Lock();
  }
}

PortLocker::~PortLocker() {
  for (size_t i = 0; i < num_ports_; ++i) {
    port_refs_[i]->port()->lock_.Unlock();
  }

#if defined(MOZ_USE_SINGLETON_PORT_MUTEX)
  detail::PortMutex::sSingleton.Unlock();
#endif

#if defined(DEBUG)
  UpdateTLS(this, nullptr);
#endif
}

#if defined(DEBUG)
void PortLocker::AssertNoPortsLockedOnCurrentThread() {
  UpdateTLS(nullptr, nullptr);
}
#endif

SinglePortLocker::SinglePortLocker(const PortRef* port_ref)
    : port_ref_(port_ref), locker_(&port_ref_, 1) {}

SinglePortLocker::~SinglePortLocker() = default;

}  
}  
}  
