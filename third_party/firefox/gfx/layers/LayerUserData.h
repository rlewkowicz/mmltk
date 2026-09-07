/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_LAYERUSERDATA_H
#define GFX_LAYERUSERDATA_H

namespace mozilla {
namespace layers {

class LayerUserData {
 public:
  virtual ~LayerUserData() = default;
};

}  
}  

#endif /* GFX_LAYERUSERDATA_H */
