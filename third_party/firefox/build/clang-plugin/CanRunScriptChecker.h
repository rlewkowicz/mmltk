/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CanRunScriptChecker_h_
#define CanRunScriptChecker_h_

#include "plugin.h"
#include <unordered_set>

class CanRunScriptChecker : public BaseCheck {
public:
  CanRunScriptChecker(StringRef CheckName, ContextType *Context = nullptr)
      : BaseCheck(CheckName, Context) {}
  void registerMatchers(MatchFinder *AstMatcher) override;
  void check(const MatchFinder::MatchResult &Result) override;
  bool isLanguageVersionSupported(const LangOptions &LangOpts) const override {
    return LangOpts.CPlusPlus;
  }

  void onStartOfTranslationUnit() override;

private:
  void buildFuncSet(ASTContext *Context);

  bool IsFuncSetBuilt;
  std::unordered_set<const FunctionDecl *> CanRunScriptFuncs;
};

#endif
