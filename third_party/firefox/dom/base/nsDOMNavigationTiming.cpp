/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsDOMNavigationTiming.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/Document.h"
#include "mozilla/ipc/URIUtils.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsDocShell.h"
#include "nsHttp.h"
#include "nsIScriptSecurityManager.h"
#include "nsIURI.h"
#include "nsPrintfCString.h"
#include "prtime.h"

using namespace mozilla;

namespace mozilla {

LazyLogModule gPageLoadLog("PageLoad");
#define PAGELOAD_LOG(args) MOZ_LOG(gPageLoadLog, LogLevel::Debug, args)
#define PAGELOAD_LOG_ENABLED() MOZ_LOG_TEST(gPageLoadLog, LogLevel::Error)

}  

nsDOMNavigationTiming::nsDOMNavigationTiming(nsDocShell* aDocShell) {
  Clear();

  mDocShell = aDocShell;
}

nsDOMNavigationTiming::~nsDOMNavigationTiming() = default;

void nsDOMNavigationTiming::Clear() {
  mNavigationType = TYPE_RESERVED;
  mNavigationStartHighRes = 0;

  mBeforeUnloadStart = TimeStamp();
  mUnloadStart = TimeStamp();
  mUnloadEnd = TimeStamp();
  mLoadEventStart = TimeStamp();
  mLoadEventEnd = TimeStamp();
  mDOMLoading = TimeStamp();
  mDOMInteractive = TimeStamp();
  mDOMContentLoadedEventStart = TimeStamp();
  mDOMContentLoadedEventEnd = TimeStamp();
  mDOMComplete = TimeStamp();
  mContentfulComposite = TimeStamp();
  mNonBlankPaint = TimeStamp();
  mLargestContentfulRender = TimeStamp();
  mDocShellHasBeenActiveSinceNavigationStart = false;
}

void nsDOMNavigationTiming::Anonymize(nsIURI* aFinalURI) {
  mLoadedURI = aFinalURI;
  mUnloadedURI = nullptr;
  mBeforeUnloadStart = TimeStamp();
  mUnloadStart = TimeStamp();
  mUnloadEnd = TimeStamp();
}

DOMTimeMilliSec nsDOMNavigationTiming::TimeStampToDOM(TimeStamp aStamp) const {
  if (aStamp.IsNull()) {
    return 0;
  }

  TimeDuration duration = aStamp - mNavigationStart;
  return GetNavigationStart() + static_cast<int64_t>(duration.ToMilliseconds());
}

void nsDOMNavigationTiming::NotifyNavigationStart(
    DocShellState aDocShellState) {
  mNavigationStartHighRes = (double)PR_Now() / PR_USEC_PER_MSEC;
  mNavigationStart = TimeStamp::Now();
  mDocShellHasBeenActiveSinceNavigationStart =
      (aDocShellState == DocShellState::eActive);
}

void nsDOMNavigationTiming::NotifyFetchStart(nsIURI* aURI,
                                             Type aNavigationType) {
  mNavigationType = aNavigationType;
  mLoadedURI = aURI;
}

void nsDOMNavigationTiming::NotifyRestoreStart() {
  mNavigationType = TYPE_BACK_FORWARD;
}

void nsDOMNavigationTiming::NotifyBeforeUnload() {
  mBeforeUnloadStart = TimeStamp::Now();
}

void nsDOMNavigationTiming::NotifyUnloadAccepted(nsIURI* aOldURI) {
  mUnloadStart = mBeforeUnloadStart;
  mUnloadedURI = aOldURI;
}

void nsDOMNavigationTiming::NotifyUnloadEventStart() {
  mUnloadStart = TimeStamp::Now();
}

void nsDOMNavigationTiming::NotifyUnloadEventEnd() {
  mUnloadEnd = TimeStamp::Now();
}

void nsDOMNavigationTiming::NotifyLoadEventStart() {
  if (!mLoadEventStart.IsNull()) {
    return;
  }
  mLoadEventStart = TimeStamp::Now();


}

