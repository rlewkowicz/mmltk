/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "prenv.h"

#include "GLContextProvider.h"
#include "mozilla/gfx/gfxVars.h"

namespace mozilla::gl {

using namespace mozilla::gfx;
using namespace mozilla::widget;

static class GLContextProviderEGL sGLContextProviderEGL;

already_AddRefed<GLContext> GLContextProviderLinux::CreateForCompositorWidget(
    CompositorWidget* aCompositorWidget, bool aHardwareWebRender,
    bool aForceAccelerated) {
  return sGLContextProviderEGL.CreateForCompositorWidget(
      aCompositorWidget, aHardwareWebRender, aForceAccelerated);
}

already_AddRefed<GLContext> GLContextProviderLinux::CreateHeadless(
    const GLContextCreateDesc& desc, nsACString* const out_failureId) {
  return sGLContextProviderEGL.CreateHeadless(desc, out_failureId);
}

GLContext* GLContextProviderLinux::GetGlobalContext() {
  return sGLContextProviderEGL.GetGlobalContext();
}

void GLContextProviderLinux::Shutdown() {
  sGLContextProviderEGL.Shutdown();
}

}  
