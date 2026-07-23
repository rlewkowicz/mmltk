/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <gtk/gtk.h>

#include "nsUserIdleServiceGTK.h"
#include "nsDebug.h"
#include "nsITimer.h"
#include "prlink.h"
#include "mozilla/Logging.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "WidgetUtilsGtk.h"
#ifdef MOZ_ENABLE_DBUS
#  include <gio/gio.h>
#  include "AsyncDBus.h"
#  include "WakeLockListener.h"
#  include "nsIObserverService.h"
#endif

using mozilla::LogLevel;
static mozilla::LazyLogModule sIdleLog("nsIUserIdleService");

using namespace mozilla;
using namespace mozilla::widget;


#ifdef MOZ_ENABLE_DBUS
class UserIdleServiceMutter : public UserIdleServiceImpl {
 public:
  bool PollIdleTime(uint32_t* aIdleTime) override {
    MOZ_LOG(sIdleLog, LogLevel::Info, ("PollIdleTime() request\n"));

    if (!mProxy) {
      return false;
    }

    if (!mCacheTimestamp.IsNull()) {
      TimeDuration elapsed = TimeStamp::Now() - mCacheTimestamp;

      if (elapsed < TimeDuration::FromMilliseconds(kCacheFreshMs)) {
        *aIdleTime = mCachedIdleTime;
        MOZ_LOG(sIdleLog, LogLevel::Info,
                ("PollIdleTime() returns cached (fresh) %d\n", *aIdleTime));
        return true;
      }
      if (elapsed < TimeDuration::FromMilliseconds(kCacheStaleMs)) {
        *aIdleTime = mCachedIdleTime;
        MOZ_LOG(sIdleLog, LogLevel::Info,
                ("PollIdleTime() returns cached (stale) %d, refreshing\n",
                 *aIdleTime));
        if (!mPollRequest.Exists()) {
          StartAsyncPoll();
        }
        return true;
      }
      // Cache is very stale, fall through to synchronous wait
    }

    TimeStamp prevCacheTimestamp = mCacheTimestamp;
    if (!mPollRequest.Exists()) {
      StartAsyncPoll();
    }

    MOZ_LOG(sIdleLog, LogLevel::Info,
            ("PollIdleTime() waiting for fresh value\n"));
    SpinEventLoopUntil("UserIdleServiceMutter::PollIdleTime"_ns, [&]() {
      return !mPollRequest.Exists() ||
             AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed);
    });

    if (mCacheTimestamp == prevCacheTimestamp) {
      MOZ_LOG(sIdleLog, LogLevel::Info,
              ("PollIdleTime() returning failure (timeout, async error, or "
               "shutdown)\n"));
      return false;
    }