void nsDOMNavigationTiming::NotifyLoadEventEnd() {
  if (!mLoadEventEnd.IsNull()) {
    return;
  }
  mLoadEventEnd = TimeStamp::Now();


  if (IsTopLevelContentDocumentInContentProcess()) {
    if (PAGELOAD_LOG_ENABLED()) {
      TimeDuration elapsed = mLoadEventEnd - mNavigationStart;
      TimeDuration duration = mLoadEventEnd - mLoadEventStart;
      nsPrintfCString marker(
          "Document %s loaded after %dms, load event duration %dms",
          nsContentUtils::TruncatedURLForDisplay(mLoadedURI).get(),
          int(elapsed.ToMilliseconds()), int(duration.ToMilliseconds()));
      PAGELOAD_LOG(("%s", marker.get()));
    }

  }
}

void nsDOMNavigationTiming::SetDOMLoadingTimeStamp(nsIURI* aURI,
                                                   TimeStamp aValue) {
  if (!mDOMLoading.IsNull()) {
    return;
  }
  mLoadedURI = aURI;
  mDOMLoading = aValue;
}

void nsDOMNavigationTiming::NotifyDOMLoading(nsIURI* aURI) {
  if (!mDOMLoading.IsNull()) {
    return;
  }
  mLoadedURI = aURI;
  mDOMLoading = TimeStamp::Now();

}

void nsDOMNavigationTiming::NotifyDOMInteractive(nsIURI* aURI) {
  if (!mDOMInteractive.IsNull()) {
    return;
  }
  mLoadedURI = aURI;
  mDOMInteractive = TimeStamp::Now();

}

void nsDOMNavigationTiming::NotifyDOMComplete(nsIURI* aURI) {
  if (!mDOMComplete.IsNull()) {
    return;
  }
  mLoadedURI = aURI;
  mDOMComplete = TimeStamp::Now();

}

void nsDOMNavigationTiming::NotifyDOMContentLoadedStart(nsIURI* aURI) {
  if (!mDOMContentLoadedEventStart.IsNull()) {
    return;
  }

  mLoadedURI = aURI;
  mDOMContentLoadedEventStart = TimeStamp::Now();


}

void nsDOMNavigationTiming::NotifyDOMContentLoadedEnd(nsIURI* aURI) {
  if (!mDOMContentLoadedEventEnd.IsNull()) {
    return;
  }

  mLoadedURI = aURI;
  mDOMContentLoadedEventEnd = TimeStamp::Now();


}

void nsDOMNavigationTiming::TTITimeoutCallback(nsITimer* aTimer,
                                               void* aClosure) {
  nsDOMNavigationTiming* self = static_cast<nsDOMNavigationTiming*>(aClosure);
  self->TTITimeout(aTimer);
}

#define TTI_WINDOW_SIZE_MS (5 * 1000)

