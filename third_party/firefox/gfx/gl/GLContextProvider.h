/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(GLCONTEXTPROVIDER_H_)
#define GLCONTEXTPROVIDER_H_

#include "mozilla/AlreadyAddRefed.h"

#include "GLContextTypes.h"
#include "SurfaceTypes.h"

#include "nsSize.h"  // for gfx::IntSize (needed by GLContextProviderImpl.h below)
#include "nsStringFwd.h"  // needed by GLContextProviderImpl.h below

class nsIWidget;

namespace mozilla {
namespace widget {
class CompositorWidget;
}
namespace gl {

#define IN_GL_CONTEXT_PROVIDER_H

#define GL_CONTEXT_PROVIDER_NAME GLContextProviderNull

#include "GLContextProviderImpl.h"
#undef GL_CONTEXT_PROVIDER_NAME



#define GL_CONTEXT_PROVIDER_NAME GLContextProviderEGL
#include "GLContextProviderImpl.h"
#undef GL_CONTEXT_PROVIDER_NAME

#if defined(MOZ_WIDGET_GTK)
#  define GL_CONTEXT_PROVIDER_NAME GLContextProviderLinux
#  include "GLContextProviderImpl.h"
#  undef GL_CONTEXT_PROVIDER_NAME
#  define GL_CONTEXT_PROVIDER_DEFAULT GLContextProviderLinux
#endif


#if !defined(GL_CONTEXT_PROVIDER_DEFAULT)
#  define GL_CONTEXT_PROVIDER_DEFAULT GLContextProviderEGL
#endif

#if defined(MOZ_GL_PROVIDER)
#  define GL_CONTEXT_PROVIDER_NAME MOZ_GL_PROVIDER
#  include "GLContextProviderImpl.h"
#  undef GL_CONTEXT_PROVIDER_NAME
#  define GL_CONTEXT_PROVIDER_DEFAULT MOZ_GL_PROVIDER
#endif

#if defined(GL_CONTEXT_PROVIDER_DEFAULT)
typedef GL_CONTEXT_PROVIDER_DEFAULT GLContextProvider;
#else
typedef GLContextProviderNull GLContextProvider;
#endif

#undef IN_GL_CONTEXT_PROVIDER_H

}  
}  

#endif
