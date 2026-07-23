/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_SCROLLABLELAYERGUID_H
#define GFX_SCROLLABLELAYERGUID_H

#include <iosfwd>                        // for ostream
#include <stdint.h>                      // for uint8_t, uint32_t, uint64_t
#include "mozilla/layers/LayersTypes.h"  // for LayersId
#include "nsHashKeys.h"                  // for nsUint64HashKey

namespace mozilla {
namespace layers {

struct ScrollableLayerGuid {
  typedef uint64_t ViewID;
  typedef nsUint64HashKey ViewIDHashKey;
  static const ViewID NULL_SCROLL_ID;  
  static const ViewID START_SCROLL_ID = 2;  

  LayersId mLayersId;
  uint32_t mPresShellId;
  ViewID mScrollId;

  constexpr ScrollableLayerGuid()
      : mLayersId{0}, mPresShellId(0), mScrollId(0) {}

  ScrollableLayerGuid(LayersId aLayersId, uint32_t aPresShellId,
                      ViewID aScrollId);

  ScrollableLayerGuid(const ScrollableLayerGuid& other) = default;

  ~ScrollableLayerGuid() = default;

  bool operator==(const ScrollableLayerGuid& other) const;
  bool operator!=(const ScrollableLayerGuid& other) const;
  bool operator<(const ScrollableLayerGuid& other) const;

  friend std::ostream& operator<<(std::ostream& aOut,
                                  const ScrollableLayerGuid& aGuid);

  using Comparator = bool (*)(const ScrollableLayerGuid&,
                              const ScrollableLayerGuid&);

  static bool EqualsIgnoringPresShell(const ScrollableLayerGuid& aA,
                                      const ScrollableLayerGuid& aB);


  struct HashFn {
    std::size_t operator()(const ScrollableLayerGuid& aGuid) const;
  };

  struct HashIgnoringPresShellFn {
    std::size_t operator()(const ScrollableLayerGuid& aGuid) const;
  };

  struct EqualIgnoringPresShellFn {
    bool operator()(const ScrollableLayerGuid& lhs,
                    const ScrollableLayerGuid& rhs) const;
  };
};

}  
}  

#endif /* GFX_SCROLLABLELAYERGUID_H */
