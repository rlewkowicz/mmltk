/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ScriptElement_h
#define mozilla_dom_ScriptElement_h

#include "mozilla/Attributes.h"
#include "nsIScriptElement.h"
#include "nsIScriptLoaderObserver.h"
#include "nsStubMutationObserver.h"

class nsIParser;

namespace mozilla::dom {


class ScriptElement : public nsIScriptElement, public nsStubMutationObserver {
 public:
  NS_DECL_NSISCRIPTLOADEROBSERVER

  NS_DECL_NSIMUTATIONOBSERVER_CHARACTERDATACHANGED
  NS_DECL_NSIMUTATIONOBSERVER_ATTRIBUTECHANGED
  NS_DECL_NSIMUTATIONOBSERVER_CONTENTAPPENDED
  NS_DECL_NSIMUTATIONOBSERVER_CONTENTINSERTED
  NS_DECL_NSIMUTATIONOBSERVER_CONTENTREMOVED

  explicit ScriptElement(FromParser aFromParser)
      : nsIScriptElement(aFromParser) {}

  virtual nsresult FireErrorEvent() override;

  virtual bool GetScriptType(nsAString& aType) override;

 protected:

  virtual bool HasExternalScriptContent() = 0;

  virtual bool MaybeProcessScript(nsCOMPtr<nsIParser> aParser) override;

  virtual MOZ_CAN_RUN_SCRIPT nsresult
  GetTrustedTypesCompliantInlineScriptText(nsString& aSourceText) override;

 private:
  void UpdateTrustWorthiness(MutationEffectOnScript aMutationEffectOnScript);

  bool MaybeProcessScript(const nsAString& aSourceText);
};

}  

#endif  // mozilla_dom_ScriptElement_h
