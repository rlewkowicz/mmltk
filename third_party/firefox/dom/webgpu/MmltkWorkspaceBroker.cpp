/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MmltkWorkspaceBroker.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

namespace mozilla::webgpu {

namespace {

constexpr uint32_t kMagic = 0x574c4d4dU;
constexpr uint16_t kVersion = 3U;
constexpr size_t kTokenBytes = 64U;
constexpr size_t kDeviceUuidBytes = 16U;
constexpr uint32_t kFlagTimelineSemaphore = 1U << 0U;
constexpr uint32_t kFlagSlotReserved = 1U << 1U;

enum class MessageType : uint16_t {
  Hello = 1,
  Adapter = 2,
  ConfigureSlot = 3,
  SlotReady = 4,
  Present = 5,
  Retire = 6,
  Ack = 7,
  Error = 8,
  ReserveSlot = 9,
};

struct Message {
  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  MessageType type = MessageType::Error;
  uint32_t size = sizeof(Message);
  uint32_t flags = 0;
  uint32_t slot = 0;
  uint32_t slotCount = 0;
  uint32_t modifierIndex = 0;
  uint32_t modifierCount = 0;
  uint64_t generation = 0;
  uint64_t revision = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sourceX = 0;
  uint32_t sourceY = 0;
  uint32_t sourceWidth = 0;
  uint32_t sourceHeight = 0;
  uint32_t renderMajor = 0;
  uint32_t renderMinor = 0;
  uint64_t stride = 0;
  uint64_t offset = 0;
  uint64_t allocationSize = 0;
  uint64_t drmModifier = 0;
  uint32_t drmFormat = 0;
  uint32_t errorCode = 0;
  uint64_t captureNs = 0;
  uint64_t readyNs = 0;
  std::array<uint8_t, kDeviceUuidBytes> deviceUuid{};
  std::array<char, kTokenBytes> token{};
  std::array<uint8_t, 8> reserved{};
};

static_assert(sizeof(Message) == 224U);

bool Valid(const Message& aMessage) {
  return aMessage.magic == kMagic && aMessage.version == kVersion &&
         aMessage.size == sizeof(Message);
}

struct SlotKey {
  uint64_t generation;
  uint32_t slot;

  bool operator==(const SlotKey&) const = default;
};

struct SlotKeyHash {
  size_t operator()(const SlotKey& aKey) const noexcept {
    return std::hash<uint64_t>{}(aKey.generation) ^
           (std::hash<uint32_t>{}(aKey.slot) << 1U);
  }
};

struct SlotState {
  MmltkWorkspaceSlot descriptor;
  int dmaBufFd = -1;
  uint64_t revision = 0;
  bool imported = false;
  bool reserved = false;
};

}  

struct MmltkWorkspaceBroker::Impl {
  Impl() {
    const char* socketPath = std::getenv("MMLTK_WORKSPACE_BROKER_SOCKET");
    const char* token = std::getenv("MMLTK_WORKSPACE_BROKER_TOKEN");
    const char* origin = std::getenv("MMLTK_WORKSPACE_BROKER_ORIGIN");
    if (!socketPath || !*socketPath || !token || std::strlen(token) != kTokenBytes ||
        !origin || !*origin || std::strlen(socketPath) >= sizeof(sockaddr_un::sun_path)) {
      return;
    }
    struct stat socketStat {};
    std::string socketParent(socketPath);
    const size_t separator = socketParent.find_last_of('/');
    if (separator == std::string::npos) {
      return;
    }
    socketParent.resize(separator);
    struct stat parentStat {};
    if (::lstat(socketPath, &socketStat) != 0 || !S_ISSOCK(socketStat.st_mode) ||
        socketStat.st_uid != ::getuid() || (socketStat.st_mode & 0777U) != 0600U ||
        ::lstat(socketParent.c_str(), &parentStat) != 0 ||
        !S_ISDIR(parentStat.st_mode) || parentStat.st_uid != ::getuid() ||
        (parentStat.st_mode & 0777U) != 0700U) {
      return;
    }

    mFd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (mFd < 0) {
      return;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath, std::strlen(socketPath) + 1U);
    if (::connect(mFd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) != 0) {
      Close();
      return;
    }
    ucred credentials{};
    socklen_t credentialsSize = sizeof(credentials);
    if (::getsockopt(mFd, SOL_SOCKET, SO_PEERCRED, &credentials,
                     &credentialsSize) != 0 ||
        credentialsSize != sizeof(credentials) || credentials.uid != ::getuid()) {
      Close();
      return;
    }

    Message hello;
    hello.type = MessageType::Hello;
    std::memcpy(hello.token.data(), token, hello.token.size());
    if (!Send(hello)) {
      Close();
      return;
    }
    const int flags = ::fcntl(mFd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(mFd, F_SETFL, flags | O_NONBLOCK) != 0) {
      Close();
    }
  }

