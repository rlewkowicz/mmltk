#if !defined(__eglplatform_h_)
#define __eglplatform_h_

/*
** Copyright 2007-2020 The Khronos Group Inc.
** SPDX-License-Identifier: Apache-2.0
*/


#include <KHR/khrplatform.h>


#if !defined(EGLAPI)
#define EGLAPI KHRONOS_APICALL
#endif

#if !defined(EGLAPIENTRY)
#define EGLAPIENTRY  KHRONOS_APIENTRY
#endif
#define EGLAPIENTRYP EGLAPIENTRY*


#if defined(EGL_NO_PLATFORM_SPECIFIC_TYPES)

typedef void *EGLNativeDisplayType;
typedef void *EGLNativePixmapType;
typedef void *EGLNativeWindowType;

#elif 0 || defined(__VC32__) && !0 && !defined(__SCITECH_SNAP__) /* Win32 and WinCE */
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>

typedef HDC     EGLNativeDisplayType;
typedef HBITMAP EGLNativePixmapType;

#if !defined(WINAPI_FAMILY) || (WINAPI_FAMILY == WINAPI_FAMILY_DESKTOP_APP) /* Windows Desktop */
typedef HWND    EGLNativeWindowType;
#else
#include <inspectable.h>
typedef IInspectable* EGLNativeWindowType;
#endif

#elif defined(__EMSCRIPTEN__)

typedef int EGLNativeDisplayType;
typedef int EGLNativePixmapType;
typedef int EGLNativeWindowType;

#elif defined(__WINSCW__) || defined(__SYMBIAN32__)  /* Symbian */

typedef int   EGLNativeDisplayType;
typedef void *EGLNativePixmapType;
typedef void *EGLNativeWindowType;

#elif defined(WL_EGL_PLATFORM)

typedef struct wl_display     *EGLNativeDisplayType;
typedef struct wl_egl_pixmap  *EGLNativePixmapType;
typedef struct wl_egl_window  *EGLNativeWindowType;

#elif defined(__GBM__)

typedef struct gbm_device  *EGLNativeDisplayType;
typedef struct gbm_bo      *EGLNativePixmapType;
typedef void               *EGLNativeWindowType;

#elif defined(USE_OZONE)

typedef intptr_t EGLNativeDisplayType;
typedef intptr_t EGLNativePixmapType;
typedef intptr_t EGLNativeWindowType;

#elif defined(USE_X11)

#include <X11/Xlib.h>
#include <X11/Xutil.h>

typedef Display *EGLNativeDisplayType;
typedef Pixmap   EGLNativePixmapType;
typedef Window   EGLNativeWindowType;

#elif defined(__unix__)

typedef void             *EGLNativeDisplayType;
typedef khronos_uintptr_t EGLNativePixmapType;
typedef khronos_uintptr_t EGLNativeWindowType;

#elif defined(__Fuchsia__)

typedef void              *EGLNativeDisplayType;
typedef khronos_uintptr_t  EGLNativePixmapType;
typedef khronos_uintptr_t  EGLNativeWindowType;

#elif defined(__QNX__)

typedef khronos_uintptr_t      EGLNativeDisplayType;
typedef struct _screen_pixmap* EGLNativePixmapType;  
typedef struct _screen_window* EGLNativeWindowType;  

#else
#error "Platform not recognized"
#endif

typedef EGLNativeDisplayType NativeDisplayType;
typedef EGLNativePixmapType  NativePixmapType;
typedef EGLNativeWindowType  NativeWindowType;


typedef khronos_int32_t EGLint;


#if defined(__cplusplus)
#define EGL_CAST(type, value) (static_cast<type>(value))
#else
#define EGL_CAST(type, value) ((type) (value))
#endif

#endif
