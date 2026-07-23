/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_dom_PosixSerialPlatformService_h)
#define mozilla_dom_PosixSerialPlatformService_h

#include "mozilla/UniquePtrExtensions.h"
#include "mozilla/dom/SerialPlatformService.h"
#include "nsTHashMap.h"

#if defined(XP_LINUX)
#  include <glib.h>

#  include "mozilla/UdevLib.h"
#endif


namespace mozilla::dom {

class PosixSerialPlatformService final : public SerialPlatformService {
 public:
  PosixSerialPlatformService();

  nsresult Init() override;
  void Shutdown() override;

 private:
  nsresult EnumeratePortsImpl(SerialPortList& aPorts,
                              bool* aLikelyAccessDenied) override;
  nsresult OpenImpl(const nsString& aPortId,
                    const IPCSerialOptions& aOptions) override;
  nsresult CloseImpl(const nsString& aPortId) override;
  nsresult WriteImpl(const nsString& aPortId,
                     Span<const uint8_t> aData) override;
  nsresult DrainImpl(const nsString& aPortId) override;
  nsresult FlushImpl(const nsString& aPortId, bool aReceive) override;
  nsresult SetSignalsImpl(const nsString& aPortId,
                          const IPCSerialOutputSignals& aSignals) override;
  nsresult GetSignalsImpl(const nsString& aPortId,
                          IPCSerialInputSignals& aSignals) override;
  nsresult GetReadStreamImpl(const nsString& aPortId, uint32_t aBufferSize,
                             nsIAsyncInputStream** aStream) override;
  ~PosixSerialPlatformService() override;

  int FindPortFd(const nsString& aPortId);
  nsresult ConfigurePort(int aFd, const IPCSerialOptions& aOptions);

  nsresult StartMonitoring();

#if defined(XP_LINUX)
  nsresult InitializeUdev();
  static gboolean OnUdevMonitor(GIOChannel* source, GIOCondition condition,
                                gpointer data);
  void ReadUdevChange();
  void PopulatePortInfoFromUdev(mozilla::udev_device* aDev,
                                const char* aDevnode,
                                IPCSerialPortInfo& aPortInfo);

  mozilla::UniquePtr<mozilla::udev_lib> mUdevLib;
  mozilla::udev_monitor* mMonitor;
  guint mMonitorSourceID;
#endif


  nsTHashMap<nsString, mozilla::UniqueFileHandle> mOpenPorts;
};

}  

#endif
