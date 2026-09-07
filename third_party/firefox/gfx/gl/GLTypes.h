/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(GLTYPES_H_)
#define GLTYPES_H_

#include <stdint.h>

#if !defined(GLAPIENTRY)
#    define GLAPIENTRY
#    define GLAPI
#endif

typedef uint8_t realGLboolean;

#if !defined(__gltypes_h_) && !defined(__gl_h_)
#  define __gltypes_h_
#  define __gl_h_

typedef uint32_t GLenum;
typedef uint32_t GLbitfield;
typedef uint32_t GLuint;
typedef int32_t GLint;
typedef int32_t GLsizei;
typedef int8_t GLbyte;
typedef int16_t GLshort;
typedef uint8_t GLubyte;
typedef uint16_t GLushort;
typedef float GLfloat;
typedef float GLclampf;
#if !defined(GLdouble_defined)
typedef double GLdouble;
#endif
typedef double GLclampd;
typedef void GLvoid;

typedef char GLchar;
#if !defined(__gl2_h_)
typedef signed long int GLintptr;
typedef signed long int GLsizeiptr;
#endif

#endif


#include <stdint.h>

typedef struct __GLsync* GLsync;
typedef int64_t GLint64;
typedef uint64_t GLuint64;

typedef void* GLeglImage;

typedef void(GLAPIENTRY* GLDEBUGPROC)(GLenum source, GLenum type, GLuint id,
                                      GLenum severity, GLsizei length,
                                      const GLchar* message,
                                      const GLvoid* userParam);

typedef void* EGLImage;
typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef intptr_t EGLAttrib;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void* EGLDisplay;
typedef void* EGLDeviceEXT;
typedef void* EGLSurface;
typedef void* EGLClientBuffer;
typedef void* EGLCastToRelevantPtr;
typedef void* EGLImage;
typedef void* EGLSync;
typedef void* EGLStreamKHR;
typedef uint64_t EGLTime;

#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONFIG ((EGLConfig) nullptr)
#define EGL_NO_SYNC ((EGLSync)0)
#define EGL_NO_IMAGE ((EGLImage)0)

typedef void* EGLNativeDisplayType;
typedef void* EGLNativePixmapType;
typedef void* EGLNativeWindowType;

#endif
