/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef jit_FlushICache_h
#define jit_FlushICache_h

#include "mozilla/Assertions.h"  // MOZ_CRASH

#include <stddef.h>  // size_t

namespace js {
namespace jit {

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)

inline void FlushICache(void* code, size_t size) {
}
#elif (defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)) ||  \
    defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)

extern void FlushICache(void* code, size_t size);

#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)

inline void FlushICache(void* code, size_t size) { MOZ_CRASH(); }

#else
#  error "Unknown architecture!"
#endif

#if (defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)) ||      \
    defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)

inline void FlushExecutionContext() {
}
inline bool CanFlushExecutionContextForAllThreads() { return true; }
inline void FlushExecutionContextForAllThreads() {
}

#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)

inline void FlushExecutionContext() { MOZ_CRASH(); }
inline bool CanFlushExecutionContextForAllThreads() { MOZ_CRASH(); }
inline void FlushExecutionContextForAllThreads() { MOZ_CRASH(); }

#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)

extern void FlushExecutionContext();

extern bool CanFlushExecutionContextForAllThreads();

extern void FlushExecutionContextForAllThreads();

#else
#  error "Unknown architecture!"
#endif

}  
}  

#endif  // jit_FlushICache_h
