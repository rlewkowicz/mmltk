/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WEBGL_CONTEXT_UTILS_H_
#define WEBGL_CONTEXT_UTILS_H_

#include "WebGLStrongTypes.h"
#include "WebGLTypes.h"

namespace mozilla {

TexTarget TexImageTargetToTexTarget(TexImageTarget texImageTarget);

struct GLComponents {
  unsigned char mComponents;

  enum Components {
    Red = (1 << 0),
    Green = (1 << 1),
    Blue = (1 << 2),
    Alpha = (1 << 3),
    Stencil = (1 << 4),
    Depth = (1 << 5),
  };

  GLComponents() : mComponents(0) {}

  explicit GLComponents(TexInternalFormat format);

  bool IsSubsetOf(const GLComponents& other) const;
};

const char* InfoFrom(WebGLTexImageFunc func, WebGLTexDimensions dims);

}  

#endif  // WEBGL_CONTEXT_UTILS_H_
