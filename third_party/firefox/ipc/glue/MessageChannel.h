/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(ipc_glue_MessageChannel_h)
#define ipc_glue_MessageChannel_h

#include "ipc/EnumSerializer.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Monitor.h"
#include "mozilla/MoveOnlyFunction.h"

#include <functional>

#include "MessageLink.h"  // for HasResultCodes
#include "mozilla/ipc/ScopedPort.h"
#include "nsITargetShutdownTask.h"


class MessageLoop;

namespace IPC {
template <typename T>
struct ParamTraits;
}

namespace mozilla {
namespace ipc {

class IToplevelProtocol;
class ActorLifecycleProxy;

class RefCountedMonitor : public Monitor {
 public:
  RefCountedMonitor() : Monitor("mozilla.ipc.MessageChannel.mMonitor") {}

  void AssertSameMonitor(const RefCountedMonitor& aOther) const
      MOZ_REQUIRES(*this) MOZ_ASSERT_CAPABILITY(aOther) {
    MOZ_ASSERT(this == &aOther);
  }

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(RefCountedMonitor)

 private:
  ~RefCountedMonitor() = default;
};

enum class MessageDirection {
  eSending,
  eReceiving,
};

enum class SyncSendError {
  SendSuccess,
  PreviousTimeout,
  SendingCPOWWhileDispatchingSync,
  SendingCPOWWhileDispatchingUrgent,
  NotConnectedBeforeSend,
  DisconnectedDuringSend,
  CancelledBeforeSend,
  CancelledAfterSend,
  TimedOut,
  ReplyError,
};

enum class ResponseRejectReason {
  SendError,
  ChannelClosed,
  HandlerRejected,
  ActorDestroyed,
  ResolverDestroyed,
  EndGuard_,
};

template <typename T>
using ResolveCallback = MoveOnlyFunction<void(T&&)>;

using RejectCallback = MoveOnlyFunction<void(ResponseRejectReason)>;

enum ChannelState {
  ChannelClosed,
  ChannelConnected,
  ChannelClosing,
  ChannelError
};

class AutoEnterTransaction;

class MessageChannel : HasResultCodes {
  friend class PortLink;

  typedef mozilla::Monitor Monitor;

 public:
  using Message = IPC::Message;
  using seqno_t = Message::seqno_t;

  static constexpr int32_t kNoTimeout = INT32_MIN;

  using ScopedPort = mozilla::ipc::ScopedPort;

  explicit MessageChannel(const char* aName, IToplevelProtocol* aListener);
  ~MessageChannel();

  IToplevelProtocol* Listener() const { return mListener; }

  nsISerialEventTarget* GetWorkerEventTarget() const { return mWorkerThread; }

  bool Open(ScopedPort aPort, Side aSide, const nsID& aMessageChannelId,
            nsISerialEventTarget* aEventTarget = nullptr);

  bool Open(MessageChannel* aTargetChan, nsISerialEventTarget* aEventTarget,
            Side aSide);

  bool OpenOnSameThread(MessageChannel* aTargetChan, Side aSide);

  void NotifyImpendingShutdown() MOZ_EXCLUDES(*mMonitor);

  void Close() MOZ_EXCLUDES(*mMonitor);

  void InduceConnectionError() MOZ_EXCLUDES(*mMonitor);

  void SetAbortOnError(bool abort) MOZ_EXCLUDES(*mMonitor) {
    MonitorAutoLock lock(*mMonitor);
    mAbortOnError = abort;
  }

  void PeekMessages(const std::function<bool(const Message& aMsg)>& aInvoke)
      MOZ_EXCLUDES(*mMonitor);

  enum ChannelFlags {
    REQUIRE_DEFAULT = 0,
    REQUIRE_DEFERRED_MESSAGE_PROTECTION = 1 << 0,
  };
  void SetChannelFlags(ChannelFlags aFlags) { mFlags = aFlags; }
  ChannelFlags GetChannelFlags() { return mFlags; }

  bool Send(UniquePtr<Message> aMsg, seqno_t* aSeqno = nullptr)
      MOZ_EXCLUDES(*mMonitor);

  bool SendBuildIDsMatchMessage(const char* aParentBuildID)
      MOZ_EXCLUDES(*mMonitor);
  bool DoBuildIDsMatch() MOZ_EXCLUDES(*mMonitor) {
    MonitorAutoLock lock(*mMonitor);
    return mBuildIDsConfirmedMatch;
  }

  bool Send(UniquePtr<Message> aMsg, UniquePtr<Message>* aReply)
      MOZ_EXCLUDES(*mMonitor);

  bool CanSend() const MOZ_EXCLUDES(*mMonitor);

