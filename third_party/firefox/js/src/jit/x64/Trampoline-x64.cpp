/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/CalleeToken.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/PerfSpewer.h"
#include "jit/VMFunctions.h"
#include "jit/x64/SharedICRegisters-x64.h"
#include "vm/JitActivation.h"  // js::jit::JitActivation
#include "vm/JSContext.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

struct EnterJITStackEntry {
  static constexpr int32_t offsetFromFP() {
    return -int32_t(offsetof(EnterJITStackEntry, rbp));
  }

  void* result;


  void* r15;
  void* r14;
  void* r13;
  void* r12;
  void* rbx;
  void* rbp;

  void* rip;
};

static const LiveRegisterSet AllRegs =
    LiveRegisterSet(GeneralRegisterSet(Registers::AllMask),
                    FloatRegisterSet(FloatRegisters::AllMask));

void JitRuntime::generateEnterJIT(JSContext* cx, MacroAssembler& masm) {
  AutoCreatedBy acb(masm, "JitRuntime::generateEnterJIT");

  enterJITOffset_ = startTrampolineCode(masm);

  masm.assertStackAlignment(ABIStackAlignment,
                            -int32_t(sizeof(uintptr_t)) );

  const Register reg_code = IntArgReg0;
  const Register reg_argc = IntArgReg1;
  const Register reg_argv = IntArgReg2;
  static_assert(OsrFrameReg == IntArgReg3);

  const Register token = IntArgReg4;
  const Register scopeChain = IntArgReg5;
  const Operand numStackValuesAddr = Operand(rbp, 16 + ShadowStackSpace);
  const Operand result = Operand(rbp, 24 + ShadowStackSpace);


  masm.push(rbp);
  masm.mov(rsp, rbp);

  masm.push(rbx);
  masm.push(r12);
  masm.push(r13);
  masm.push(r14);
  masm.push(r15);

  masm.push(result);


  masm.movq(token, r12);
  generateEnterJitShared(masm, reg_argc, reg_argv, r12, r13, r14, r15);

  masm.movq(result, reg_argc);
  masm.unboxInt32(Operand(reg_argc, 0), reg_argc);
  masm.pushFrameDescriptorForJitCall(FrameType::CppToJSJit, reg_argc, reg_argc);

  CodeLabel returnLabel;
  Label oomReturnLabel;
  {
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    MOZ_ASSERT(!regs.has(rbp));
    regs.take(OsrFrameReg);
    regs.take(reg_code);

    Register scratch = regs.takeAny();

    Label notOsr;
    masm.branchTestPtr(Assembler::Zero, OsrFrameReg, OsrFrameReg, &notOsr);

    Register numStackValues = regs.takeAny();
    masm.movq(numStackValuesAddr, numStackValues);

    masm.mov(&returnLabel, scratch);
    masm.push(scratch);

    masm.push(rbp);
    masm.mov(rsp, rbp);

    masm.subPtr(Imm32(BaselineFrame::Size()), rsp);

    Register framePtrScratch = regs.takeAny();
    masm.touchFrameValues(numStackValues, scratch, framePtrScratch);
    masm.mov(rsp, framePtrScratch);

    Register valuesSize = regs.takeAny();
    masm.mov(numStackValues, valuesSize);
    masm.shll(Imm32(3), valuesSize);
    masm.subPtr(valuesSize, rsp);

    masm.push(FrameDescriptor(FrameType::BaselineJS));
    masm.push(Imm32(0));  
    masm.push(FramePointer);
    masm.loadJSContext(scratch);
    masm.enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

    regs.add(valuesSize);

    masm.push(reg_code);

    using Fn = void (*)(BaselineFrame* frame, InterpreterFrame* interpFrame,
                        uint32_t numStackValues);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(framePtrScratch);  
    masm.passABIArg(OsrFrameReg);      
    masm.passABIArg(numStackValues);
    masm.callWithABI<Fn, jit::InitBaselineFrameForOsr>(
        ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    masm.pop(reg_code);

    MOZ_ASSERT(reg_code != ReturnReg);

    masm.addPtr(Imm32(ExitFrameLayout::SizeWithFooter()), rsp);

    {
      Label skipProfilingInstrumentation;
      AbsoluteAddress addressOfEnabled(
          cx->runtime()->geckoProfiler().addressOfEnabled());
      masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                    &skipProfilingInstrumentation);
      masm.profilerEnterFrame(rbp, scratch);
      masm.bind(&skipProfilingInstrumentation);
    }

    masm.jump(reg_code);

    masm.bind(&notOsr);
    masm.movq(scopeChain, R1.scratchReg());
  }

  masm.assertStackAlignment(JitStackAlignment, 2 * sizeof(uintptr_t));

  masm.callJitNoProfiler(reg_code);

  {
    masm.bind(&returnLabel);
    masm.addCodeLabel(returnLabel);
    masm.bind(&oomReturnLabel);
  }

  masm.lea(Operand(rbp, EnterJITStackEntry::offsetFromFP()), rsp);

  masm.pop(r12);  
  masm.storeValue(JSReturnOperand, Operand(r12, 0));

  masm.pop(r15);
  masm.pop(r14);
  masm.pop(r13);
  masm.pop(r12);
  masm.pop(rbx);

  masm.pop(rbp);
  masm.ret();
}

