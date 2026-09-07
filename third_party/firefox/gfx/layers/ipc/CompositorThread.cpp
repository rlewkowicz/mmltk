/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "CompositorThread.h"

#include "CompositorBridgeParent.h"
#include "gfxGradientCache.h"
#include "MainThreadUtils.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/layers/CompositorManagerParent.h"
#include "mozilla/layers/ImageBridgeParent.h"
#include "mozilla/layers/VideoBridgeParent.h"
#include "nsThreadUtils.h"

namespace mozilla {
namespace layers {

static StaticRefPtr<CompositorThreadHolder> sCompositorThreadHolder;
static Atomic<bool> sFinishedCompositorShutDown(false);
static PlatformThreadId sCompositorThreadId;

nsIThread* CompositorThread() {
  return sCompositorThreadHolder
             ? sCompositorThreadHolder->GetCompositorThread()
             : nullptr;
}

CompositorThreadHolder* CompositorThreadHolder::GetSingleton() {
  return sCompositorThreadHolder;
}

CompositorThreadHolder::CompositorThreadHolder()
    : mCompositorThread(CreateCompositorThread()) {
  MOZ_ASSERT(NS_IsMainThread());
}

CompositorThreadHolder::~CompositorThreadHolder() {
  sFinishedCompositorShutDown = true;
}

 already_AddRefed<nsIThread>
CompositorThreadHolder::CreateCompositorThread() {
  MOZ_ASSERT(NS_IsMainThread());

  MOZ_ASSERT(!sCompositorThreadHolder,
             "The compositor thread has already been started!");

  uint32_t stackSize = nsIThreadManager::DEFAULT_STACK_SIZE;
  if (stackSize) {
    stackSize =
        std::max(stackSize, gfx::gfxVars::SupportsThreadsafeGL() &&
                                    !gfx::gfxVars::UseCanvasRenderThread()
                                ? 4096U << 10
                                : 512U << 10);
  }

  nsCOMPtr<nsIThread> compositorThread;
  nsresult rv = NS_NewNamedThread(
      "Compositor", getter_AddRefs(compositorThread),
      NS_NewRunnableFunction(
          "CompositorThreadHolder::CompositorThreadHolderSetup",
          []() { sCompositorThreadId = PlatformThread::CurrentId(); }),
      {.stackSize = stackSize});

  if (NS_FAILED(rv)) {
    return nullptr;
  }

  ImageBridgeParent::Setup();

  return compositorThread.forget();
}

void CompositorThreadHolder::Start() {
  MOZ_ASSERT(NS_IsMainThread(), "Should be on the main Thread!");
  MOZ_ASSERT(!sCompositorThreadHolder,
             "The compositor thread has already been started!");

  sCompositorThreadId = 0;
  sCompositorThreadHolder = new CompositorThreadHolder();
  if (!sCompositorThreadHolder->GetCompositorThread()) {
    gfxCriticalNote << "Compositor thread not started ("
                    << XRE_IsParentProcess() << ")";
    sCompositorThreadHolder = nullptr;
  }
}

void CompositorThreadHolder::Shutdown() {
  MOZ_ASSERT(NS_IsMainThread(), "Should be on the main Thread!");
  if (!sCompositorThreadHolder) {
    return;
  }

  ImageBridgeParent::Shutdown();
  CompositorManagerParent::Shutdown();
  gfx::gfxGradientCache::Shutdown();

  CompositorThread()->Dispatch(NS_NewRunnableFunction(
      "CompositorThreadHolder::Shutdown",
      [compositorThreadHolder = RefPtr<CompositorThreadHolder>(
           sCompositorThreadHolder)]() {
        VideoBridgeParent::UnregisterExternalImages();
      }));

  sCompositorThreadHolder = nullptr;

  SpinEventLoopUntil("CompositorThreadHolder::Shutdown"_ns, [&]() {
    bool finished = sFinishedCompositorShutDown;
    return finished;
  });

  CompositorBridgeParent::FinishShutdown();
}

bool CompositorThreadHolder::IsInCompositorThread() {
  if (!CompositorThread()) {
    return sCompositorThreadId == PlatformThread::CurrentId();
  }
  bool in = false;
  MOZ_ALWAYS_SUCCEEDS(CompositorThread()->IsOnCurrentThread(&in));
  return in;
}

}  
}  

bool NS_IsInCompositorThread() {
  return mozilla::layers::CompositorThreadHolder::IsInCompositorThread();
}
