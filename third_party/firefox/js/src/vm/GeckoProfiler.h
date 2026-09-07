/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GeckoProfiler_h
#define vm_GeckoProfiler_h

#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/RefPtr.h"

#include <stddef.h>
#include <stdint.h>

#include "jspubtd.h"

#include "js/AllocPolicy.h"
#include "js/HashTable.h"
#include "js/ProfilingCategory.h"
#include "js/ProfilingSources.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"
#include "threading/ExclusiveData.h"
#include "threading/ProtectedData.h"


class JS_PUBLIC_API ProfilingStack;

namespace js {

class BaseScript;
class GeckoProfilerThread;
class ScriptSource;

using ProfilerScriptSourceSet =
    HashSet<RefPtr<ScriptSource>, PointerHasher<ScriptSource*>,
            SystemAllocPolicy>;

class GeckoProfilerRuntime {
  JSRuntime* rt;
  RWExclusiveData<ProfilerScriptSourceSet> scriptSources_;
  bool slowAssertions;
  uint32_t enabled_;

 public:
  explicit GeckoProfilerRuntime(JSRuntime* rt);

  bool enabled() const { return enabled_; }
  void enable(bool enabled);
  void enableSlowAssertions(bool enabled) { slowAssertions = enabled; }
  bool slowAssertionsEnabled() { return slowAssertions; }

  static JS::UniqueChars allocProfileString(JSContext* cx, BaseScript* script);
  const char* profileString(JSContext* cx, BaseScript* script);

  size_t stringsCount();
  void stringsReset();

  bool insertScriptSource(ScriptSource* scriptSource) {
    MOZ_ASSERT(scriptSource);
    auto guard = scriptSources_.writeLock();
    if (!enabled_) {
      return true;
    }

    return guard->put(scriptSource);
  }

  js::ProfilerJSSources getProfilerScriptSources(bool gatherSourceText);

  size_t scriptSourcesCount() { return scriptSources_.readLock()->count(); }

  const uint32_t* addressOfEnabled() const { return &enabled_; }
};

class MOZ_RAII GeckoProfilerEntryMarker {
 public:
  explicit MOZ_ALWAYS_INLINE GeckoProfilerEntryMarker(JSContext* cx,
                                                      JSScript* script);
  MOZ_ALWAYS_INLINE ~GeckoProfilerEntryMarker();

 private:
  GeckoProfilerThread* profiler_;
#ifdef DEBUG
  uint32_t spBefore_;
#endif
};

class MOZ_RAII AutoGeckoProfilerEntry {
 public:
  explicit MOZ_ALWAYS_INLINE AutoGeckoProfilerEntry(
      JSContext* cx, const char* label, const char* dynamicString,
      JS::ProfilingCategoryPair categoryPair = JS::ProfilingCategoryPair::JS,
      uint32_t flags = 0);
  explicit MOZ_ALWAYS_INLINE AutoGeckoProfilerEntry(
      JSContext* cx, const char* label,
      JS::ProfilingCategoryPair categoryPair = JS::ProfilingCategoryPair::JS,
      uint32_t flags = 0);
  MOZ_ALWAYS_INLINE ~AutoGeckoProfilerEntry();

 private:
  ProfilingStack* profilingStack_;
#ifdef DEBUG
  GeckoProfilerThread* profiler_;
  uint32_t spBefore_;
#endif
};

class MOZ_RAII AutoJSMethodProfilerEntry : public AutoGeckoProfilerEntry {
 public:
  explicit MOZ_ALWAYS_INLINE AutoJSMethodProfilerEntry(
      JSContext* cx, const char* label, const char* dynamicString = nullptr);
};

class MOZ_RAII AutoJSConstructorProfilerEntry : public AutoGeckoProfilerEntry {
 public:
  explicit MOZ_ALWAYS_INLINE AutoJSConstructorProfilerEntry(JSContext* cx,
                                                            const char* label);
};

class MOZ_RAII GeckoProfilerBaselineOSRMarker {
 public:
  explicit GeckoProfilerBaselineOSRMarker(JSContext* cx, bool hasProfilerFrame);
  ~GeckoProfilerBaselineOSRMarker();

 private:
  GeckoProfilerThread* profiler;
  mozilla::DebugOnly<uint32_t> spBefore_ = 0;
};

class MOZ_RAII AutoSuppressProfilerSampling {
 public:
  explicit AutoSuppressProfilerSampling(JSContext* cx);

  ~AutoSuppressProfilerSampling();

 private:
  JSContext* cx_;
  bool previouslyEnabled_;
};

} 

#endif /* vm_GeckoProfiler_h */