void nsDOMNavigationTiming::TTITimeout(nsITimer* aTimer) {
  TimeStamp now = TimeStamp::Now();
  MOZ_RELEASE_ASSERT(!mContentfulComposite.IsNull(),
                     "TTI timeout with no contentful-composite?");

  nsCOMPtr<nsIThread> mainThread = do_GetMainThread();
  TimeStamp lastLongTaskEnded;
  mainThread->GetLastLongNonIdleTaskEnd(&lastLongTaskEnded);
  if (lastLongTaskEnded.IsNull() || lastLongTaskEnded < mContentfulComposite) {
    PAGELOAD_LOG(
        ("no longtask (last was %g ms before ContentfulComposite)",
         lastLongTaskEnded.IsNull()
             ? 0
             : (mContentfulComposite - lastLongTaskEnded).ToMilliseconds()));
    lastLongTaskEnded = mContentfulComposite;
  }
  TimeDuration delta = now - lastLongTaskEnded;
  PAGELOAD_LOG(("TTI delta: %g ms", delta.ToMilliseconds()));
  if (delta.ToMilliseconds() < TTI_WINDOW_SIZE_MS) {
    PAGELOAD_LOG(("TTI: waiting additional %g ms",
                  (TTI_WINDOW_SIZE_MS + 100) - delta.ToMilliseconds()));
    aTimer->InitWithNamedFuncCallback(
        TTITimeoutCallback, this,
        (TTI_WINDOW_SIZE_MS + 100) -
            delta.ToMilliseconds(),  
        nsITimer::TYPE_ONE_SHOT_LOW_PRIORITY,
        "nsDOMNavigationTiming::TTITimeout"_ns);
    return;
  }




  if (mTTFI.IsNull()) {
    mTTFI = (mDOMContentLoadedEventEnd.IsNull() ||
             lastLongTaskEnded > mDOMContentLoadedEventEnd)
                ? lastLongTaskEnded
                : mDOMContentLoadedEventEnd;
    PAGELOAD_LOG(
        ("TTFI after %dms (LongTask was at %dms, DCL was %dms)",
         int((mTTFI - mNavigationStart).ToMilliseconds()),
         lastLongTaskEnded.IsNull()
             ? 0
             : int((lastLongTaskEnded - mNavigationStart).ToMilliseconds()),
         mDOMContentLoadedEventEnd.IsNull()
             ? 0
             : int((mDOMContentLoadedEventEnd - mNavigationStart)
                       .ToMilliseconds())));
  }

  mTTITimer = nullptr;

  if (PAGELOAD_LOG_ENABLED()) {
    TimeDuration elapsed = mTTFI - mNavigationStart;
    MOZ_ASSERT(elapsed.ToMilliseconds() > 0);
    TimeDuration elapsedLongTask =
        lastLongTaskEnded.IsNull()
            ? TimeDuration()
            : TimeDuration(lastLongTaskEnded - mNavigationStart);
    nsPrintfCString marker(
        "TTFI after %dms (LongTask was at %dms) for URL %s",
        int(elapsed.ToMilliseconds()), int(elapsedLongTask.ToMilliseconds()),
        nsContentUtils::TruncatedURLForDisplay(mLoadedURI).get());

  }
}

void nsDOMNavigationTiming::NotifyNonBlankPaintForRootContentDocument() {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mNavigationStart.IsNull());

  if (!mNonBlankPaint.IsNull()) {
    return;
  }

  mNonBlankPaint = TimeStamp::Now();

  if (PAGELOAD_LOG_ENABLED()) {
    TimeDuration elapsed = mNonBlankPaint - mNavigationStart;
    nsPrintfCString marker(
        "Non-blank paint after %dms for URL %s, %s",
        int(elapsed.ToMilliseconds()),
        nsContentUtils::TruncatedURLForDisplay(mLoadedURI).get(),
        mDocShellHasBeenActiveSinceNavigationStart
            ? "foreground tab"
            : "this tab was inactive some of the time between navigation start "
              "and first non-blank paint");
    PAGELOAD_LOG(("%s", marker.get()));
  }

  if (mDocShellHasBeenActiveSinceNavigationStart) {

  }
}

void nsDOMNavigationTiming::NotifyContentfulCompositeForRootContentDocument(
    const mozilla::TimeStamp& aCompositeEndTime) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mNavigationStart.IsNull());

  if (!mContentfulComposite.IsNull()) {
    return;
  }

  mContentfulComposite = aCompositeEndTime;

  if (PAGELOAD_LOG_ENABLED()) {
    TimeDuration elapsed = mContentfulComposite - mNavigationStart;
    nsPrintfCString marker(
        "Contentful composite after %dms for URL %s, %s",
        int(elapsed.ToMilliseconds()),
        nsContentUtils::TruncatedURLForDisplay(mLoadedURI).get(),
        mDocShellHasBeenActiveSinceNavigationStart
            ? "foreground tab"
            : "this tab was inactive some of the time between navigation start "
              "and first non-blank paint");
    PAGELOAD_LOG(("%s", marker.get()));
  }

  if (!mTTITimer) {
    mTTITimer = NS_NewTimer();
  }

  mTTITimer->InitWithNamedFuncCallback(TTITimeoutCallback, this,
                                       TTI_WINDOW_SIZE_MS,
                                       nsITimer::TYPE_ONE_SHOT_LOW_PRIORITY,
                                       "nsDOMNavigationTiming::TTITimeout"_ns);

  if (mDocShellHasBeenActiveSinceNavigationStart) {

  }
}

