/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Sink_h
#define jit_Sink_h

namespace js {
namespace jit {

class MIRGenerator;
class MIRGraph;

[[nodiscard]] bool Sink(const MIRGenerator* mir, MIRGraph& graph);

}  
}  

#endif /* jit_Sink_h */
