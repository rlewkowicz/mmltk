/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_HttpBackgroundChannelParent_h
#define mozilla_net_HttpBackgroundChannelParent_h

#include "mozilla/net/PHttpBackgroundChannelParent.h"
#include "mozilla/Atomics.h"
#include "mozilla/Mutex.h"
#include "mozilla/dom/ipc/IdType.h"
#include "nsID.h"
#include "nsISupportsImpl.h"

class nsISerialEventTarget;

namespace mozilla {
namespace net {

class HttpChannelParent;

class HttpBackgroundChannelParent final : public PHttpBackgroundChannelParent {
 public:
  explicit HttpBackgroundChannelParent();

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HttpBackgroundChannelParent, final)

  nsresult Init(const dom::ContentParentId& aCpId, const uint64_t& aChannelId);

  dom::ContentParentId GetContentParentId() const { return mContentParentId; }

  void LinkToChannel(HttpChannelParent* aChannelParent);

  void OnChannelClosed();

  bool OnStartRequest(nsHttpResponseHead&& aResponseHead,
                      const bool& aUseResponseHead,
                      const nsHttpHeaderArray& aRequestHeaders,
                      const HttpChannelOnStartRequestArgs& aArgs,
                      const nsCOMPtr<nsICacheEntry>& aCacheEntry,
                      TimeStamp aOnStartRequestStart);

  bool OnTransportAndData(const nsresult& aChannelStatus,
                          const nsresult& aTransportStatus,
                          const uint64_t& aOffset, const uint32_t& aCount,
                          const nsCString& aData,
                          TimeStamp aOnDataAvailableStart);

  bool OnStopRequest(const nsresult& aChannelStatus,
                     const ResourceTimingStructArgs& aTiming,
                     const nsHttpHeaderArray& aResponseTrailers,
                     const nsTArray<ConsoleReportCollected>& aConsoleReports,
                     TimeStamp aOnStopRequestStart);

  bool OnConsoleReport(const nsTArray<ConsoleReportCollected>& aConsoleReports);

  bool OnAfterLastPart(const nsresult aStatus);

  bool OnProgress(const int64_t aProgress, const int64_t aProgressMax);

  bool OnStatus(const nsresult aStatus);

  bool OnDiversion();

  nsISerialEventTarget* GetBackgroundTarget();

 protected:
  void ActorDestroy(ActorDestroyReason aWhy) override;

 private:
  virtual ~HttpBackgroundChannelParent();

  Atomic<bool> mIPCOpened;

  Mutex mBgThreadMutex;

  nsCOMPtr<nsISerialEventTarget> mBackgroundThread
      MOZ_GUARDED_BY(mBgThreadMutex);

  dom::ContentParentId mContentParentId;

  RefPtr<HttpChannelParent> mChannelParent;
};

}  
}  

#endif  // mozilla_net_HttpBackgroundChannelParent_h