  ~Impl() { Close(); }

  void Close() {
    for (auto& [key, state] : mSlots) {
      (void)key;
      if (state.dmaBufFd >= 0) {
        ::close(state.dmaBufFd);
      }
    }
    mSlots.clear();
    mRetired.clear();
    mPendingReservations.clear();
    mFailed = false;
    if (mFd >= 0) {
      ::close(mFd);
      mFd = -1;
    }
  }

  bool Send(const Message& aMessage, int aFd = -1) {
    if (mFd < 0 || !Valid(aMessage)) {
      return false;
    }
    iovec payload{const_cast<Message*>(&aMessage), sizeof(aMessage)};
    msghdr header{};
    header.msg_iov = &payload;
    header.msg_iovlen = 1;
    std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
    if (aFd >= 0) {
      header.msg_control = control.data();
      header.msg_controllen = control.size();
      cmsghdr* cmsg = CMSG_FIRSTHDR(&header);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(sizeof(int));
      std::memcpy(CMSG_DATA(cmsg), &aFd, sizeof(aFd));
    }
    return ::sendmsg(mFd, &header, MSG_NOSIGNAL) ==
           static_cast<ssize_t>(sizeof(aMessage));
  }

  void SendError(uint32_t aCode, std::string_view aDetail) {
    Message error;
    error.type = MessageType::Error;
    error.errorCode = aCode;
    const size_t count = std::min(aDetail.size(), error.token.size() - 1U);
    std::memcpy(error.token.data(), aDetail.data(), count);
    error.token[count] = '\0';
    (void)Send(error);
  }

  void Fail(uint32_t aCode, std::string_view aDetail) {
    SendError(aCode, aDetail);
    mFailed = true;
  }

  void MaybeAcknowledgeRetirement(uint64_t aGeneration) {
    if (!mRetired.contains(aGeneration)) {
      return;
    }
    for (const auto& [key, state] : mSlots) {
      if (key.generation == aGeneration && state.imported) {
        return;
      }
    }
    for (auto current = mSlots.begin(); current != mSlots.end();) {
      if (current->first.generation != aGeneration) {
        ++current;
        continue;
      }
      if (current->second.dmaBufFd >= 0) {
        ::close(current->second.dmaBufFd);
      }
      current = mSlots.erase(current);
    }
    for (auto current = mPendingReservations.begin();
         current != mPendingReservations.end();) {
      if (current->generation == aGeneration) {
        current = mPendingReservations.erase(current);
      } else {
        ++current;
      }
    }
    Message ack;
    ack.type = MessageType::Ack;
    ack.generation = aGeneration;
    if (Send(ack)) {
      mRetired.erase(aGeneration);
    }
  }

  void FlushReservations() {
    for (auto current = mPendingReservations.begin();
         current != mPendingReservations.end();) {
      Message reserve;
      reserve.type = MessageType::ReserveSlot;
      reserve.generation = current->generation;
      reserve.slot = current->slot;
      if (!Send(reserve)) {
        return;
      }
      current = mPendingReservations.erase(current);
    }
  }

  bool WaitForReservation(const SlotKey& aKey) {
    for (uint32_t attempt = 0; attempt < 100U && mFd >= 0 && !mFailed;
         ++attempt) {
      Pump();
      const auto found = mSlots.find(aKey);
      if (found == mSlots.end()) {
        return false;
      }
      if (found->second.reserved) {
        return true;
      }
      pollfd descriptor{mFd, POLLIN, 0};
      int status;
      do {
        status = ::poll(&descriptor, 1, 50);
      } while (status < 0 && errno == EINTR);
      if (status < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return false;
      }
    }
    return false;
  }

