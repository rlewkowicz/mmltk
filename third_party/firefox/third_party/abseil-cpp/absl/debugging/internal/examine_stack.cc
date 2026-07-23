// Copyright 2018 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/debugging/internal/examine_stack.h"

#include <unistd.h>

#include "absl/base/config.h"

#if defined(ABSL_HAVE_MMAP)
#include <sys/mman.h>
#if defined(MAP_ANON) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#if defined(__linux__) || 0
#include <sys/ucontext.h>
#endif

#include <csignal>
#include <cstdio>

#include "absl/base/attributes.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/base/macros.h"
#include "absl/debugging/stacktrace.h"
#include "absl/debugging/symbolize.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {

namespace {
constexpr int kDefaultDumpStackFramesLimit = 64;
constexpr int kPrintfPointerFieldWidth = 2 + 2 * sizeof(void*);

ABSL_CONST_INIT SymbolizeUrlEmitter debug_stack_trace_hook = nullptr;

void* Allocate(size_t num_bytes) {
#if defined(ABSL_HAVE_MMAP)
  void* p = ::mmap(nullptr, num_bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
#else
  (void)num_bytes;
  return nullptr;
#endif
}

void Deallocate(void* p, size_t size) {
#if defined(ABSL_HAVE_MMAP)
  ::munmap(p, size);
#else
  (void)p;
  (void)size;
#endif
}

void DumpPC(OutputWriter* writer, void* writer_arg, void* const pc,
            const char* const prefix) {
  char buf[100];
  snprintf(buf, sizeof(buf), "%s@ %*p\n", prefix, kPrintfPointerFieldWidth, pc);
  writer(buf, writer_arg);
}

void DumpPCAndFrameSize(OutputWriter* writer, void* writer_arg, void* const pc,
                        int framesize, const char* const prefix) {
  char buf[100];
  if (framesize <= 0) {
    snprintf(buf, sizeof(buf), "%s@ %*p  (unknown)\n", prefix,
             kPrintfPointerFieldWidth, pc);
  } else {
    snprintf(buf, sizeof(buf), "%s@ %*p  %9d\n", prefix,
             kPrintfPointerFieldWidth, pc, framesize);
  }
  writer(buf, writer_arg);
}

void DumpPCAndSymbol(OutputWriter* writer, void* writer_arg, void* const pc,
                     const char* const prefix) {
  char tmp[1024];
  const char* symbol = "(unknown)";
  const uintptr_t prev_pc = reinterpret_cast<uintptr_t>(pc) - 1;
  if (absl::Symbolize(reinterpret_cast<const char*>(prev_pc), tmp,
                      sizeof(tmp)) ||
      absl::Symbolize(pc, tmp, sizeof(tmp))) {
    symbol = tmp;
  }
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s@ %*p  %s\n", prefix, kPrintfPointerFieldWidth,
           pc, symbol);
  writer(buf, writer_arg);
}

void DumpPCAndFrameSizeAndSymbol(OutputWriter* writer, void* writer_arg,
                                 void* const pc, void* const symbolize_pc,
                                 int framesize, const char* const prefix) {
  char tmp[1024];
  const char* symbol = "(unknown)";
  if (absl::Symbolize(symbolize_pc, tmp, sizeof(tmp))) {
    symbol = tmp;
  }
  char buf[1024];
  if (framesize <= 0) {
    snprintf(buf, sizeof(buf), "%s@ %*p  (unknown)  %s\n", prefix,
             kPrintfPointerFieldWidth, pc, symbol);
  } else {
    snprintf(buf, sizeof(buf), "%s@ %*p  %9d  %s\n", prefix,
             kPrintfPointerFieldWidth, pc, framesize, symbol);
  }
  writer(buf, writer_arg);
}

void DebugStackTraceHookLegacyAdapter(void* const stack[], int depth,
                                      OutputWriter* writer, void* writer_arg) {
  debug_stack_trace_hook(stack, depth, nullptr, writer,
                         writer_arg);
}

}  

void RegisterDebugStackTraceHook(SymbolizeUrlEmitter hook) {
  debug_stack_trace_hook = hook;
}

SymbolizeUrlEmitterLegacy GetDebugStackTraceHookLegacy() {
  if (debug_stack_trace_hook == nullptr) {
    return nullptr;
  }
  return &DebugStackTraceHookLegacyAdapter;
}

SymbolizeUrlEmitter GetDebugStackTraceHook() { return debug_stack_trace_hook; }

