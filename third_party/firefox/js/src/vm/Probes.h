/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Probes_h
#define vm_Probes_h

#include "vm/JSObject.h"

namespace js {

class InterpreterFrame;

namespace probes {


extern bool ProfilingActive;

bool EnterScript(JSContext*, JSScript*, JSFunction*, InterpreterFrame*);

void ExitScript(JSContext*, JSScript*, JSFunction*, bool popProfilerFrame);

bool CreateObject(JSContext* cx, JSObject* obj);

bool FinalizeObject(JSObject* obj);
}  

inline bool probes::CreateObject(JSContext* cx, JSObject* obj) { return true; }

inline bool probes::FinalizeObject(JSObject* obj) { return true; }

} 

#endif /* vm_Probes_h */