  SyncSendError LastSendError() const {
    AssertWorkerThread();
    return mLastSendError;
  }

  void SetReplyTimeoutMs(int32_t aTimeoutMs);

  bool IsOnCxxStack() const { return mOnCxxStack; }

  void CancelCurrentTransaction() MOZ_EXCLUDES(*mMonitor);

  bool TestOnlyIsTransactionComplete() const MOZ_EXCLUDES(*mMonitor);

  bool IsClosed() MOZ_EXCLUDES(*mMonitor) {
    MonitorAutoLock lock(*mMonitor);
    return IsClosedLocked();
  }
  bool IsClosedLocked() const MOZ_REQUIRES(*mMonitor) {
    mMonitor->AssertCurrentThreadOwns();
    return mLink ? mLink->IsClosed() : true;
  }

  static bool IsPumpingMessages() { return sIsPumpingMessages; }
  static void SetIsPumpingMessages(bool aIsPumping) {
    sIsPumpingMessages = aIsPumping;
  }

  bool IsCrossProcess() const MOZ_REQUIRES(*mMonitor);
  void SetIsCrossProcess(bool aIsCrossProcess) MOZ_REQUIRES(*mMonitor);

  nsID GetMessageChannelId() const {
    MonitorAutoLock lock(*mMonitor);
    return mMessageChannelId;
  }

  struct MOZ_RAII ErrorNotifyBatcher {
    ErrorNotifyBatcher();
    ~ErrorNotifyBatcher();

    static void BatchDispatch(nsIEventTarget* aTarget,
                              already_AddRefed<CancelableRunnable> aRunnable);

   private:
    [[nodiscard]] static bool TryBatchDispatch(
        nsIEventTarget* aTarget, RefPtr<CancelableRunnable>& aRunnable);

    static ErrorNotifyBatcher* sCurrent;

    class BatchTask;
    AutoTArray<RefPtr<BatchTask>, 8> mToNotify;
  };



 private:
  void PostErrorNotifyTask() MOZ_REQUIRES(*mMonitor);
  void OnNotifyMaybeChannelError() MOZ_EXCLUDES(*mMonitor);
  void ReportConnectionError(const char* aFunctionName,
                             const uint32_t aMsgTyp) const
      MOZ_REQUIRES(*mMonitor);
  bool MaybeHandleError(Result code, const Message& aMsg,
                        const char* channelName) MOZ_EXCLUDES(*mMonitor);

  void Clear() MOZ_REQUIRES(*mMonitor);

  bool HasPendingEvents() MOZ_REQUIRES(*mMonitor);

  void ProcessPendingRequests(ActorLifecycleProxy* aProxy,
                              AutoEnterTransaction& aTransaction)
      MOZ_REQUIRES(*mMonitor);
  bool ProcessPendingRequest(ActorLifecycleProxy* aProxy,
                             UniquePtr<Message> aUrgent)
      MOZ_REQUIRES(*mMonitor);

  void EnqueuePendingMessages() MOZ_REQUIRES(*mMonitor);

  void DispatchMessage(ActorLifecycleProxy* aProxy, UniquePtr<Message> aMsg)
      MOZ_REQUIRES(*mMonitor);

  void DispatchSyncMessage(ActorLifecycleProxy* aProxy, const Message& aMsg,
                           UniquePtr<Message>& aReply) MOZ_EXCLUDES(*mMonitor);
  void DispatchAsyncMessage(ActorLifecycleProxy* aProxy, const Message& aMsg)
      MOZ_EXCLUDES(*mMonitor);

  bool WaitForSyncNotify() MOZ_REQUIRES(*mMonitor);

  bool WaitResponse(bool aWaitTimedOut);

  bool ShouldContinueFromTimeout() MOZ_REQUIRES(*mMonitor);

  void EndTimeout() MOZ_REQUIRES(*mMonitor);
  void CancelTransaction(seqno_t transaction) MOZ_REQUIRES(*mMonitor);

  void RepostAllMessages() MOZ_REQUIRES(*mMonitor);

  seqno_t NextSeqno() {
    AssertWorkerThread();
    MOZ_RELEASE_ASSERT(mozilla::Abs(mNextSeqno) < INT64_MAX, "seqno overflow");
    return (mSide == ChildSide) ? --mNextSeqno : ++mNextSeqno;
  }

  void DebugAbort(const char* file, int line, const char* cond, const char* why,
                  bool reply = false) MOZ_REQUIRES(*mMonitor);

 private:
  bool DispatchingAsyncMessage() const {
    AssertWorkerThread();
    return mDispatchingAsyncMessage;
  }

  int DispatchingAsyncMessageNestedLevel() const {
    AssertWorkerThread();
    return mDispatchingAsyncMessageNestedLevel;
  }

