/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_WorkerCommon_h
#define mozilla_dom_workers_WorkerCommon_h

#include "js/TypeDecls.h"
#include "nsString.h"

class nsPIDOMWindowInner;

namespace mozilla::dom {

class WorkerPrivate;


WorkerPrivate* GetWorkerPrivateFromContext(JSContext* aCx);

WorkerPrivate* GetCurrentThreadWorkerPrivate();

bool IsCurrentThreadRunningWorker();

bool IsCurrentThreadRunningChromeWorker();

JSContext* GetCurrentWorkerThreadJSContext();

JSObject* GetCurrentThreadWorkerGlobal();

JSObject* GetCurrentThreadWorkerDebuggerGlobal();

void CancelWorkersForWindow(const nsPIDOMWindowInner& aWindow);

void UpdateWorkersBackgroundState(const nsPIDOMWindowInner& aWindow,
                                  bool aIsBackground);

void FreezeWorkersForWindow(const nsPIDOMWindowInner& aWindow);

void ThawWorkersForWindow(const nsPIDOMWindowInner& aWindow);

void SuspendWorkersForWindow(const nsPIDOMWindowInner& aWindow);

void ResumeWorkersForWindow(const nsPIDOMWindowInner& aWindow);

void PropagateStorageAccessPermissionGrantedToWorkers(
    const nsPIDOMWindowInner& aWindow);

void UpdateTimezoneOverrideForWorkers(const nsPIDOMWindowInner& aWindow,
                                      const nsAString& aTimezone);


bool IsWorkerGlobal(JSObject* global);

bool IsWorkerDebuggerGlobal(JSObject* global);

bool IsWorkerDebuggerSandbox(JSObject* object);

void UpdateWorkersPlaybackState(const nsPIDOMWindowInner& aWindow,
                                bool aIsPlayingAudio);

inline size_t GetWorkerScriptMaxSizeInBytes() {
  return nsString::LengthStorage::kMax;
}

}  

#endif  // mozilla_dom_workers_WorkerCommon_h
