// Protocol Buffers - Google's data interchange format
// Copyright 2025 Google LLC.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// Restore macros that may have been #undef'd in os_macros_undef.inc.

#if defined(PROTOBUF_DID_UNDEF_noreturn)
#pragma pop_macro("noreturn")
#undef PROTOBUF_DID_UNDEF_noreturn
#endif

#if defined(PROTOBUF_DID_UNDEF_PACKAGE)
#pragma pop_macro("PACKAGE")
#undef PROTOBUF_DID_UNDEF_PACKAGE
#endif

#if defined(PROTOBUF_DID_UNDEF_PACKED)
#pragma pop_macro("PACKED")
#undef PROTOBUF_DID_UNDEF_PACKED
#endif

#if defined(PROTOBUF_DID_UNDEF_DOMAIN)
#pragma pop_macro("DOMAIN")
#undef PROTOBUF_DID_UNDEF_DOMAIN
#endif

#if defined(PROTOBUF_DID_UNDEF_LINUX)
#pragma pop_macro("linux")
#endif

#if defined(PROTOBUF_DID_UNDEF_NO_DATA)
#pragma pop_macro("NO_DATA")
#undef PROTOBUF_DID_UNDEF_NO_DATA
#endif



#if 0 || defined(__ANDROID__)
#pragma pop_macro("UID_MAX")
#pragma pop_macro("GID_MAX")
#endif


#if defined(__clang__) || defined(__GNUC__) || defined(_MSC_VER)
#pragma pop_macro("DEBUG")
#endif
