/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitHints_inl_h
#define jit_JitHints_inl_h

#include "jit/JitHints.h"
#include "mozilla/HashFunctions.h"

namespace js::jit {

inline JitHintsMap::ScriptKey JitHintsMap::getScriptKey(
    JSScript* script) const {
  ScriptKey filenameHash = script->filenameHash();
  if (filenameHash && !script->scriptSource()->hasIntroducerFilename()) {
    return mozilla::AddToHash(filenameHash, script->sourceStart());
  }
  return 0;
}

inline void JitHintsMap::incrementBaselineEntryCount() {
  if (++baselineEntryCount_ > MaxEntries_) {
    baselineHintMap_.clear();
    baselineEntryCount_ = 0;
  }
}

inline void JitHintsMap::setEagerBaselineHint(JSScript* script) {
  ScriptKey key = getScriptKey(script);
  if (!key) {
    return;
  }

  if (baselineHintMap_.mightContain(key)) {
    return;
  }

  incrementBaselineEntryCount();

  script->setNoEagerBaselineHint(false);
  baselineHintMap_.add(key);
}

inline bool JitHintsMap::mightHaveEagerBaselineHint(JSScript* script) const {
  if (ScriptKey key = getScriptKey(script)) {
    return baselineHintMap_.mightContain(key);
  }
  script->setNoEagerBaselineHint(true);
  return false;
}

}  

#endif /* jit_JitHints_inl_h */