mozilla::Maybe<::JS::ProfilingFrameIterator::RegisterState>
JitRuntime::getCppEntryRegisters(JitFrameLayout* frameStackAddress) {
  if (frameStackAddress->prevType() != FrameType::CppToJSJit) {
    return mozilla::Nothing{};
  }

  uint8_t* fp = frameStackAddress->callerFramePtr();
  auto* enterJITStackEntry = reinterpret_cast<EnterJITStackEntry*>(
      fp + EnterJITStackEntry::offsetFromFP());

  ::JS::ProfilingFrameIterator::RegisterState registerState;
  registerState.fp = enterJITStackEntry->rbp;
  registerState.pc = enterJITStackEntry->rip;
  registerState.sp = &enterJITStackEntry->rip + 1;
  registerState.lr = nullptr;
  return mozilla::Some(registerState);
}

static void DumpAllRegs(MacroAssembler& masm) {
#if defined(ENABLE_WASM_SIMD)
  masm.PushRegsInMask(AllRegs);
#else
  for (GeneralRegisterBackwardIterator iter(AllRegs.gprs()); iter.more();
       ++iter) {
    masm.Push(*iter);
  }

  masm.reserveStack(sizeof(RegisterDump::FPUArray));
  for (FloatRegisterBackwardIterator iter(AllRegs.fpus()); iter.more();
       ++iter) {
    FloatRegister reg = *iter;
    Address spillAddress(StackPointer, reg.getRegisterDumpOffsetInBytes());
    masm.storeDouble(reg, spillAddress);
  }
#endif
}

