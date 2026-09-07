/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef include_gfx_ipc_UiCompositorControllerMessageTypes_h
#define include_gfx_ipc_UiCompositorControllerMessageTypes_h

namespace mozilla {
namespace layers {


// clang-format off
enum UiCompositorControllerMessageTypes {
  FIRST_PAINT                      = 0, 
  LAYERS_UPDATED                   = 1, 
  COMPOSITOR_CONTROLLER_OPEN       = 2, 
  IS_COMPOSITOR_CONTROLLER_OPEN    = 3  
};
// clang-format on

}  
}  

#endif  // include_gfx_ipc_UiCompositorControllerMessageTypes_h