  void Pump() {
    if (mFailed) {
      return;
    }
    FlushReservations();
    std::vector<uint64_t> retirements(mRetired.begin(), mRetired.end());
    for (const uint64_t generation : retirements) {
      MaybeAcknowledgeRetirement(generation);
      if (mFd < 0) {
        return;
      }
    }
    while (mFd >= 0) {
      Message message{};
      iovec payload{&message, sizeof(message)};
      std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
      msghdr header{};
      header.msg_iov = &payload;
      header.msg_iovlen = 1;
      header.msg_control = control.data();
      header.msg_controllen = control.size();
      const ssize_t received =
          ::recvmsg(mFd, &header, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
      if (received < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return;
      }
      if (received != static_cast<ssize_t>(sizeof(message)) ||
          (header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
          !Valid(message)) {
        Fail(1U, "invalid broker record");
        return;
      }
      int receivedFd = -1;
      bool ancillaryValid = true;
      for (cmsghdr* cmsg = CMSG_FIRSTHDR(&header); cmsg;
           cmsg = CMSG_NXTHDR(&header, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int)) && receivedFd < 0) {
          std::memcpy(&receivedFd, CMSG_DATA(cmsg), sizeof(receivedFd));
        } else {
          ancillaryValid = false;
        }
      }
      if (!ancillaryValid) {
        if (receivedFd >= 0) {
          ::close(receivedFd);
        }
        Fail(2U, "invalid ancillary descriptor count");
        return;
      }

      if (message.type == MessageType::ConfigureSlot && receivedFd >= 0) {
        constexpr uint64_t kInvalidModifier = (1ULL << 56U) - 1U;
        if (message.generation == 0 || message.slotCount == 0 ||
            message.slotCount > 16 || message.slot >= message.slotCount ||
            message.width == 0 || message.height == 0 ||
            message.width > 32767 || message.height > 32767 ||
            message.stride < static_cast<uint64_t>(message.width) * 4U ||
            message.allocationSize < message.offset ||
            message.allocationSize - message.offset <
                message.stride * message.height ||
            message.drmModifier == kInvalidModifier ||
            message.drmFormat != 0x34324241U ||
            mRetired.contains(message.generation)) {
          ::close(receivedFd);
          Fail(3U, "invalid workspace slot configuration");
          return;
        }
        SlotKey key{message.generation, message.slot};
        auto& state = mSlots[key];
        if (state.dmaBufFd >= 0 || state.imported) {
          ::close(receivedFd);
          Fail(4U, "duplicate workspace slot configuration");
          return;
        }
        state.descriptor = MmltkWorkspaceSlot{
            message.width,          message.height,      message.stride,
            message.offset,         message.allocationSize,
            message.drmModifier,    message.drmFormat,   message.slotCount};
        state.dmaBufFd = receivedFd;
        state.reserved = true;
        continue;
      }
      if (message.type == MessageType::Present && receivedFd < 0) {
        const auto found = mSlots.find({message.generation, message.slot});
        if (found == mSlots.end() || message.revision == 0 ||
            message.revision <= found->second.revision || message.width == 0 ||
            message.height == 0 ||
            message.width > found->second.descriptor.width ||
            message.height > found->second.descriptor.height ||
            message.sourceWidth == 0 || message.sourceHeight == 0 ||
            message.captureNs > message.readyNs) {
          Fail(5U, "invalid workspace presentation metadata");
          return;
        }
        found->second.revision = message.revision;
        continue;
      }
      if (message.type == MessageType::Retire && receivedFd < 0 &&
          message.generation != 0) {
        mRetired.insert(message.generation);
        MaybeAcknowledgeRetirement(message.generation);
        continue;
      }
      if (message.type == MessageType::Error && receivedFd < 0 &&
          message.errorCode != 0) {
        mFailed = true;
        return;
      }
      if (message.type == MessageType::Ack && receivedFd < 0 &&
          (message.flags & kFlagSlotReserved) != 0) {
        const auto found = mSlots.find({message.generation, message.slot});
        if (found == mSlots.end() || found->second.imported ||
            found->second.reserved) {
          Fail(10U, "unexpected workspace slot reservation acknowledgement");
          return;
        }
        found->second.reserved = true;
        found->second.revision = 0;
        continue;
      }
      if (receivedFd >= 0) {
        ::close(receivedFd);
      }
      Fail(6U, "unexpected workspace broker record");
      return;
    }
  }

