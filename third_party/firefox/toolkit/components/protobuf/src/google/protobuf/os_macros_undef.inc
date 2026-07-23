// Protocol Buffers - Google's data interchange format
// Copyright 2025 Google LLC.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// Push and undef macros from the OS that may conflict with protobuf's
// generated symbols.

// noreturn is defined as a macro in C's stdnoreturn.h
#if defined(noreturn)
#define PROTOBUF_DID_UNDEF_noreturn
#pragma push_macro("noreturn")
#undef noreturn
#endif

// autoheader defines this in some circumstances
#if defined(PACKAGE)
#define PROTOBUF_DID_UNDEF_PACKAGE
#pragma push_macro("PACKAGE")
#undef PACKAGE
#endif

// a few common headers define this
#if defined(PACKED)
#define PROTOBUF_DID_UNDEF_PACKED
#pragma push_macro("PACKED")
#undef PACKED
#endif

// This is a macro on Windows, macOS, and some variants of GCC.
#if defined(DOMAIN)
#define PROTOBUF_DID_UNDEF_DOMAIN
#pragma push_macro("DOMAIN")
#undef DOMAIN
#endif

// linux is a legacy MACRO defined in most popular C++ standards.
#if defined(linux)
#pragma push_macro("linux")
#undef linux
#define PROTOBUF_DID_UNDEF_LINUX
#endif

// netdb.h defines this on some platforms.
#if defined(NO_DATA)
#pragma push_macro("NO_DATA")
#undef NO_DATA
#define PROTOBUF_DID_UNDEF_NO_DATA
#endif

// Windows declares several inconvenient macro names.  We #undef them and then
// restore them in port_undef.inc.


#if 0 || defined(__ANDROID__)
// Inconvenient macro names from usr/include/limits.h in some Android NDKs.
#pragma push_macro("UID_MAX")
#undef UID_MAX
#pragma push_macro("GID_MAX")
#undef GID_MAX
#endif


#if defined(__clang__) || defined(__GNUC__) || defined(_MSC_VER)
// Don't let Objective-C Macros interfere with proto identifiers with the same
// name.
#pragma push_macro("DEBUG")
#undef DEBUG
#endif
