/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GPU_MmltkWorkspaceBroker_H_
#define GPU_MmltkWorkspaceBroker_H_

#include <cstdint>
#include <cstddef>
#include <memory>

namespace mozilla::webgpu {

struct MmltkWorkspaceSlot {
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t stride = 0;
  uint64_t offset = 0;
  uint64_t allocationSize = 0;
  uint64_t drmModifier = 0;
  uint32_t drmFormat = 0;
  uint32_t slotCount = 0;
};

class MmltkWorkspaceBroker final {
 public:
  MmltkWorkspaceBroker();
  ~MmltkWorkspaceBroker();

  MmltkWorkspaceBroker(const MmltkWorkspaceBroker&) = delete;
  MmltkWorkspaceBroker& operator=(const MmltkWorkspaceBroker&) = delete;

  bool ConfigureAdapter(uint32_t aRenderMajor, uint32_t aRenderMinor,
                        const uint8_t* aDeviceUuid,
                        const uint64_t* aModifiers, size_t aModifierCount,
                        bool aTimelineSemaphore);
  bool TakeSlot(uint64_t aGeneration, uint32_t aSlot,
                MmltkWorkspaceSlot* aDescriptor, int* aDmaBufFd);
  bool SendSlotReady(uint64_t aGeneration, uint32_t aSlot,
                     int aSemaphoreFd);
  void ReleaseSlot(uint64_t aGeneration, uint32_t aSlot);
  uint64_t PresentRevision(uint64_t aGeneration, uint32_t aSlot);

 private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

}  

#endif  // GPU_MmltkWorkspaceBroker_H_