  int mFd = -1;
  uint32_t mAdapterRenderMajor = 0;
  uint32_t mAdapterRenderMinor = 0;
  std::array<uint8_t, kDeviceUuidBytes> mAdapterDeviceUuid{};
  std::vector<uint64_t> mAdapterModifiers;
  std::unordered_map<SlotKey, SlotState, SlotKeyHash> mSlots;
  std::unordered_set<uint64_t> mRetired;
  std::unordered_set<SlotKey, SlotKeyHash> mPendingReservations;
  bool mFailed = false;
};

MmltkWorkspaceBroker::MmltkWorkspaceBroker()
    : mImpl(std::make_unique<Impl>()) {}

MmltkWorkspaceBroker::~MmltkWorkspaceBroker() = default;

bool MmltkWorkspaceBroker::ConfigureAdapter(
    uint32_t aRenderMajor, uint32_t aRenderMinor, const uint8_t* aDeviceUuid,
    const uint64_t* aModifiers, size_t aModifierCount,
    bool aTimelineSemaphore) {
  if (!aDeviceUuid || !aModifiers || aModifierCount == 0 ||
      aModifierCount > 256 || aRenderMajor == 0 || !aTimelineSemaphore) {
    mImpl->SendError(7U, "Vulkan adapter interop is unsupported");
    return false;
  }
  const bool alreadyConfigured = !mImpl->mAdapterModifiers.empty();
  if (alreadyConfigured) {
    return mImpl->mAdapterRenderMajor == aRenderMajor &&
           mImpl->mAdapterRenderMinor == aRenderMinor &&
           std::memcmp(mImpl->mAdapterDeviceUuid.data(), aDeviceUuid,
                       mImpl->mAdapterDeviceUuid.size()) == 0 &&
           mImpl->mAdapterModifiers.size() == aModifierCount &&
           std::equal(mImpl->mAdapterModifiers.begin(),
                      mImpl->mAdapterModifiers.end(), aModifiers);
  }
  mImpl->mAdapterRenderMajor = aRenderMajor;
  mImpl->mAdapterRenderMinor = aRenderMinor;
  std::memcpy(mImpl->mAdapterDeviceUuid.data(), aDeviceUuid,
              mImpl->mAdapterDeviceUuid.size());
  mImpl->mAdapterModifiers.assign(aModifiers, aModifiers + aModifierCount);
  for (size_t index = 0; index < aModifierCount; ++index) {
    Message message;
    message.type = MessageType::Adapter;
    message.flags = kFlagTimelineSemaphore;
    message.modifierIndex = static_cast<uint32_t>(index);
    message.modifierCount = static_cast<uint32_t>(aModifierCount);
    message.renderMajor = aRenderMajor;
    message.renderMinor = aRenderMinor;
    message.drmModifier = aModifiers[index];
    std::memcpy(message.deviceUuid.data(), aDeviceUuid,
                message.deviceUuid.size());
    if (!mImpl->Send(message)) {
      mImpl->Close();
      return false;
    }
  }
  return true;
}

bool MmltkWorkspaceBroker::TakeSlot(uint64_t aGeneration, uint32_t aSlot,
                                    MmltkWorkspaceSlot* aDescriptor,
                                    int* aDmaBufFd) {
  if (!aDescriptor || !aDmaBufFd) {
    return false;
  }
  mImpl->Pump();
  const SlotKey key{aGeneration, aSlot};
  if (!mImpl->WaitForReservation(key)) {
    mImpl->SendError(8U, "workspace DMA-BUF slot reservation timed out");
    return false;
  }
  const auto found = mImpl->mSlots.find(key);
  if (found == mImpl->mSlots.end() || found->second.dmaBufFd < 0 ||
      found->second.imported || !found->second.reserved) {
    mImpl->SendError(8U, "workspace DMA-BUF slot is unavailable");
    return false;
  }
  const int importedFd =
      ::fcntl(found->second.dmaBufFd, F_DUPFD_CLOEXEC, 0);
  if (importedFd < 0) {
    mImpl->SendError(9U, "workspace DMA-BUF duplication failed");
    return false;
  }
  *aDescriptor = found->second.descriptor;
  *aDmaBufFd = importedFd;
  found->second.imported = true;
  found->second.reserved = false;
  return true;
}

bool MmltkWorkspaceBroker::SendSlotReady(uint64_t aGeneration,
                                         uint32_t aSlot,
                                         int aSemaphoreFd) {
  Message message;
  message.type = MessageType::SlotReady;
  message.generation = aGeneration;
  message.slot = aSlot;
  const bool sent = mImpl->Send(message, aSemaphoreFd);
  if (aSemaphoreFd >= 0) {
    ::close(aSemaphoreFd);
  }
  return sent;
}

void MmltkWorkspaceBroker::ReleaseSlot(uint64_t aGeneration,
                                       uint32_t aSlot) {
  mImpl->Pump();
  const auto found = mImpl->mSlots.find({aGeneration, aSlot});
  if (found == mImpl->mSlots.end() || !found->second.imported) {
    return;
  }
  found->second.imported = false;
  if (mImpl->mRetired.contains(aGeneration)) {
    mImpl->MaybeAcknowledgeRetirement(aGeneration);
    return;
  }
  mImpl->mPendingReservations.insert({aGeneration, aSlot});
  mImpl->Pump();
}

uint64_t MmltkWorkspaceBroker::PresentRevision(uint64_t aGeneration,
                                               uint32_t aSlot) {
  mImpl->Pump();
  const auto found = mImpl->mSlots.find({aGeneration, aSlot});
  return found == mImpl->mSlots.end() ? 0 : found->second.revision;
}

}  
