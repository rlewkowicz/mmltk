/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ConcurrentDelazification_h
#define vm_ConcurrentDelazification_h

#include "mozilla/Maybe.h"            // mozilla::Maybe
#include "mozilla/MemoryReporting.h"  // mozilla::MallocSizeOf

#include <stddef.h>  // size_t
#include <utility>   // std::pair

#include "frontend/CompilationStencil.h"  // frontend::{InitialStencilAndDelazifications, CompilationStencil, ScriptStencilRef, CompilationStencilMerger}
#include "frontend/ScriptIndex.h"         // frontend::ScriptIndex
#include "js/AllocPolicy.h"               // SystemAllocPolicy
#include "js/CompileOptions.h"  // JS::PrefableCompileOptions, JS::ReadOnlyCompileOptions
#include "js/experimental/JSStencil.h"  // RefPtrTraits for InitialStencilAndDelazifications
#include "js/UniquePtr.h"               // UniquePtr
#include "js/Vector.h"                  // Vector

namespace js {

class FrontendContext;

struct DelazifyStrategy {
  using ScriptStencilRef = frontend::ScriptStencilRef;
  using ScriptIndex = frontend::ScriptIndex;
  using InitialStencilAndDelazifications =
      frontend::InitialStencilAndDelazifications;

  virtual ~DelazifyStrategy() = default;

  virtual bool done() const = 0;

  virtual ScriptStencilRef next() = 0;

  virtual void clear() = 0;

  [[nodiscard]] virtual bool insert(ScriptStencilRef& ref) = 0;

  [[nodiscard]] bool add(FrontendContext* fc, ScriptStencilRef& ref);
};

struct DepthFirstDelazification final : public DelazifyStrategy {
  Vector<frontend::ScriptStencilRef, 0, SystemAllocPolicy> stack;

  bool done() const override { return stack.empty(); }
  ScriptStencilRef next() override { return stack.popCopy(); }
  void clear() override { return stack.clear(); }
  bool insert(frontend::ScriptStencilRef& ref) override {
    return stack.append(ref);
  }
};

struct LargeFirstDelazification final : public DelazifyStrategy {
  using SourceSize = uint32_t;
  Vector<std::pair<SourceSize, ScriptStencilRef>, 0, SystemAllocPolicy> heap;

  bool done() const override { return heap.empty(); }
  ScriptStencilRef next() override;
  void clear() override { return heap.clear(); }
  bool insert(frontend::ScriptStencilRef&) override;
};

class DelazificationContext {
  const JS::PrefableCompileOptions initialPrefableOptions_;
  using Stencils = frontend::InitialStencilAndDelazifications;

  UniquePtr<DelazifyStrategy> strategy_;

  RefPtr<Stencils> stencils_;
  mozilla::Maybe<Stencils::RelativeIndexesGuard> indexesGuard_;

  FrontendContext fc_;

  size_t stackQuota_;

  bool isInterrupted_ = false;

 public:
  explicit DelazificationContext(
      const JS::PrefableCompileOptions& initialPrefableOptions,
      size_t stackQuota)
      : initialPrefableOptions_(initialPrefableOptions),
        stackQuota_(stackQuota) {}

  bool init(const JS::ReadOnlyCompileOptions& options, Stencils* stencils);
  bool delazify();

  bool isInterrupted() const { return isInterrupted_; }
  void interrupt() { isInterrupted_ = true; }

  bool done() const;

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

} 

#endif /* vm_ConcurrentDelazification_h */
