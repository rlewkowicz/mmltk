/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_loader_BaseLoadContext_h
#define js_loader_BaseLoadContext_h

#include "nsCycleCollectionParticipant.h"
#include "nsIStringBundle.h"

namespace mozilla::dom {
class ScriptLoadContext;
class WorkerLoadContext;
class WorkletLoadContext;
}  

namespace mozilla::loader {
class SyncLoadContext;
}

namespace JS::loader {

class ScriptLoadRequest;


enum class ContextKind { Window, Sync, Worker, Worklet };

class LoadContextBase : public nsISupports {
 private:
  ContextKind mKind;

 protected:
  virtual ~LoadContextBase();

 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_CLASS(LoadContextBase)

  explicit LoadContextBase(ContextKind kind);

  void SetRequest(ScriptLoadRequest* aRequest);

  virtual bool IsPreload() const { return false; }

  bool IsWindowContext() const { return mKind == ContextKind::Window; }
  mozilla::dom::ScriptLoadContext* AsWindowContext();
  const mozilla::dom::ScriptLoadContext* AsWindowContext() const;

  bool IsSyncContext() const { return mKind == ContextKind::Sync; }
  mozilla::loader::SyncLoadContext* AsSyncContext();

  bool IsWorkerContext() const { return mKind == ContextKind::Worker; }
  mozilla::dom::WorkerLoadContext* AsWorkerContext();

  bool IsWorkletContext() const { return mKind == ContextKind::Worklet; }
  mozilla::dom::WorkletLoadContext* AsWorkletContext();

  RefPtr<ScriptLoadRequest> mRequest;
};

}  

#endif  // js_loader_BaseLoadContext_h
