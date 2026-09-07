/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_Serial_h
#define mozilla_dom_Serial_h

#include "mozilla/DOMEventTargetHelper.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/dom/PSerialPortChild.h"
#include "mozilla/dom/SerialPortInfo.h"
#include "mozilla/ipc/Endpoint.h"
#include "nsCOMPtr.h"
#include "nsTArray.h"

namespace mozilla::dom {

class Promise;
class SerialManagerChild;
class SerialPort;
struct SerialPortRequestOptions;

class Serial final : public DOMEventTargetHelper, public SupportsWeakPtr {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(Serial, DOMEventTargetHelper)

  explicit Serial(nsPIDOMWindowInner* aWindow);
  explicit Serial(nsIGlobalObject* aGlobal);

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  already_AddRefed<Promise> RequestPort(
      const SerialPortRequestOptions& aOptions, ErrorResult& aRv);

  already_AddRefed<Promise> GetPorts(ErrorResult& aRv);

  IMPL_EVENT_HANDLER(connect)
  IMPL_EVENT_HANDLER(disconnect)

  void Shutdown();

  SerialManagerChild* GetOrCreateManagerChild();

  void DisconnectFromOwner() override;

  MOZ_CAN_RUN_SCRIPT void ForgetAllPorts();

  RefPtr<SerialPort> GetOrCreatePort(
      const IPCSerialPortInfo& aInfo,
      mozilla::ipc::Endpoint<PSerialPortChild>&& aEndpoint);

  MOZ_CAN_RUN_SCRIPT_BOUNDARY void ForgetPort(const nsAString& aPortId);

  static bool IsValidBluetoothUUID(const nsAString& aString);

  static bool ValidatePortFilter(bool aHasUsbVendorId, bool aHasUsbProductId,
                                 bool aHasBluetoothServiceClassId,
                                 nsACString& aFailureReason);

 private:
  ~Serial() override;

  nsTArray<RefPtr<SerialPort>> mPorts;

  RefPtr<SerialManagerChild> mManagerChild;

  bool mHasShutdown = false;

};

}  

#endif  // mozilla_dom_Serial_h
