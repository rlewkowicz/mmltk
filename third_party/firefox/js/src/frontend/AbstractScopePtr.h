/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_AbstractScopePtr_h
#define frontend_AbstractScopePtr_h

#include <type_traits>

#include "frontend/ScopeIndex.h"
#include "vm/ScopeKind.h"  // For ScopeKind

namespace js {
class Scope;
class GlobalScope;
class EvalScope;

namespace frontend {
struct CompilationState;
class ScopeStencil;
}  

class AbstractScopePtr {
 private:
  ScopeIndex index_;

  frontend::CompilationState& compilationState_;

 public:
  friend class js::Scope;

  AbstractScopePtr(frontend::CompilationState& compilationState,
                   ScopeIndex index)
      : index_(index), compilationState_(compilationState) {}

  static AbstractScopePtr compilationEnclosingScope(
      frontend::CompilationState& compilationState) {
    return AbstractScopePtr(compilationState, ScopeIndex::invalid());
  }

 private:
  bool isScopeStencil() const { return index_.isValid(); }

  frontend::ScopeStencil& scopeData() const;

 public:
  template <typename T>
  bool is() const {
    static_assert(std::is_base_of_v<Scope, T>,
                  "Trying to ask about non-Scope type");
    return kind() == T::classScopeKind_;
  }

  ScopeKind kind() const;
  AbstractScopePtr enclosing() const;
  bool hasEnvironment() const;
  bool isArrow() const;

#ifdef DEBUG
  bool hasNonSyntacticScopeOnChain() const;
#endif
};

template <>
inline bool AbstractScopePtr::is<GlobalScope>() const {
  return kind() == ScopeKind::Global || kind() == ScopeKind::NonSyntactic;
}

template <>
inline bool AbstractScopePtr::is<EvalScope>() const {
  return kind() == ScopeKind::Eval || kind() == ScopeKind::StrictEval;
}

}  

#endif  // frontend_AbstractScopePtr_h
