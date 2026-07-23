/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_RawEndpoint_h
#define mozilla_ipc_RawEndpoint_h

#include "mojo/core/ports/port_ref.h"

namespace mozilla::ipc {

class NodeController;

class ScopedPort {
  using PortName = mojo::core::ports::PortName;
  using PortRef = mojo::core::ports::PortRef;

 public:
  ScopedPort();
  ~ScopedPort();

  ScopedPort(PortRef aPort, NodeController* aController);

  ScopedPort(ScopedPort&& aOther);
  ScopedPort(const ScopedPort&) = delete;

  ScopedPort& operator=(ScopedPort&& aOther);
  ScopedPort& operator=(const ScopedPort&) = delete;

  bool IsValid() const { return mValid; }
  explicit operator bool() const { return IsValid(); }

  const PortName& Name() const { return mPort.name(); }
  const PortRef& Port() const { return mPort; }
  NodeController* Controller() const { return mController; }

  PortRef Release();

 private:
  void Reset();

  bool mValid = false;
  PortRef mPort;
  RefPtr<NodeController> mController;

};

}  

namespace IPC {

template <typename T>
struct ParamTraits;

template <>
struct ParamTraits<mozilla::ipc::ScopedPort> {
  using paramType = mozilla::ipc::ScopedPort;

  static void Write(MessageWriter* aWriter, paramType&& aParam);
  static bool Read(MessageReader* aReader, paramType* aResult);
};

}  

#endif
