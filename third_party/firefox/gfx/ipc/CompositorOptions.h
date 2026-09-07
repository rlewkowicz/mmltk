/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _include_mozilla_gfx_ipc_CompositorOptions_h_
#define _include_mozilla_gfx_ipc_CompositorOptions_h_

namespace IPC {
template <typename>
struct ParamTraits;
}  

namespace mozilla {
namespace layers {

class CompositorOptions {
 public:
  CompositorOptions() = default;

  explicit CompositorOptions(bool aUseAPZ) : mUseAPZ(aUseAPZ) {}

  bool UseAPZ() const { return mUseAPZ; }
  bool InitiallyPaused() const { return mInitiallyPaused; }
  bool NeedFastSnaphot() const { return mNeedFastSnaphot; }
  bool AllowNativeCompositor() const { return mAllowNativeCompositor; }

  void SetUseAPZ(bool aUseAPZ) { mUseAPZ = aUseAPZ; }

  void SetInitiallyPaused(bool aPauseAtStartup) {
    mInitiallyPaused = aPauseAtStartup;
  }

  void SetNeedFastSnaphot(bool aNeedFastSnaphot) {
    mNeedFastSnaphot = aNeedFastSnaphot;
  }

  void SetAllowNativeCompositor(bool aAllowNativeCompositor) {
    mAllowNativeCompositor = aAllowNativeCompositor;
  }

  bool EqualsIgnoringApzEnablement(const CompositorOptions& aOther) const {
    return mInitiallyPaused == aOther.mInitiallyPaused &&
           mNeedFastSnaphot == aOther.mNeedFastSnaphot &&
           mAllowNativeCompositor == aOther.mAllowNativeCompositor;
  }

  bool operator==(const CompositorOptions& aOther) const {
    return mUseAPZ == aOther.mUseAPZ && EqualsIgnoringApzEnablement(aOther);
  }

  friend struct IPC::ParamTraits<CompositorOptions>;

 private:
  bool mUseAPZ = false;
  bool mInitiallyPaused = false;
  bool mNeedFastSnaphot = false;
  bool mAllowNativeCompositor = true;

};

}  
}  

#endif  // _include_mozilla_gfx_ipc_CompositorOptions_h_
