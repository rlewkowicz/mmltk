/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_LayerTreeOwnerTracker_h
#define mozilla_layers_LayerTreeOwnerTracker_h

#include "base/process.h"   // for base::ProcessId
#include "LayersTypes.h"    // for LayersId
#include "mozilla/Mutex.h"  // for mozilla::Mutex

#include <functional>
#include <map>

namespace mozilla {

namespace dom {
class ContentParent;
}

namespace layers {

class LayerTreeOwnerTracker final {
 public:
  static void Initialize();
  static void Shutdown();
  static LayerTreeOwnerTracker* Get();

  void Map(LayersId aLayersId, base::ProcessId aProcessId);

  void Unmap(LayersId aLayersId, base::ProcessId aProcessId);

  bool IsMapped(LayersId aLayersId, base::ProcessId aProcessId);

  void Iterate(
      const std::function<void(LayersId aLayersId, base::ProcessId aProcessId)>&
          aCallback);

 private:
  LayerTreeOwnerTracker();

  mozilla::Mutex mLayerIdsLock MOZ_UNANNOTATED;
  std::map<LayersId, base::ProcessId> mLayerIds;
};

}  
}  

#endif  // mozilla_layers_LayerTreeOwnerTracker_h