void JitRuntime::generateInvalidator(MacroAssembler& masm, Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateInvalidator");


  invalidatorOffset_ = startTrampolineCode(masm);

  DumpAllRegs(masm);

  masm.movq(rsp, rax);  

  masm.reserveStack(sizeof(void*));
  masm.movq(rsp, rbx);

  using Fn = bool (*)(InvalidationBailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupUnalignedABICall(rdx);
  masm.passABIArg(rax);
  masm.passABIArg(rbx);
  masm.callWithABI<Fn, InvalidationBailout>(
      ABIType::General, CheckUnsafeCallWithABI::DontCheckOther);

  masm.pop(r9);  

  masm.moveToStackPtr(FramePointer);

  masm.jmp(bailoutTail);
}

static void PushBailoutFrame(MacroAssembler& masm, Register spArg) {
  DumpAllRegs(masm);

  masm.movq(rsp, spArg);
}

static void GenerateBailoutThunk(MacroAssembler& masm, Label* bailoutTail) {
  PushBailoutFrame(masm, r8);

  masm.reserveStack(sizeof(void*));
  masm.movq(rsp, r9);

  using Fn = bool (*)(BailoutStack* sp, BaselineBailoutInfo** info);
  masm.setupUnalignedABICall(rax);
  masm.passABIArg(r8);
  masm.passABIArg(r9);
  masm.callWithABI<Fn, Bailout>(ABIType::General,
                                CheckUnsafeCallWithABI::DontCheckOther);

  masm.pop(r9);  

  masm.moveToStackPtr(FramePointer);

  masm.jmp(bailoutTail);
}

void JitRuntime::generateBailoutHandler(MacroAssembler& masm,
                                        Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateBailoutHandler");

  bailoutHandlerOffset_ = startTrampolineCode(masm);

  GenerateBailoutThunk(masm, bailoutTail);
}

bool JitRuntime::generateVMWrapper(JSContext* cx, MacroAssembler& masm,
                                   VMFunctionId id, const VMFunctionData& f,
                                   DynFn nativeFun, uint32_t* wrapperOffset) {
  AutoCreatedBy acb(masm, "JitRuntime::generateVMWrapper");

  *wrapperOffset = startTrampolineCode(masm);

  AllocatableGeneralRegisterSet regs(Register::Codes::WrapperMask);

  static_assert(
      (Register::Codes::VolatileMask & ~Register::Codes::WrapperMask) == 0,
      "Wrapper register set must be a superset of Volatile register set");

  Register cxreg = IntArgReg0;
  regs.take(cxreg);

  masm.Push(FramePointer);
  masm.moveStackPtrTo(FramePointer);
  masm.loadJSContext(cxreg);
  masm.enterExitFrame(cxreg, regs.getAny(), id);

  masm.reserveVMFunctionOutParamSpace(f);

  masm.setupUnalignedABICallDontSaveRestoreSP();
  masm.passABIArg(cxreg);

  size_t argDisp = ExitFrameLayout::Size();

  for (uint32_t explicitArg = 0; explicitArg < f.explicitArgs; explicitArg++) {
    switch (f.argProperties(explicitArg)) {
      case VMFunctionData::WordByValue:
        if (f.argPassedInFloatReg(explicitArg)) {
          masm.passABIArg(MoveOperand(FramePointer, argDisp), ABIType::Float64);
        } else {
          masm.passABIArg(MoveOperand(FramePointer, argDisp), ABIType::General);
        }
        argDisp += sizeof(void*);
        break;
      case VMFunctionData::WordByRef:
        masm.passABIArg(MoveOperand(FramePointer, argDisp,
                                    MoveOperand::Kind::EffectiveAddress),
                        ABIType::General);
        argDisp += sizeof(void*);
        break;
      case VMFunctionData::DoubleByValue:
      case VMFunctionData::DoubleByRef:
        MOZ_CRASH("NYI: x64 callVM should not be used with 128bits values.");
    }
  }

  const int32_t outParamOffset =
      -int32_t(ExitFooterFrame::Size()) - f.sizeOfOutParamStackSlot();
  if (f.outParam != Type_Void) {
    masm.passABIArg(MoveOperand(FramePointer, outParamOffset,
                                MoveOperand::Kind::EffectiveAddress),
                    ABIType::General);
  }

  masm.callWithABI(nativeFun, ABIType::General,
                   CheckUnsafeCallWithABI::DontCheckHasExitFrame);

  switch (f.failType()) {
    case Type_Cell:
      masm.branchTestPtr(Assembler::Zero, rax, rax, masm.failureLabel());
      break;
    case Type_Bool:
      masm.testb(rax, rax);
      masm.j(Assembler::Zero, masm.failureLabel());
      break;
    case Type_Void:
      break;
    default:
      MOZ_CRASH("unknown failure kind");
  }

  masm.loadVMFunctionOutParam(f, Address(FramePointer, outParamOffset));

  if (f.returnsData() && JitOptions.spectreJitToCxxCalls) {
    masm.speculationBarrier();
  }

  masm.moveToStackPtr(FramePointer);
  masm.pop(FramePointer);

  masm.retn(Imm32(sizeof(ExitFrameLayout) - sizeof(void*) +
                  f.explicitStackSlots() * sizeof(void*) +
                  f.extraValuesToPop * sizeof(Value)));

  return true;
}

uint32_t JitRuntime::generatePreBarrier(JSContext* cx, MacroAssembler& masm,
                                        MIRType type) {
  AutoCreatedBy acb(masm, "JitRuntime::generatePreBarrier");

  uint32_t offset = startTrampolineCode(masm);

  static_assert(PreBarrierReg == rdx);
  Register temp1 = rax;
  Register temp2 = rbx;
  Register temp3 = rcx;
  masm.push(temp1);
  masm.push(temp2);
  masm.push(temp3);

  Label noBarrier;
  masm.emitPreBarrierFastPath(type, temp1, temp2, temp3, &noBarrier);

  masm.pop(temp3);
  masm.pop(temp2);
  masm.pop(temp1);

  LiveRegisterSet regs =
      LiveRegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                      FloatRegisterSet(FloatRegisters::VolatileMask));
  masm.PushRegsInMask(regs);

  masm.mov(ImmPtr(cx->runtime()), rcx);

  masm.setupUnalignedABICall(rax);
  masm.passABIArg(rcx);
  masm.passABIArg(rdx);
  masm.callWithABI(JitPreWriteBarrier(type));

  masm.PopRegsInMask(regs);
  masm.ret();

  masm.bind(&noBarrier);
  masm.pop(temp3);
  masm.pop(temp2);
  masm.pop(temp1);
  masm.ret();

  return offset;
}

void JitRuntime::generateBailoutTailStub(MacroAssembler& masm,
                                         Label* bailoutTail) {
  AutoCreatedBy acb(masm, "JitRuntime::generateBailoutTailStub");

  masm.bind(bailoutTail);
  masm.generateBailoutTail(rdx, r9);
}