void* GetProgramCounter(void* const vuc) {
#if defined(__linux__)
  if (vuc != nullptr) {
    ucontext_t* context = reinterpret_cast<ucontext_t*>(vuc);
#if defined(__aarch64__)
    return reinterpret_cast<void*>(context->uc_mcontext.pc);
#elif defined(__alpha__)
    return reinterpret_cast<void*>(context->uc_mcontext.sc_pc);
#elif defined(__arm__)
    return reinterpret_cast<void*>(context->uc_mcontext.arm_pc);
#elif defined(__hppa__)
    return reinterpret_cast<void*>(context->uc_mcontext.sc_iaoq[0]);
#elif defined(__i386__)
    if (14 < ABSL_ARRAYSIZE(context->uc_mcontext.gregs))
      return reinterpret_cast<void*>(context->uc_mcontext.gregs[14]);
#elif defined(__ia64__)
    return reinterpret_cast<void*>(context->uc_mcontext.sc_ip);
#elif defined(__m68k__)
    return reinterpret_cast<void*>(context->uc_mcontext.gregs[16]);
#elif defined(__mips__)
    return reinterpret_cast<void*>(context->uc_mcontext.pc);
#elif defined(__powerpc64__)
    return reinterpret_cast<void*>(context->uc_mcontext.gp_regs[32]);
#elif defined(__powerpc__)
    return reinterpret_cast<void*>(context->uc_mcontext.uc_regs->gregs[32]);
#elif defined(__riscv)
    return reinterpret_cast<void*>(context->uc_mcontext.__gregs[REG_PC]);
#elif defined(__s390__) && !defined(__s390x__)
    return reinterpret_cast<void*>(context->uc_mcontext.psw.addr & 0x7fffffff);
#elif defined(__s390__) && defined(__s390x__)
    return reinterpret_cast<void*>(context->uc_mcontext.psw.addr);
#elif defined(__sh__)
    return reinterpret_cast<void*>(context->uc_mcontext.pc);
#elif defined(__sparc__) && !defined(__arch64__)
    return reinterpret_cast<void*>(context->uc_mcontext.gregs[19]);
#elif defined(__sparc__) && defined(__arch64__)
    return reinterpret_cast<void*>(context->uc_mcontext.mc_gregs[19]);
#elif defined(__x86_64__)
    if (16 < ABSL_ARRAYSIZE(context->uc_mcontext.gregs))
      return reinterpret_cast<void*>(context->uc_mcontext.gregs[16]);
#elif defined(__e2k__)
    return reinterpret_cast<void*>(context->uc_mcontext.cr0_hi);
#elif defined(__loongarch__)
    return reinterpret_cast<void*>(context->uc_mcontext.__pc);
#else
#error "Undefined Architecture."
#endif
  }
#elif defined(__akaros__)
  auto* ctx = reinterpret_cast<struct user_context*>(vuc);
  return reinterpret_cast<void*>(get_user_ctx_pc(ctx));
#endif
  static_cast<void>(vuc);
  return nullptr;
}

void DumpPCAndFrameSizesAndStackTrace(void* const pc, void* const stack[],
                                      int frame_sizes[], int depth,
                                      int min_dropped_frames,
                                      bool symbolize_stacktrace,
                                      OutputWriter* writer, void* writer_arg) {
  if (pc != nullptr) {
    if (symbolize_stacktrace) {
      DumpPCAndFrameSizeAndSymbol(writer, writer_arg, pc, pc, 0, "PC: ");
    } else {
      DumpPCAndFrameSize(writer, writer_arg, pc, 0, "PC: ");
    }
  }
  for (int i = 0; i < depth; i++) {
    if (symbolize_stacktrace) {
      DumpPCAndFrameSizeAndSymbol(writer, writer_arg, stack[i],
                                  reinterpret_cast<char*>(stack[i]) - 1,
                                  frame_sizes[i], "    ");
    } else {
      DumpPCAndFrameSize(writer, writer_arg, stack[i], frame_sizes[i], "    ");
    }
  }
  if (min_dropped_frames > 0) {
    char buf[100];
    snprintf(buf, sizeof(buf), "    @ ... and at least %d more frames\n",
             min_dropped_frames);
    writer(buf, writer_arg);
  }
}

ABSL_ATTRIBUTE_NOINLINE
void DumpStackTrace(int min_dropped_frames, int max_num_frames,
                    bool symbolize_stacktrace, OutputWriter* writer,
                    void* writer_arg) {
  void* stack_buf[kDefaultDumpStackFramesLimit];
  void** stack = stack_buf;
  int num_stack = kDefaultDumpStackFramesLimit;
  size_t allocated_bytes = 0;

  if (num_stack >= max_num_frames) {
    num_stack = max_num_frames;
  } else {
    const size_t needed_bytes =
        static_cast<size_t>(max_num_frames) * sizeof(stack[0]);
    void* p = Allocate(needed_bytes);
    if (p != nullptr) {  
      num_stack = max_num_frames;
      stack = reinterpret_cast<void**>(p);
      allocated_bytes = needed_bytes;
    }
  }

  int depth = absl::GetStackTrace(stack, num_stack, min_dropped_frames + 1);
  for (int i = 0; i < depth; i++) {
    if (symbolize_stacktrace) {
      DumpPCAndSymbol(writer, writer_arg, stack[static_cast<size_t>(i)],
                      "    ");
    } else {
      DumpPC(writer, writer_arg, stack[static_cast<size_t>(i)], "    ");
    }
  }

  auto hook = GetDebugStackTraceHook();
  if (hook != nullptr) {
    hook(stack, depth, nullptr, writer, writer_arg);
  }

  if (allocated_bytes != 0) Deallocate(stack, allocated_bytes);
}

}  
ABSL_NAMESPACE_END
}  