void nsDOMNavigationTiming::NotifyLargestContentfulRenderForRootContentDocument(
    const DOMHighResTimeStamp& aRenderTime) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mNavigationStart.IsNull());

  mLargestContentfulRender =
      mNavigationStart + TimeDuration::FromMilliseconds(aRenderTime);
}

void nsDOMNavigationTiming::NotifyDocShellStateChanged(
    DocShellState aDocShellState) {
  mDocShellHasBeenActiveSinceNavigationStart &=
      (aDocShellState == DocShellState::eActive);
}

mozilla::TimeStamp nsDOMNavigationTiming::GetUnloadEventStartTimeStamp() const {
  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();
  nsresult rv = ssm->CheckSameOriginURI(mLoadedURI, mUnloadedURI, false, false);
  if (NS_SUCCEEDED(rv)) {
    return mUnloadStart;
  }
  return mozilla::TimeStamp();
}

mozilla::TimeStamp nsDOMNavigationTiming::GetUnloadEventEndTimeStamp() const {
  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();
  nsresult rv = ssm->CheckSameOriginURI(mLoadedURI, mUnloadedURI, false, false);
  if (NS_SUCCEEDED(rv)) {
    return mUnloadEnd;
  }
  return mozilla::TimeStamp();
}

bool nsDOMNavigationTiming::IsTopLevelContentDocumentInContentProcess() const {
  if (!mDocShell) {
    return false;
  }
  if (!XRE_IsContentProcess()) {
    return false;
  }
  return mDocShell->GetBrowsingContext()->IsTopContent();
}

nsDOMNavigationTiming::nsDOMNavigationTiming(nsDocShell* aDocShell,
                                             nsDOMNavigationTiming* aOther)
    : mDocShell(aDocShell),
      mUnloadedURI(aOther->mUnloadedURI),
      mLoadedURI(aOther->mLoadedURI),
      mNavigationType(aOther->mNavigationType),
      mNavigationStartHighRes(aOther->mNavigationStartHighRes),
      mNavigationStart(aOther->mNavigationStart),
      mNonBlankPaint(aOther->mNonBlankPaint),
      mContentfulComposite(aOther->mContentfulComposite),
      mLargestContentfulRender(aOther->mLargestContentfulRender),
      mBeforeUnloadStart(aOther->mBeforeUnloadStart),
      mUnloadStart(aOther->mUnloadStart),
      mUnloadEnd(aOther->mUnloadEnd),
      mLoadEventStart(aOther->mLoadEventStart),
      mLoadEventEnd(aOther->mLoadEventEnd),
      mDOMLoading(aOther->mDOMLoading),
      mDOMInteractive(aOther->mDOMInteractive),
      mDOMContentLoadedEventStart(aOther->mDOMContentLoadedEventStart),
      mDOMContentLoadedEventEnd(aOther->mDOMContentLoadedEventEnd),
      mDOMComplete(aOther->mDOMComplete),
      mTTFI(aOther->mTTFI),
      mDocShellHasBeenActiveSinceNavigationStart(
          aOther->mDocShellHasBeenActiveSinceNavigationStart) {}

