/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AntiTrackingLog.h"
#include "ContentBlockingLog.h"

#include "nsIWebProgressListener.h"
#include "nsRFPService.h"
#include "nsTArray.h"
#include "mozilla/StaticPrefs_browser.h"
#include "mozilla/StaticPrefs_privacy.h"

namespace mozilla {

Maybe<uint32_t> ContentBlockingLog::RecordLogParent(
    const nsACString& aOrigin, uint32_t aType, bool aBlocked,
    const Maybe<ContentBlockingNotifier::StorageAccessPermissionGrantedReason>&
        aReason,
    const nsTArray<nsCString>& aTrackingFullHashes,
    const Maybe<CanvasFingerprintingEvent>& aCanvasFingerprintingEvent) {
  MOZ_ASSERT(XRE_IsParentProcess());

  uint32_t events = GetContentBlockingEventsInLog();

  bool blockedValue = aBlocked;
  bool unblocked = false;
  OriginEntry* entry;

  switch (aType) {
    case nsIWebProgressListener::STATE_COOKIES_LOADED:
      MOZ_ASSERT(!aBlocked,
                 "We don't expected to see blocked STATE_COOKIES_LOADED");
      [[fallthrough]];

    case nsIWebProgressListener::STATE_COOKIES_LOADED_TRACKER:
      MOZ_ASSERT(
          !aBlocked,
          "We don't expected to see blocked STATE_COOKIES_LOADED_TRACKER");
      [[fallthrough]];

    case nsIWebProgressListener::STATE_COOKIES_LOADED_SOCIALTRACKER:
      MOZ_ASSERT(!aBlocked,
                 "We don't expected to see blocked "
                 "STATE_COOKIES_LOADED_SOCIALTRACKER");
      blockedValue = !aBlocked;
      [[fallthrough]];

    case nsIWebProgressListener::STATE_BLOCKED_TRACKING_CONTENT:
    case nsIWebProgressListener::STATE_LOADED_LEVEL_1_TRACKING_CONTENT:
    case nsIWebProgressListener::STATE_LOADED_LEVEL_2_TRACKING_CONTENT:
    case nsIWebProgressListener::STATE_BLOCKED_FINGERPRINTING_CONTENT:
    case nsIWebProgressListener::STATE_LOADED_FINGERPRINTING_CONTENT:
    case nsIWebProgressListener::STATE_BLOCKED_CRYPTOMINING_CONTENT:
    case nsIWebProgressListener::STATE_LOADED_CRYPTOMINING_CONTENT:
    case nsIWebProgressListener::STATE_BLOCKED_SOCIALTRACKING_CONTENT:
    case nsIWebProgressListener::STATE_LOADED_SOCIALTRACKING_CONTENT:
    case nsIWebProgressListener::STATE_COOKIES_BLOCKED_BY_PERMISSION:
    case nsIWebProgressListener::STATE_COOKIES_BLOCKED_ALL:
    case nsIWebProgressListener::STATE_COOKIES_BLOCKED_FOREIGN:
    case nsIWebProgressListener::STATE_BLOCKED_EMAILTRACKING_CONTENT:
    case nsIWebProgressListener::STATE_LOADED_EMAILTRACKING_LEVEL_1_CONTENT:
    case nsIWebProgressListener::STATE_LOADED_EMAILTRACKING_LEVEL_2_CONTENT:
    case nsIWebProgressListener::STATE_PURGED_BOUNCETRACKER:
    case nsIWebProgressListener::STATE_COOKIES_PARTITIONED_TRACKER:
      (void)RecordLogInternal(aOrigin, aType, blockedValue);
      break;

    case nsIWebProgressListener::STATE_COOKIES_BLOCKED_TRACKER:
    case nsIWebProgressListener::STATE_COOKIES_BLOCKED_SOCIALTRACKER:
      (void)RecordLogInternal(aOrigin, aType, blockedValue, aReason,
                              aTrackingFullHashes);
      break;

    case nsIWebProgressListener::STATE_REPLACED_FINGERPRINTING_CONTENT:
    case nsIWebProgressListener::STATE_ALLOWED_FINGERPRINTING_CONTENT:
    case nsIWebProgressListener::STATE_REPLACED_TRACKING_CONTENT:
    case nsIWebProgressListener::STATE_ALLOWED_TRACKING_CONTENT:
      (void)RecordLogInternal(aOrigin, aType, blockedValue, aReason,
                              aTrackingFullHashes);
      break;
    case nsIWebProgressListener::STATE_ALLOWED_FONT_FINGERPRINTING:
      MOZ_ASSERT(!aBlocked,
                 "We don't expected to see blocked "
                 "STATE_ALLOWED_FONT_FINGERPRINTING");
      entry = RecordLogInternal(aOrigin, aType, blockedValue);

      aType = nsIWebProgressListener::STATE_BLOCKED_SUSPICIOUS_FINGERPRINTING;

      if (entry && entry->mData->mHasSuspiciousFingerprintingActivity) {
        blockedValue = true;
      }
      break;

    case nsIWebProgressListener::STATE_ALLOWED_CANVAS_FINGERPRINTING:
      MOZ_ASSERT(!aBlocked,
                 "We don't expected to see blocked "
                 "STATE_ALLOWED_CANVAS_FINGERPRINTING");
      entry = RecordLogInternal(aOrigin, aType, blockedValue, Nothing(), {},
                                aCanvasFingerprintingEvent);

      aType = nsIWebProgressListener::STATE_BLOCKED_SUSPICIOUS_FINGERPRINTING;

      if (entry && entry->mData->mHasSuspiciousFingerprintingActivity) {
        blockedValue = true;
      }
      break;

    default:
      break;
  }

  if (!aBlocked) {
    unblocked = (events & aType) != 0;
  }

  const uint32_t oldEvents = events;
  if (blockedValue) {
    events |= aType;
  } else if (unblocked) {
    events &= ~aType;
  }

  if (events == oldEvents
  ) {
    return Nothing();
  }

  return Some(events);
}


ContentBlockingLog::OriginEntry* ContentBlockingLog::RecordLogInternal(
    const nsACString& aOrigin, uint32_t aType, bool aBlocked,
    const Maybe<ContentBlockingNotifier::StorageAccessPermissionGrantedReason>&
        aReason,
    const nsTArray<nsCString>& aTrackingFullHashes,
    const Maybe<CanvasFingerprintingEvent>& aCanvasFingerprintingEvent) {
  DebugOnly<bool> isCookiesBlockedTracker =
      aType == nsIWebProgressListener::STATE_COOKIES_BLOCKED_TRACKER ||
      aType == nsIWebProgressListener::STATE_COOKIES_BLOCKED_SOCIALTRACKER;
  MOZ_ASSERT_IF(aBlocked, aReason.isNothing());
  MOZ_ASSERT_IF(!isCookiesBlockedTracker, aReason.isNothing());
  MOZ_ASSERT_IF(isCookiesBlockedTracker && !aBlocked, aReason.isSome());

  if (aOrigin.IsVoid()) {
    return nullptr;
  }
  auto index = mLog.IndexOf(aOrigin, 0, Comparator());
  if (index != OriginDataTable::NoIndex) {
    OriginEntry& entry = mLog[index];
    if (!entry.mData) {
      return nullptr;
    }

    if (RecordLogEntryInCustomField(aType, entry, aBlocked)) {
      return &entry;
    }
    if (!entry.mData->mLogs.IsEmpty()) {
      auto& last = entry.mData->mLogs.LastElement();
      if (last.mType == aType && last.mBlocked == aBlocked &&
          last.mCanvasFingerprintingEvent == aCanvasFingerprintingEvent) {
        ++last.mRepeatCount;
        for (const auto& hash : aTrackingFullHashes) {
          if (!last.mTrackingFullHashes.Contains(hash)) {
            last.mTrackingFullHashes.AppendElement(hash);
          }
        }
        return &entry;
      }
    }
    if (entry.mData->mLogs.Length() ==
        std::max(1u, StaticPrefs::browser_contentblocking_originlog_length())) {
      entry.mData->mLogs.RemoveElementAt(0);
    }
    entry.mData->mLogs.AppendElement(LogEntry{aType, 1u, aBlocked, aReason,
                                              aTrackingFullHashes.Clone(),
                                              aCanvasFingerprintingEvent});

    if (aType == nsIWebProgressListener::STATE_ALLOWED_CANVAS_FINGERPRINTING ||
        aType == nsIWebProgressListener::STATE_ALLOWED_FONT_FINGERPRINTING) {
      entry.mData->mHasSuspiciousFingerprintingActivity = true;
    }
    return &entry;
  }

  OriginEntry* entry = mLog.AppendElement();
  if (NS_WARN_IF(!entry || !entry->mData)) {
    return nullptr;
  }

  entry->mOrigin = aOrigin;

  if (aType == nsIWebProgressListener::STATE_LOADED_LEVEL_1_TRACKING_CONTENT) {
    entry->mData->mHasLevel1TrackingContentLoaded = aBlocked;
  } else if (aType ==
             nsIWebProgressListener::STATE_LOADED_LEVEL_2_TRACKING_CONTENT) {
    entry->mData->mHasLevel2TrackingContentLoaded = aBlocked;
  } else if (aType == nsIWebProgressListener::STATE_COOKIES_LOADED) {
    MOZ_ASSERT(entry->mData->mHasCookiesLoaded.isNothing());
    entry->mData->mHasCookiesLoaded.emplace(aBlocked);
  } else if (aType == nsIWebProgressListener::STATE_COOKIES_LOADED_TRACKER) {
    MOZ_ASSERT(entry->mData->mHasTrackerCookiesLoaded.isNothing());
    entry->mData->mHasTrackerCookiesLoaded.emplace(aBlocked);
  } else if (aType ==
             nsIWebProgressListener::STATE_COOKIES_LOADED_SOCIALTRACKER) {
    MOZ_ASSERT(entry->mData->mHasSocialTrackerCookiesLoaded.isNothing());
    entry->mData->mHasSocialTrackerCookiesLoaded.emplace(aBlocked);
  } else {
    entry->mData->mLogs.AppendElement(LogEntry{aType, 1u, aBlocked, aReason,
                                               aTrackingFullHashes.Clone(),
                                               aCanvasFingerprintingEvent});

    if (aType == nsIWebProgressListener::STATE_ALLOWED_CANVAS_FINGERPRINTING ||
        aType == nsIWebProgressListener::STATE_ALLOWED_FONT_FINGERPRINTING) {
      entry->mData->mHasSuspiciousFingerprintingActivity = true;
    }
  }

  return entry;
}

}  
