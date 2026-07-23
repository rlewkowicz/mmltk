/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/AvailableMemoryTracker.h"


#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsIRunnable.h"
#include "nsISupports.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"

#include "mozilla/Mutex.h"
#include "mozilla/Services.h"

#if defined(MOZ_MEMORY)
#  include "mozmemory.h"
#endif

using namespace mozilla;

Atomic<uint32_t, MemoryOrdering::Relaxed> sNumLowPhysicalMemEvents;

namespace {


class nsJemallocFreeDirtyPagesRunnable final : public Runnable {
  ~nsJemallocFreeDirtyPagesRunnable() = default;


 public:
  NS_DECL_NSIRUNNABLE

  nsJemallocFreeDirtyPagesRunnable()
      : Runnable("nsJemallocFreeDirtyPagesRunnable") {}
};

NS_IMETHODIMP
nsJemallocFreeDirtyPagesRunnable::Run() {
  MOZ_ASSERT(NS_IsMainThread());

#if defined(MOZ_MEMORY)
  jemalloc_free_dirty_pages();
#endif


  return NS_OK;
}


class nsMemoryPressureWatcher final : public nsIObserver {
  ~nsMemoryPressureWatcher() = default;

 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

  void Init();
};

NS_IMPL_ISUPPORTS(nsMemoryPressureWatcher, nsIObserver)

void nsMemoryPressureWatcher::Init() {
  nsCOMPtr<nsIObserverService> os = services::GetObserverService();

  if (os) {
    os->AddObserver(this, "memory-pressure",  false);
  }
}

NS_IMETHODIMP
nsMemoryPressureWatcher::Observe(nsISupports* aSubject, const char* aTopic,
                                 const char16_t* aData) {
  MOZ_ASSERT(!strcmp(aTopic, "memory-pressure"), "Unknown topic");

  nsCOMPtr<nsIRunnable> runnable = new nsJemallocFreeDirtyPagesRunnable();

  NS_DispatchToMainThread(runnable);

  return NS_OK;
}

}  

namespace mozilla {
namespace AvailableMemoryTracker {

void Init() {
  RefPtr watcher = MakeRefPtr<nsMemoryPressureWatcher>();
  watcher->Init();

}

}  
}  
