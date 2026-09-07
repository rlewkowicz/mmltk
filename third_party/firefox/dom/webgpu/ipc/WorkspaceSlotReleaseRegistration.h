/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WORKSPACE_SLOT_RELEASE_REGISTRATION_H_
#define WORKSPACE_SLOT_RELEASE_REGISTRATION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace mozilla::webgpu {

class WorkspaceSlotReleaseRegistrationOwner final {
 public:
  using DeviceId = uint64_t;
  using WindowId = uint64_t;

  struct RegisterResult {
    std::optional<WindowId> attach_window;
    std::optional<WindowId> detach_window;
  };

  [[nodiscard]] RegisterResult Register(DeviceId aDeviceId,
                                        WindowId aWindowId) {
    RegisterResult result;
    if (const auto device = mDeviceWindows.find(aDeviceId);
        device != mDeviceWindows.end()) {
      if (device->second == aWindowId) {
        return result;
      }
      result.detach_window = UnregisterDevice(device);
    }

    const auto [window, inserted] = mWindowDeviceCounts.try_emplace(aWindowId);
    ++window->second;
    mDeviceWindows.insert_or_assign(aDeviceId, aWindowId);
    if (inserted) {
      result.attach_window = aWindowId;
    }
    return result;
  }

  [[nodiscard]] std::optional<WindowId> Unregister(DeviceId aDeviceId) {
    const auto device = mDeviceWindows.find(aDeviceId);
    if (device == mDeviceWindows.end()) {
      return std::nullopt;
    }
    return UnregisterDevice(device);
  }

  [[nodiscard]] bool RemoveWindow(WindowId aWindowId) {
    if (mWindowDeviceCounts.erase(aWindowId) == 0) {
      return false;
    }
    for (auto device = mDeviceWindows.begin();
         device != mDeviceWindows.end();) {
      if (device->second == aWindowId) {
        device = mDeviceWindows.erase(device);
      } else {
        ++device;
      }
    }
    return true;
  }

  void Clear() {
    mDeviceWindows.clear();
    mWindowDeviceCounts.clear();
  }

  [[nodiscard]] std::size_t DeviceCount() const {
    return mDeviceWindows.size();
  }
  [[nodiscard]] std::size_t WindowCount() const {
    return mWindowDeviceCounts.size();
  }
  [[nodiscard]] std::size_t DeviceCount(WindowId aWindowId) const {
    const auto window = mWindowDeviceCounts.find(aWindowId);
    return window == mWindowDeviceCounts.end() ? 0 : window->second;
  }

 private:
  using DeviceWindowMap = std::unordered_map<DeviceId, WindowId>;

  [[nodiscard]] std::optional<WindowId> UnregisterDevice(
      DeviceWindowMap::iterator aDevice) {
    const WindowId windowId = aDevice->second;
    mDeviceWindows.erase(aDevice);
    const auto window = mWindowDeviceCounts.find(windowId);
    if (window == mWindowDeviceCounts.end()) {
      return std::nullopt;
    }
    if (window->second > 1) {
      --window->second;
      return std::nullopt;
    }
    mWindowDeviceCounts.erase(window);
    return windowId;
  }

  DeviceWindowMap mDeviceWindows;
  std::unordered_map<WindowId, std::size_t> mWindowDeviceCounts;
};

}  // namespace mozilla::webgpu

#endif  // WORKSPACE_SLOT_RELEASE_REGISTRATION_H_
