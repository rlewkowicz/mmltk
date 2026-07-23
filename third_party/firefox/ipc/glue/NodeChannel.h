/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_ipc_NodeChannel_h)
#define mozilla_ipc_NodeChannel_h

#include "mojo/core/ports/node.h"
#include "mojo/core/ports/node_delegate.h"
#include "base/process.h"
#include "chrome/common/ipc_message.h"
#include "chrome/common/ipc_channel.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "nsISupports.h"
#include "nsTHashMap.h"
#include "mozilla/Queue.h"
#include "mozilla/DataMutex.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/WeakPtr.h"


namespace mozilla::ipc {

class GeckoChildProcessHost;
class NodeController;


class NodeChannel final : public IPC::Channel::Listener {
  using NodeName = mojo::core::ports::NodeName;
  using PortName = mojo::core::ports::PortName;


 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING_WITH_DESTROY(NodeChannel, Destroy())

  struct Introduction {
    NodeName mName;
    IPC::Channel::ChannelHandle mHandle;
    base::ProcessId mMyPid = base::kInvalidProcessId;
    base::ProcessId mOtherPid = base::kInvalidProcessId;
  };

  class Listener {
   public:
    virtual ~Listener() = default;

    NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING

    virtual void OnEventMessage(const NodeName& aFromNode,
                                UniquePtr<IPC::Message> aMessage) = 0;
    virtual void OnBroadcast(const NodeName& aFromNode,
                             UniquePtr<IPC::Message> aMessage) = 0;
    virtual void OnIntroduce(const NodeName& aFromNode,
                             Introduction aIntroduction) = 0;
    virtual void OnRequestIntroduction(const NodeName& aFromNode,
                                       const NodeName& aName) = 0;
    virtual void OnAcceptInvite(const NodeName& aFromNode,
                                const NodeName& aRealName,
                                const PortName& aInitialPort) = 0;
    virtual void OnChannelError(const NodeName& aFromNode) = 0;
  };

  NodeChannel(const NodeName& aName, IPC::Channel* aChannel,
              Listener* aListener,
              base::ProcessId aPid = base::kInvalidProcessId,
              GeckoChildProcessHost* aChildProcessHost = nullptr);

  void SendEventMessage(UniquePtr<IPC::Message> aMessage);

  void Broadcast(UniquePtr<IPC::Message> aMessage);

  void RequestIntroduction(const NodeName& aPeerName);

  void Introduce(Introduction aIntroduction);

  void AcceptInvite(const NodeName& aRealName, const PortName& aInitialPort);

  base::ProcessId OtherPid() const { return mOtherPid; }

  void Start();

  void Close();

  void SetName(const NodeName& aNewName) { mName = aNewName; }


  void SetOtherPid(base::ProcessId aNewPid);


 private:
  ~NodeChannel();

  void Destroy();
  void FinalDestroy();

  void SendMessage(UniquePtr<IPC::Message> aMessage);

  void OnMessageReceived(UniquePtr<IPC::Message> aMessage) override;
  void OnChannelConnected(base::ProcessId aPeerPid) override;
  void OnChannelError() override;

  const RefPtr<Listener> mListener;


  NodeName mName;

  std::atomic<base::ProcessId> mOtherPid;

  const RefPtr<IPC::Channel> mChannel;

  enum class State { Active, Closing, Closed };
  std::atomic<State> mState = State::Active;


  WeakPtr<mozilla::ipc::GeckoChildProcessHost> mChildProcessHost;
};

}  

#endif
