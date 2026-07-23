/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_WasmSummarizeInsn_h
#define wasm_WasmSummarizeInsn_h

#include "mozilla/Maybe.h"
#include "wasm/WasmCodegenTypes.h"  // TrapMachineInsn

namespace js {
namespace wasm {

#ifdef DEBUG

mozilla::Maybe<TrapMachineInsn> SummarizeTrapInstruction(const uint8_t* insn);

#endif

}  
}  

#endif /* wasm_WasmSummarizeInsn_h */