void IPC::ParamTraits<nsDOMNavigationTiming*>::Write(
    MessageWriter* aWriter, nsDOMNavigationTiming* aParam) {
  bool isNull = !aParam;
  WriteParam(aWriter, isNull);
  if (isNull) {
    return;
  }

  RefPtr<nsIURI> unloadedURI = aParam->mUnloadedURI.get();
  RefPtr<nsIURI> loadedURI = aParam->mLoadedURI.get();
  WriteParam(aWriter, unloadedURI ? Some(unloadedURI) : Nothing());
  WriteParam(aWriter, loadedURI ? Some(loadedURI) : Nothing());
  WriteParam(aWriter, uint32_t(aParam->mNavigationType));
  WriteParam(aWriter, aParam->mNavigationStartHighRes);
  WriteParam(aWriter, aParam->mNavigationStart);
  WriteParam(aWriter, aParam->mNonBlankPaint);
  WriteParam(aWriter, aParam->mContentfulComposite);
  WriteParam(aWriter, aParam->mBeforeUnloadStart);
  WriteParam(aWriter, aParam->mUnloadStart);
  WriteParam(aWriter, aParam->mUnloadEnd);
  WriteParam(aWriter, aParam->mLoadEventStart);
  WriteParam(aWriter, aParam->mLoadEventEnd);
  WriteParam(aWriter, aParam->mDOMLoading);
  WriteParam(aWriter, aParam->mDOMInteractive);
  WriteParam(aWriter, aParam->mDOMContentLoadedEventStart);
  WriteParam(aWriter, aParam->mDOMContentLoadedEventEnd);
  WriteParam(aWriter, aParam->mDOMComplete);
  WriteParam(aWriter, aParam->mTTFI);
  WriteParam(aWriter, aParam->mDocShellHasBeenActiveSinceNavigationStart);
}

bool IPC::ParamTraits<nsDOMNavigationTiming*>::Read(
    IPC::MessageReader* aReader, RefPtr<nsDOMNavigationTiming>* aResult) {
  bool isNull;
  if (!ReadParam(aReader, &isNull)) {
    return false;
  }
  if (isNull) {
    *aResult = nullptr;
    return true;
  }

  auto timing = MakeRefPtr<nsDOMNavigationTiming>(nullptr);
  uint32_t type;
  Maybe<RefPtr<nsIURI>> unloadedURI;
  Maybe<RefPtr<nsIURI>> loadedURI;
  if (!ReadParam(aReader, &unloadedURI) || !ReadParam(aReader, &loadedURI) ||
      !ReadParam(aReader, &type) ||
      !ReadParam(aReader, &timing->mNavigationStartHighRes) ||
      !ReadParam(aReader, &timing->mNavigationStart) ||
      !ReadParam(aReader, &timing->mNonBlankPaint) ||
      !ReadParam(aReader, &timing->mContentfulComposite) ||
      !ReadParam(aReader, &timing->mBeforeUnloadStart) ||
      !ReadParam(aReader, &timing->mUnloadStart) ||
      !ReadParam(aReader, &timing->mUnloadEnd) ||
      !ReadParam(aReader, &timing->mLoadEventStart) ||
      !ReadParam(aReader, &timing->mLoadEventEnd) ||
      !ReadParam(aReader, &timing->mDOMLoading) ||
      !ReadParam(aReader, &timing->mDOMInteractive) ||
      !ReadParam(aReader, &timing->mDOMContentLoadedEventStart) ||
      !ReadParam(aReader, &timing->mDOMContentLoadedEventEnd) ||
      !ReadParam(aReader, &timing->mDOMComplete) ||
      !ReadParam(aReader, &timing->mTTFI) ||
      !ReadParam(aReader,
                 &timing->mDocShellHasBeenActiveSinceNavigationStart)) {
    return false;
  }
  timing->mNavigationType = nsDOMNavigationTiming::Type(type);
  if (unloadedURI) {
    timing->mUnloadedURI = std::move(*unloadedURI);
  }
  if (loadedURI) {
    timing->mLoadedURI = std::move(*loadedURI);
  }
  *aResult = std::move(timing);
  return true;
}
