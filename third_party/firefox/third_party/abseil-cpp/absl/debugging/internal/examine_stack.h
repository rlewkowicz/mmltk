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

#ifndef ABSL_DEBUGGING_INTERNAL_EXAMINE_STACK_H_
#define ABSL_DEBUGGING_INTERNAL_EXAMINE_STACK_H_

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {

typedef void OutputWriter(const char*, void*);

typedef void (*SymbolizeUrlEmitter)(void* const stack[], int depth,
                                    const void* crash_pc, OutputWriter* writer,
                                    void* writer_arg);
typedef void (*SymbolizeUrlEmitterLegacy)(void* const stack[], int depth,
                                          OutputWriter* writer,
                                          void* writer_arg);

void RegisterDebugStackTraceHook(SymbolizeUrlEmitter hook);
SymbolizeUrlEmitter GetDebugStackTraceHook();

SymbolizeUrlEmitterLegacy GetDebugStackTraceHookLegacy();

void* GetProgramCounter(void* const vuc);

void DumpPCAndFrameSizesAndStackTrace(void* const pc, void* const stack[],
                                      int frame_sizes[], int depth,
                                      int min_dropped_frames,
                                      bool symbolize_stacktrace,
                                      OutputWriter* writer, void* writer_arg);

void DumpStackTrace(int min_dropped_frames, int max_num_frames,
                    bool symbolize_stacktrace, OutputWriter* writer,
                    void* writer_arg);

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_DEBUGGING_INTERNAL_EXAMINE_STACK_H_