  bool Connected() const MOZ_REQUIRES(*mMonitor);

  bool ConnectedOrClosing() const MOZ_REQUIRES(*mMonitor);

 private:
  void NotifyWorkerThread() MOZ_REQUIRES(*mMonitor);

  bool MaybeInterceptSpecialIOMessage(const Message& aMsg)
      MOZ_REQUIRES(*mMonitor);

  static bool IsAlwaysDeferred(const Message& aMsg);

  void SendMessageToLink(UniquePtr<Message> aMsg) MOZ_REQUIRES(*mMonitor);

  void FlushLazySendMessages() MOZ_REQUIRES(*mMonitor);

  bool ShouldDeferMessage(const Message& aMsg) MOZ_REQUIRES(*mMonitor);
  void OnMessageReceivedFromLink(UniquePtr<Message> aMsg)
      MOZ_REQUIRES(*mMonitor);
  void OnChannelErrorFromLink() MOZ_REQUIRES(*mMonitor);

 private:
  void NotifyChannelClosed(ReleasableMonitorAutoLock& aLock)
      MOZ_REQUIRES(*mMonitor);
  void NotifyMaybeChannelError(ReleasableMonitorAutoLock& aLock)
      MOZ_REQUIRES(*mMonitor);

 private:
  void AssertWorkerThread() const {
    MOZ_ASSERT(mWorkerThread, "Channel hasn't been opened yet");
    MOZ_RELEASE_ASSERT(mWorkerThread && mWorkerThread->IsOnCurrentThread(),
                       "not on worker thread!");
  }

 private:
  class MessageTask : public CancelableRunnable,
                      public LinkedListElement<RefPtr<MessageTask>>,
                      public nsIRunnablePriority,
                      public nsIRunnableIPCMessageType {
   public:
    explicit MessageTask(MessageChannel* aChannel, UniquePtr<Message> aMessage);
    MessageTask() = delete;
    MessageTask(const MessageTask&) = delete;

    NS_DECL_ISUPPORTS_INHERITED

    NS_IMETHOD Run() override;
    nsresult Cancel() override;
    NS_IMETHOD GetPriority(uint32_t* aPriority) override;
    NS_DECL_NSIRUNNABLEIPCMESSAGETYPE
    void Post() MOZ_REQUIRES(*mMonitor);

    bool IsScheduled() const MOZ_REQUIRES(*mMonitor) {
      mMonitor->AssertCurrentThreadOwns();
      return mScheduled;
    }

    UniquePtr<Message>& Msg() MOZ_REQUIRES(*mMonitor) {
      MOZ_DIAGNOSTIC_ASSERT(mMessage, "message was moved");
      return mMessage;
    }
    const UniquePtr<Message>& Msg() const MOZ_REQUIRES(*mMonitor) {
      MOZ_DIAGNOSTIC_ASSERT(mMessage, "message was moved");
      return mMessage;
    }

    void AssertMonitorHeld(const RefCountedMonitor& aMonitor)
        MOZ_REQUIRES(aMonitor) MOZ_ASSERT_CAPABILITY(*mMonitor) {
      aMonitor.AssertSameMonitor(*mMonitor);
    }

   private:
    ~MessageTask();

    MessageChannel* Channel() MOZ_REQUIRES(*mMonitor) {
      mMonitor->AssertCurrentThreadOwns();
      MOZ_RELEASE_ASSERT(isInList());
      return mChannel;
    }

    RefPtr<RefCountedMonitor> const mMonitor;
    MessageChannel* const mChannel;
    UniquePtr<Message> mMessage MOZ_GUARDED_BY(*mMonitor);
    uint32_t const mPriority;
    bool mScheduled : 1 MOZ_GUARDED_BY(*mMonitor);
  };

  bool ShouldRunMessage(const Message& aMsg) MOZ_REQUIRES(*mMonitor);
  void RunMessage(ActorLifecycleProxy* aProxy, MessageTask& aTask)
      MOZ_REQUIRES(*mMonitor);

  class WorkerTargetShutdownTask final : public nsITargetShutdownTask {
   public:
    NS_DECL_THREADSAFE_ISUPPORTS

    WorkerTargetShutdownTask(nsISerialEventTarget* aTarget,
                             MessageChannel* aChannel);

    void TargetShutdown() override;
    void Clear();

   private:
    ~WorkerTargetShutdownTask() = default;

    const nsCOMPtr<nsISerialEventTarget> mTarget;
    MessageChannel* MOZ_NON_OWNING_REF mChannel;
  };

