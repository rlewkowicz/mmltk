/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WEBGL_TRANSFORM_FEEDBACK_H_
#define WEBGL_TRANSFORM_FEEDBACK_H_

#include "WebGLObjectModel.h"
#include "WebGLTypes.h"

namespace mozilla {
namespace webgl {
struct CachedDrawFetchLimits;
}

class WebGLTransformFeedback final : public WebGLContextBoundObject {
  friend class ScopedDrawWithTransformFeedback;
  friend class WebGLContext;
  friend class WebGL2Context;
  friend class WebGLProgram;

  MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(WebGLTransformFeedback, override)

  friend const webgl::CachedDrawFetchLimits* ValidateDraw(WebGLContext*, GLenum,
                                                          uint32_t);

 public:
  const GLuint mGLName;
  bool mHasBeenBound = false;

 private:
  std::vector<IndexedBufferBinding> mIndexedBindings;
  bool mIsPaused;
  bool mIsActive;
  RefPtr<WebGLProgram> mActive_Program;
  MOZ_INIT_OUTSIDE_CTOR GLenum mActive_PrimMode;
  MOZ_INIT_OUTSIDE_CTOR size_t mActive_VertPosition;
  MOZ_INIT_OUTSIDE_CTOR size_t mActive_VertCapacity;

 public:
  WebGLTransformFeedback(WebGLContext* webgl, GLuint tf);

 private:
  ~WebGLTransformFeedback() override;
  bool PrepareTransformFeedback();

 public:
  bool IsActiveAndNotPaused() const { return mIsActive && !mIsPaused; }

  void BeginTransformFeedback(GLenum primMode);
  void EndTransformFeedback();
  void PauseTransformFeedback();
  void ResumeTransformFeedback();
};

}  

#endif  // WEBGL_TRANSFORM_FEEDBACK_H_
