/*
 * Copyright 2014 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmSignalHandlers.h"

#include "mozilla/Casting.h"
#include "mozilla/ThreadLocal.h"

#include "threading/Thread.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/Realm.h"
#include "vm/Runtime.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmInstance.h"

#if !defined(__wasi__)
#  include <signal.h>
#endif

using namespace js;
using namespace js::wasm;

#if !defined(JS_CODEGEN_NONE)


#if 0 || defined(__FreeBSD_kernel__)
#    include <sys/ucontext.h>  // for ucontext_t, mcontext_t
#endif

#if defined(__x86_64__)
#if 0 || defined(__FreeBSD_kernel__) || \
        0 || 0
#      include <machine/fpu.h>  // for struct savefpu/fxsave64
#endif
#endif

#if defined(__linux__) || 0
#if defined(__linux__)
#      define EIP_sig(p) ((p)->uc_mcontext.gregs[REG_EIP])
#      define EBP_sig(p) ((p)->uc_mcontext.gregs[REG_EBP])
#      define ESP_sig(p) ((p)->uc_mcontext.gregs[REG_ESP])
#else
#      define EIP_sig(p) ((p)->uc_mcontext.gregs[REG_PC])
#      define EBP_sig(p) ((p)->uc_mcontext.gregs[REG_EBP])
#      define ESP_sig(p) ((p)->uc_mcontext.gregs[REG_ESP])
#endif
#    define RIP_sig(p) ((p)->uc_mcontext.gregs[REG_RIP])
#    define RSP_sig(p) ((p)->uc_mcontext.gregs[REG_RSP])
#    define RBP_sig(p) ((p)->uc_mcontext.gregs[REG_RBP])
#if defined(__linux__) && defined(__arm__)
#      define R11_sig(p) ((p)->uc_mcontext.arm_fp)
#      define R13_sig(p) ((p)->uc_mcontext.arm_sp)
#      define R14_sig(p) ((p)->uc_mcontext.arm_lr)
#      define R15_sig(p) ((p)->uc_mcontext.arm_pc)
#else
#      define R11_sig(p) ((p)->uc_mcontext.gregs[REG_R11])
#      define R13_sig(p) ((p)->uc_mcontext.gregs[REG_R13])
#      define R14_sig(p) ((p)->uc_mcontext.gregs[REG_R14])
#      define R15_sig(p) ((p)->uc_mcontext.gregs[REG_R15])
#endif
#if defined(__linux__) && defined(__aarch64__)
#      define EPC_sig(p) ((p)->uc_mcontext.pc)
#      define RFP_sig(p) ((p)->uc_mcontext.regs[29])
#      define RLR_sig(p) ((p)->uc_mcontext.regs[30])
#      define R31_sig(p) ((p)->uc_mcontext.sp)
#endif
#if defined(__linux__) && defined(__mips__)
#      define EPC_sig(p) ((p)->uc_mcontext.pc)
#      define RFP_sig(p) ((p)->uc_mcontext.gregs[30])
#      define RSP_sig(p) ((p)->uc_mcontext.gregs[29])
#      define R31_sig(p) ((p)->uc_mcontext.gregs[31])
#endif
#if defined(__linux__) && (defined(__sparc__) && defined(__arch64__))
#      define PC_sig(p) ((p)->uc_mcontext.mc_gregs[MC_PC])
#      define FP_sig(p) ((p)->uc_mcontext.mc_fp)
#      define SP_sig(p) ((p)->uc_mcontext.mc_i7)
#endif
#if defined(__linux__) && (defined(__ppc64__) || defined(__PPC64__) || \
                               defined(__ppc64le__) || defined(__PPC64LE__))
#      define R01_sig(p) ((p)->uc_mcontext.gp_regs[1])
#      define R32_sig(p) ((p)->uc_mcontext.gp_regs[32])
#endif
#if defined(__linux__) && defined(__loongarch__)
#      define EPC_sig(p) ((p)->uc_mcontext.__pc)
#      define RRA_sig(p) ((p)->uc_mcontext.__gregs[1])
#      define R03_sig(p) ((p)->uc_mcontext.__gregs[3])
#      define RFP_sig(p) ((p)->uc_mcontext.__gregs[22])
#endif
#if defined(__linux__) && defined(__riscv)
#      define RPC_sig(p) ((p)->uc_mcontext.__gregs[REG_PC])
#      define RRA_sig(p) ((p)->uc_mcontext.__gregs[REG_RA])
#      define RFP_sig(p) ((p)->uc_mcontext.__gregs[REG_S0])
#      define R02_sig(p) ((p)->uc_mcontext.__gregs[REG_SP])
#endif
#elif 0 || 0 || \
      defined(__FreeBSD_kernel__)
#    define EIP_sig(p) ((p)->uc_mcontext.mc_eip)
#    define EBP_sig(p) ((p)->uc_mcontext.mc_ebp)
#    define ESP_sig(p) ((p)->uc_mcontext.mc_esp)
#    define RIP_sig(p) ((p)->uc_mcontext.mc_rip)
#    define RSP_sig(p) ((p)->uc_mcontext.mc_rsp)
#    define RBP_sig(p) ((p)->uc_mcontext.mc_rbp)
#      define R11_sig(p) ((p)->uc_mcontext.mc_r11)
#      define R13_sig(p) ((p)->uc_mcontext.mc_r13)
#      define R14_sig(p) ((p)->uc_mcontext.mc_r14)
#      define R15_sig(p) ((p)->uc_mcontext.mc_r15)
#else
#    error \
        "Don't know how to read/write to the thread state via the mcontext_t."
#endif


#    define CONTEXT ucontext_t

#if defined(_M_X64) || defined(__x86_64__)
#    define PC_sig(p) RIP_sig(p)
#    define FP_sig(p) RBP_sig(p)
#    define SP_sig(p) RSP_sig(p)
#elif defined(_M_IX86) || defined(__i386__)
#    define PC_sig(p) EIP_sig(p)
#    define FP_sig(p) EBP_sig(p)
#    define SP_sig(p) ESP_sig(p)
#elif defined(__arm__)
#    define FP_sig(p) R11_sig(p)
#    define SP_sig(p) R13_sig(p)
#    define LR_sig(p) R14_sig(p)
#    define PC_sig(p) R15_sig(p)
#elif defined(_M_ARM64) || defined(__aarch64__)
#    define PC_sig(p) EPC_sig(p)
#    define FP_sig(p) RFP_sig(p)
#    define SP_sig(p) R31_sig(p)
#    define LR_sig(p) RLR_sig(p)
#elif defined(__mips__)
#    define PC_sig(p) EPC_sig(p)
#    define FP_sig(p) RFP_sig(p)
#    define SP_sig(p) RSP_sig(p)
#    define LR_sig(p) R31_sig(p)
#elif defined(__ppc64__) || defined(__PPC64__) || defined(__ppc64le__) || \
      defined(__PPC64LE__)
#    define PC_sig(p) R32_sig(p)
#    define SP_sig(p) R01_sig(p)
#    define FP_sig(p) R01_sig(p)
#elif defined(__loongarch__)
#    define PC_sig(p) EPC_sig(p)
#    define FP_sig(p) RFP_sig(p)
#    define SP_sig(p) R03_sig(p)
#    define LR_sig(p) RRA_sig(p)
#elif defined(__riscv)
#    define PC_sig(p) RPC_sig(p)
#    define FP_sig(p) RFP_sig(p)
#    define SP_sig(p) R02_sig(p)
#    define LR_sig(p) RRA_sig(p)
#endif

static void SetContextPC(CONTEXT* context, uint8_t* pc) {
#if defined(PC_sig)
  *mozilla::BitwiseCast<uint8_t**>(&PC_sig(context)) = pc;
#else
  MOZ_CRASH();
#endif
}

static uint8_t* ContextToPC(CONTEXT* context) {
#if defined(PC_sig)
  return mozilla::BitwiseCast<uint8_t*>(PC_sig(context));
#else
  MOZ_CRASH();
#endif
}

static uint8_t* ContextToFP(CONTEXT* context) {
#if defined(FP_sig)
  return mozilla::BitwiseCast<uint8_t*>(FP_sig(context));
#else
  MOZ_CRASH();
#endif
}

static uint8_t* ContextToSP(CONTEXT* context) {
#if defined(SP_sig)
  return mozilla::BitwiseCast<uint8_t*>(SP_sig(context));
#else
  MOZ_CRASH();
#endif
}

#if defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
      defined(__loongarch__) || defined(__riscv)
static uint8_t* ContextToLR(CONTEXT* context) {
#if defined(LR_sig)
  return mozilla::BitwiseCast<uint8_t*>(LR_sig(context));
#else
  MOZ_CRASH();
#endif
}
#endif

static JS::ProfilingFrameIterator::RegisterState ToRegisterState(
    CONTEXT* context) {
  JS::ProfilingFrameIterator::RegisterState state;
  state.fp = ContextToFP(context);
  state.pc = ContextToPC(context);
  state.sp = ContextToSP(context);
#if defined(__arm__) || defined(__aarch64__) || defined(__mips__) || \
      defined(__loongarch__) || defined(__riscv)
  state.lr = ContextToLR(context);
#else
  state.lr = (void*)UINTPTR_MAX;
#endif
  return state;
}


static MOZ_THREAD_LOCAL(bool) sAlreadyHandlingTrap;

struct AutoHandlingTrap {
  AutoHandlingTrap() {
    MOZ_ASSERT(!sAlreadyHandlingTrap.get());
    sAlreadyHandlingTrap.set(true);
  }

  ~AutoHandlingTrap() {
    MOZ_ASSERT(sAlreadyHandlingTrap.get());
    sAlreadyHandlingTrap.set(false);
  }
};

[[nodiscard]] static bool HandleTrap(CONTEXT* context,
                                     uint8_t* faultAddr = nullptr,
                                     JSContext* assertCx = nullptr) {
  MOZ_ASSERT(sAlreadyHandlingTrap.get());

  uint8_t* pc = ContextToPC(context);
  const CodeBlock* codeBlock = LookupCodeBlock(pc);
  if (!codeBlock) {
    return false;
  }

  Trap trap;
  TrapSite trapSite;
  if (!codeBlock->lookupTrap(pc, &trap, &trapSite)) {
    return false;
  }


  auto* frame = reinterpret_cast<Frame*>(ContextToFP(context));
  Instance* instance = GetNearestEffectiveInstance(frame);
  MOZ_RELEASE_ASSERT(&instance->code() == codeBlock->code ||
                     trap == Trap::IndirectCallBadSig);

  uint32_t faultMemoryIndex = 0;
  uint64_t faultByteOffset = 0;
  if (trap == Trap::OutOfBounds && faultAddr) {
    if (!instance->memoryAccessInMappedRegion(faultAddr, &faultMemoryIndex,
                                              &faultByteOffset)) {
      return false;
    }
  }

  ((FrameWithInstances*)frame)->setCalleeInstance(instance);

  JSContext* cx =
      instance->realm()->runtimeFromAnyThread()->mainContextFromAnyThread();
  MOZ_RELEASE_ASSERT(!assertCx || cx == assertCx);

  jit::JitActivation* activation = cx->activation()->asJit();
  activation->startWasmTrap(trap, trapSite, ToRegisterState(context));
  if (trap == Trap::OutOfBounds && faultAddr) {
    activation->setWasmTrapFaultInfo(faultMemoryIndex, faultByteOffset);
  }
  SetContextPC(context, codeBlock->code->trapCode());
  return true;
}



#if defined(__mips__) || defined(__loongarch__)
static const uint32_t kWasmTrapSignal = SIGFPE;
#else
static const uint32_t kWasmTrapSignal = SIGILL;
#endif

static struct sigaction sPrevSEGVHandler;
static struct sigaction sPrevSIGBUSHandler;
static struct sigaction sPrevWasmTrapHandler;

typedef void (*sa_sigaction_t)(int, siginfo_t*, void*);

#    define SIG_ACTION_DFL ((sa_sigaction_t)SIG_DFL)
#    define SIG_ACTION_IGN ((sa_sigaction_t)SIG_IGN)

static void WasmTrapHandler(int signum, siginfo_t* info, void* context) {
  if (!sAlreadyHandlingTrap.get()) {
    AutoHandlingTrap aht;
    MOZ_RELEASE_ASSERT(signum == SIGSEGV || signum == SIGBUS ||
                       signum == kWasmTrapSignal);
    uint8_t* faultAddr = nullptr;
    if (signum == SIGSEGV || signum == SIGBUS) {
      faultAddr = (uint8_t*)info->si_addr;
    }
    JSContext* cx = TlsContext.get();  
    if (HandleTrap((CONTEXT*)context, faultAddr, cx)) {
      return;
    }
  }

  struct sigaction* previousSignal = nullptr;
  switch (signum) {
    case SIGSEGV:
      previousSignal = &sPrevSEGVHandler;
      break;
    case SIGBUS:
      previousSignal = &sPrevSIGBUSHandler;
      break;
    case kWasmTrapSignal:
      previousSignal = &sPrevWasmTrapHandler;
      break;
  }
  MOZ_ASSERT(previousSignal);

  // function, but fallthrough.
  if ((previousSignal->sa_flags & SA_SIGINFO) &&
      previousSignal->sa_sigaction != SIG_ACTION_DFL &&
      previousSignal->sa_sigaction != SIG_ACTION_IGN) {
    previousSignal->sa_sigaction(signum, info, context);
  } else if (previousSignal->sa_handler == SIG_DFL ||
             previousSignal->sa_handler == SIG_IGN) {
    sigaction(signum, previousSignal, nullptr);
  } else {
    previousSignal->sa_handler(signum);
  }
}

struct InstallState {
  bool tried;
  bool success;
  InstallState() : tried(false), success(false) {}
};

MOZ_RUNINIT static ExclusiveData<InstallState> sEagerInstallState(
    mutexid::WasmSignalInstallState);

#endif

void wasm::EnsureEagerProcessSignalHandlers() {
#if defined(JS_CODEGEN_NONE)
  return;
#else
  auto eagerInstallState = sEagerInstallState.lock();
  if (eagerInstallState->tried) {
    return;
  }

  eagerInstallState->tried = true;
  MOZ_RELEASE_ASSERT(eagerInstallState->success == false);

  sAlreadyHandlingTrap.infallibleInit();

  // handling the signal, and fall through to the Breakpad handler by testing

  struct sigaction faultHandler;
  faultHandler.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
  faultHandler.sa_sigaction = WasmTrapHandler;
  sigemptyset(&faultHandler.sa_mask);
  if (sigaction(SIGSEGV, &faultHandler, &sPrevSEGVHandler)) {
    MOZ_CRASH("unable to install segv handler");
  }

#if defined(JS_CODEGEN_ARM)
  struct sigaction busHandler;
  busHandler.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
  busHandler.sa_sigaction = WasmTrapHandler;
  sigemptyset(&busHandler.sa_mask);
  if (sigaction(SIGBUS, &busHandler, &sPrevSIGBUSHandler)) {
    MOZ_CRASH("unable to install sigbus handler");
  }
#endif

  struct sigaction wasmTrapHandler;
  wasmTrapHandler.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
  wasmTrapHandler.sa_sigaction = WasmTrapHandler;
  sigemptyset(&wasmTrapHandler.sa_mask);
  if (sigaction(kWasmTrapSignal, &wasmTrapHandler, &sPrevWasmTrapHandler)) {
    MOZ_CRASH("unable to install wasm trap handler");
  }

  eagerInstallState->success = true;
#endif
}

#if !defined(JS_CODEGEN_NONE)
MOZ_RUNINIT static ExclusiveData<InstallState> sLazyInstallState(
    mutexid::WasmSignalInstallState);

static bool EnsureLazyProcessSignalHandlers() {
  auto lazyInstallState = sLazyInstallState.lock();
  if (lazyInstallState->tried) {
    return lazyInstallState->success;
  }

  lazyInstallState->tried = true;
  MOZ_RELEASE_ASSERT(lazyInstallState->success == false);


  lazyInstallState->success = true;
  return true;
}
#endif

bool wasm::EnsureFullSignalHandlers(JSContext* cx) {
#if defined(JS_CODEGEN_NONE)
  return false;
#else
  if (cx->wasm().triedToInstallSignalHandlers) {
    return cx->wasm().haveSignalHandlers;
  }

  cx->wasm().triedToInstallSignalHandlers = true;
  MOZ_RELEASE_ASSERT(!cx->wasm().haveSignalHandlers);

  {
    auto eagerInstallState = sEagerInstallState.lock();
    MOZ_RELEASE_ASSERT(eagerInstallState->tried);
    if (!eagerInstallState->success) {
      return false;
    }
  }

  if (!EnsureLazyProcessSignalHandlers()) {
    return false;
  }


  cx->wasm().haveSignalHandlers = true;
  return true;
#endif
}

bool wasm::MemoryAccessTraps(const RegisterState& regs, uint8_t* addr,
                             uint32_t numBytes, uint8_t** newPC) {
#if defined(JS_CODEGEN_NONE)
  return false;
#else
  const wasm::CodeBlock* codeBlock = wasm::LookupCodeBlock(regs.pc);
  if (!codeBlock) {
    return false;
  }

  Trap trap;
  TrapSite trapSite;
  if (!codeBlock->code->lookupTrap(regs.pc, &trap, &trapSite)) {
    return false;
  }
  switch (trap) {
    case Trap::OutOfBounds:
      break;
    case Trap::NullPointerDereference:
    case Trap::BadCast:
      if ((uintptr_t)addr >= NullPtrGuardSize) {
        return false;
      }
      break;
#if defined(WASM_HAS_HEAPREG)
    case Trap::IndirectCallToNull:
      if (addr !=
          reinterpret_cast<uint8_t*>(wasm::Instance::offsetOfMemory0Base())) {
        return false;
      }
      break;
#endif
    default:
      return false;
  }

  FrameWithInstances* frame = (FrameWithInstances*)(regs.fp);
  Instance& instance = *GetNearestEffectiveInstance(frame);
  MOZ_ASSERT(&instance.code() == codeBlock->code);

  frame->setCalleeInstance(&instance);

  uint32_t faultMemoryIndex = 0;
  uint64_t faultByteOffset = 0;
  switch (trap) {
    case Trap::OutOfBounds:
      if (!instance.memoryAccessInGuardRegion((uint8_t*)addr, numBytes)) {
        return false;
      }
      MOZ_ALWAYS_TRUE(instance.memoryAccessInMappedRegion(
          (uint8_t*)addr, &faultMemoryIndex, &faultByteOffset));
      break;
    case Trap::NullPointerDereference:
    case Trap::BadCast:
#if defined(WASM_HAS_HEAPREG)
    case Trap::IndirectCallToNull:
#endif
      break;
    default:
      MOZ_CRASH("Should not happen");
  }

  JSContext* cx = TlsContext.get();  
  jit::JitActivation* activation = cx->activation()->asJit();
  activation->startWasmTrap(trap, trapSite, regs);
  if (trap == Trap::OutOfBounds) {
    activation->setWasmTrapFaultInfo(faultMemoryIndex, faultByteOffset);
  }
  *newPC = codeBlock->code->trapCode();
  return true;
#endif
}

bool wasm::HandleIllegalInstruction(const RegisterState& regs,
                                    uint8_t** newPC) {
#if defined(JS_CODEGEN_NONE)
  return false;
#else
  const wasm::CodeBlock* codeBlock = wasm::LookupCodeBlock(regs.pc);
  if (!codeBlock) {
    return false;
  }

  Trap trap;
  TrapSite trapSite;
  if (!codeBlock->code->lookupTrap(regs.pc, &trap, &trapSite)) {
    return false;
  }

  FrameWithInstances* frame = (FrameWithInstances*)(regs.fp);
  Instance& instance = *GetNearestEffectiveInstance(frame);
  MOZ_ASSERT(&instance.code() == codeBlock->code);

  frame->setCalleeInstance(&instance);

  JSContext* cx = TlsContext.get();  
  jit::JitActivation* activation = cx->activation()->asJit();
  activation->startWasmTrap(trap, trapSite, regs);
  *newPC = codeBlock->code->trapCode();
  return true;
#endif
}