  class FlushLazySendMessagesRunnable final : public CancelableRunnable {
   public:
    explicit FlushLazySendMessagesRunnable(MessageChannel* aChannel);

    NS_DECL_ISUPPORTS_INHERITED

    NS_IMETHOD Run() override;
    nsresult Cancel() override;

    void PushMessage(UniquePtr<Message> aMsg);
    nsTArray<UniquePtr<Message>> TakeMessages();

   private:
    ~FlushLazySendMessagesRunnable() = default;

    MessageChannel* MOZ_NON_OWNING_REF mChannel;

    nsTArray<UniquePtr<Message>> mQueue;
  };

  typedef LinkedList<RefPtr<MessageTask>> MessageQueue;
  typedef IPC::Message::msgid_t msgid_t;

 private:
  const char* const mName;

  nsID mMessageChannelId MOZ_GUARDED_BY(*mMonitor) = {};

  IToplevelProtocol* const mListener;

  RefPtr<RefCountedMonitor> const mMonitor;

  ChannelState mChannelState MOZ_GUARDED_BY(*mMonitor) = ChannelClosed;
  Side mSide = UnknownSide;
  bool mIsCrossProcess MOZ_GUARDED_BY(*mMonitor) = false;
  UniquePtr<MessageLink> mLink MOZ_GUARDED_BY(*mMonitor);

  RefPtr<CancelableRunnable> mChannelErrorTask MOZ_GUARDED_BY(*mMonitor);

  nsCOMPtr<nsISerialEventTarget> mWorkerThread;

  RefPtr<WorkerTargetShutdownTask> mShutdownTask MOZ_GUARDED_BY(*mMonitor);

  RefPtr<FlushLazySendMessagesRunnable> mFlushLazySendTask
      MOZ_GUARDED_BY(*mMonitor);

  int32_t mTimeoutMs = kNoTimeout;
  bool mInTimeoutSecondHalf = false;

  seqno_t mNextSeqno = 0;

  static bool sIsPumpingMessages;

  SyncSendError mLastSendError = SyncSendError::SendSuccess;

  template <class T>
  class AutoSetValue {
   public:
    explicit AutoSetValue(T& var, const T& newValue)
        : mVar(var), mPrev(var), mNew(newValue) {
      mVar = newValue;
    }
    ~AutoSetValue() {
      if (mVar == mNew) {
        mVar = mPrev;
      }
    }

   private:
    T& mVar;
    T mPrev;
    T mNew;
  };

  bool mDispatchingAsyncMessage = false;
  int mDispatchingAsyncMessageNestedLevel = 0;


  friend class AutoEnterTransaction;
  AutoEnterTransaction* mTransactionStack MOZ_GUARDED_BY(*mMonitor) = nullptr;

  seqno_t CurrentNestedInsideSyncTransaction() const MOZ_REQUIRES(*mMonitor);

  bool AwaitingSyncReply() const MOZ_REQUIRES(*mMonitor);
  int AwaitingSyncReplyNestedLevel() const MOZ_REQUIRES(*mMonitor);

  bool DispatchingSyncMessage() const MOZ_REQUIRES(*mMonitor);
  int DispatchingSyncMessageNestedLevel() const MOZ_REQUIRES(*mMonitor);

#if defined(DEBUG)
  void AssertMaybeDeferredCountCorrect() MOZ_REQUIRES(*mMonitor);
#else
  void AssertMaybeDeferredCountCorrect() MOZ_REQUIRES(*mMonitor) {}
#endif

  seqno_t mTimedOutMessageSeqno MOZ_GUARDED_BY(*mMonitor) = 0;
  int mTimedOutMessageNestedLevel MOZ_GUARDED_BY(*mMonitor) = 0;

  MessageQueue mPending MOZ_GUARDED_BY(*mMonitor);

  size_t mMaybeDeferredPendingCount MOZ_GUARDED_BY(*mMonitor) = 0;

  bool mOnCxxStack = false;


  bool mAbortOnError MOZ_GUARDED_BY(*mMonitor) = false;

  bool mNotifiedChannelDone MOZ_GUARDED_BY(*mMonitor) = false;

  ChannelFlags mFlags = REQUIRE_DEFAULT;

  bool mBuildIDsConfirmedMatch MOZ_GUARDED_BY(*mMonitor) = false;

  bool mIsSameThreadChannel = false;
};

void CancelCPOWs();

}  
}  

namespace IPC {
template <>
struct ParamTraits<mozilla::ipc::ResponseRejectReason>
    : public ContiguousEnumSerializer<
          mozilla::ipc::ResponseRejectReason,
          mozilla::ipc::ResponseRejectReason::SendError,
          mozilla::ipc::ResponseRejectReason::EndGuard_> {};
}  

#endif