    *aIdleTime = mCachedIdleTime;
    MOZ_LOG(sIdleLog, LogLevel::Info,
            ("PollIdleTime() returns fresh %d\n", *aIdleTime));
    return true;
  }

 private:
  static void CancelTimer(nsCOMPtr<nsITimer>& aTimer) {
    if (aTimer) {
      aTimer->Cancel();
      aTimer = nullptr;
    }
  }

  void StartAsyncPoll() {
    mPollRequest.DisconnectIfExists();
    mCancellable = dont_AddRef(g_cancellable_new());
    CancelTimer(mPollTimer);
    NS_NewTimerWithCallback(
        getter_AddRefs(mPollTimer),
        [this](nsITimer*) {
          MOZ_LOG(sIdleLog, LogLevel::Warning, ("PollIdleTime() timed out\n"));
          g_cancellable_cancel(mCancellable);
          mPollRequest.DisconnectIfExists();
        },
        TimeDuration::FromMilliseconds(kPollTimeoutMs), nsITimer::TYPE_ONE_SHOT,
        "UserIdleServiceMutter::PollIdleTime"_ns);

    DBusProxyCall(mProxy, "GetIdletime", nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                  mCancellable)
        ->Then(
            GetCurrentSerialEventTarget(), __func__,
            [this](RefPtr<GVariant>&& aResult) {
              mPollRequest.Complete();
              CancelTimer(mPollTimer);
              if (!g_variant_is_of_type(aResult, G_VARIANT_TYPE_TUPLE) ||
                  g_variant_n_children(aResult) != 1) {
                MOZ_LOG(sIdleLog, LogLevel::Info,
                        ("PollIdleTime() Unexpected params type: %s\n",
                         g_variant_get_type_string(aResult)));
                return;
              }
              RefPtr<GVariant> iTime =
                  dont_AddRef(g_variant_get_child_value(aResult, 0));
              if (!g_variant_is_of_type(iTime, G_VARIANT_TYPE_UINT64)) {
                MOZ_LOG(sIdleLog, LogLevel::Info,
                        ("PollIdleTime() Unexpected params type: %s\n",
                         g_variant_get_type_string(aResult)));
                return;
              }
              uint64_t idleTime = g_variant_get_uint64(iTime);
              if (idleTime > std::numeric_limits<uint32_t>::max()) {
                idleTime = std::numeric_limits<uint32_t>::max();
              }
              mCachedIdleTime = idleTime;
              mCacheTimestamp = TimeStamp::Now();
              MOZ_LOG(sIdleLog, LogLevel::Info,
                      ("Async handler got %d, cached\n", mCachedIdleTime));
            },
            [this](GUniquePtr<GError>&& aError) {
              mPollRequest.Complete();
              CancelTimer(mPollTimer);
              if (!IsCancelledGError(aError.get())) {
                MOZ_LOG(
                    sIdleLog, LogLevel::Warning,
                    ("Failed to call GetIdletime(): %s\n", aError->message));
              }
            })
        ->Track(mPollRequest);
  }

 public:
  bool ProbeImplementation() override {
    MOZ_LOG(sIdleLog, LogLevel::Info,
            ("UserIdleServiceMutter::UserIdleServiceMutter()\n"));

    mCancellable = dont_AddRef(g_cancellable_new());
    NS_NewTimerWithCallback(
        getter_AddRefs(mProbeTimer),
        [this](nsITimer*) {
          MOZ_LOG(sIdleLog, LogLevel::Warning,
                  ("ProbeImplementation() timed out\n"));
          g_cancellable_cancel(mCancellable);
          mProbeRequest.DisconnectIfExists();
        },
        TimeDuration::FromMilliseconds(kProbeTimeoutMs),
        nsITimer::TYPE_ONE_SHOT,
        "UserIdleServiceMutter::ProbeImplementation"_ns);

    CreateDBusProxyForBus(
        G_BUS_TYPE_SESSION,
        GDBusProxyFlags(G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS |
                        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES),
        nullptr, "org.gnome.Mutter.IdleMonitor",
        "/org/gnome/Mutter/IdleMonitor/Core", "org.gnome.Mutter.IdleMonitor",
        mCancellable)
        ->Then(
            GetCurrentSerialEventTarget(), __func__,
            [this](RefPtr<GDBusProxy>&& aProxy) {
              mProbeRequest.Complete();
              CancelTimer(mProbeTimer);
              mProxy = std::move(aProxy);
            },
            [this](GUniquePtr<GError>&& aError) {
              mProbeRequest.Complete();
              CancelTimer(mProbeTimer);
              if (!IsCancelledGError(aError.get())) {
                MOZ_LOG(sIdleLog, LogLevel::Warning,
                        ("Failed to create DBus proxy: %s\n", aError->message));
              }
            })
        ->Track(mProbeRequest);

    SpinEventLoopUntil("UserIdleServiceMutter::ProbeImplementation"_ns, [&]() {
      return !mProbeRequest.Exists() ||
             AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed);
    });

    if (mProxy) {
      mUserIdleServiceGTK->AcceptServiceCallback();
    }
    return !!mProxy;
  }

  explicit UserIdleServiceMutter(nsUserIdleServiceGTK* aUserIdleService)
      : UserIdleServiceImpl(aUserIdleService) {};

  ~UserIdleServiceMutter() {
    mProbeRequest.DisconnectIfExists();
    mPollRequest.DisconnectIfExists();
    CancelTimer(mProbeTimer);
    CancelTimer(mPollTimer);
    if (mCancellable) {
      g_cancellable_cancel(mCancellable);
      mCancellable = nullptr;
    }
    mProxy = nullptr;
  }

 private:
  static constexpr uint32_t kCacheFreshMs = 1000;
  static constexpr uint32_t kCacheStaleMs = 5000;
  static constexpr uint32_t kPollTimeoutMs = 3000;
  static constexpr uint32_t kProbeTimeoutMs = 3000;

  RefPtr<GDBusProxy> mProxy;
  RefPtr<GCancellable> mCancellable;
  MozPromiseRequestHolder<DBusProxyPromise> mProbeRequest;
  MozPromiseRequestHolder<DBusCallPromise> mPollRequest;
  nsCOMPtr<nsITimer> mProbeTimer;
  nsCOMPtr<nsITimer> mPollTimer;
  uint32_t mCachedIdleTime = 0;
  TimeStamp mCacheTimestamp;
};
#endif

void nsUserIdleServiceGTK::ProbeService() {
  MOZ_LOG(sIdleLog, LogLevel::Info,
          ("nsUserIdleServiceGTK::ProbeService() mIdleServiceType %d\n",
           mIdleServiceType));
  MOZ_ASSERT(!mIdleService);

  switch (mIdleServiceType) {
#ifdef MOZ_ENABLE_DBUS
    case IDLE_SERVICE_MUTTER:
      mIdleService = MakeUnique<UserIdleServiceMutter>(this);
      break;
#endif
    default:
      return;
  }

  if (!mIdleService->ProbeImplementation()) {
    RejectAndTryNextServiceCallback();
  }
}

void nsUserIdleServiceGTK::AcceptServiceCallback() {
  MOZ_LOG(sIdleLog, LogLevel::Info,
          ("nsUserIdleServiceGTK::AcceptServiceCallback() type %d\n",
           mIdleServiceType));
  mIdleServiceInitialized = true;
}

void nsUserIdleServiceGTK::RejectAndTryNextServiceCallback() {
  MOZ_LOG(sIdleLog, LogLevel::Info,
          ("nsUserIdleServiceGTK::RejectAndTryNextServiceCallback() type %d\n",
           mIdleServiceType));

  MOZ_ASSERT(mIdleService, "Nothing to reject?");
  mIdleService = nullptr;
  mIdleServiceInitialized = false;

  mIdleServiceType++;
  if (mIdleServiceType < IDLE_SERVICE_NONE) {
    MOZ_LOG(sIdleLog, LogLevel::Info,
            ("nsUserIdleServiceGTK try next idle service\n"));
    ProbeService();
  } else {
    MOZ_LOG(sIdleLog, LogLevel::Info, ("nsUserIdleServiceGTK failed\n"));
  }
}

bool nsUserIdleServiceGTK::PollIdleTime(uint32_t* aIdleTime) {
  if (!mIdleServiceInitialized) {
    return false;
  }
  return mIdleService->PollIdleTime(aIdleTime);
}
