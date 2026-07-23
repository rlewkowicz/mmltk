/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ShutdownPhase_h
#define ShutdownPhase_h

namespace mozilla {

enum class ShutdownPhase {
  NotInShutdown = 0,
  AppShutdownConfirmed,
  AppShutdownNetTeardown,
  AppShutdownTeardown,
  AppShutdown,
  AppShutdownQM,
  AppShutdownTelemetry,
  XPCOMWillShutdown,
  XPCOMShutdown,
  XPCOMShutdownThreads,
  XPCOMShutdownFinal,
  CCPostLastCycleCollection,
  ShutdownPhase_Length,         
  First = AppShutdownConfirmed  
};

}  

#endif  // ShutdownPhase_h
