/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsXMLPrettyPrinter_h_
#define nsXMLPrettyPrinter_h_

#include "nsCOMPtr.h"
#include "nsStubDocumentObserver.h"

class nsXMLPrettyPrinter : public nsStubDocumentObserver {
 public:
  nsXMLPrettyPrinter();

  NS_DECL_ISUPPORTS

  NS_DECL_NSIMUTATIONOBSERVER_ATTRIBUTECHANGED
  NS_DECL_NSIMUTATIONOBSERVER_CONTENTAPPENDED
  NS_DECL_NSIMUTATIONOBSERVER_CONTENTINSERTED
  NS_DECL_NSIMUTATIONOBSERVER_CONTENTREMOVED
  NS_DECL_NSIMUTATIONOBSERVER_NODEWILLBEDESTROYED

  nsresult PrettyPrint(mozilla::dom::Document* aDocument,
                       bool aShowXSLTDisabledMessage, bool* aDidPrettyPrint);

  void Unhook();

 private:
  virtual ~nsXMLPrettyPrinter();

  void MaybeUnhook(nsIContent* aContent);

  mozilla::dom::Document*
      mDocument;  
  bool mUnhookPending;
};

nsresult NS_NewXMLPrettyPrinter(nsXMLPrettyPrinter** aPrinter);

#endif  // nsXMLPrettyPrinter_h_
