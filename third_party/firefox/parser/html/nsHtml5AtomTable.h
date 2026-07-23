/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsHtml5AtomTable_h
#define nsHtml5AtomTable_h

#include "nsHashKeys.h"
#include "nsTHashtable.h"
#include "nsAtom.h"
#include "nsISerialEventTarget.h"

#define RECENTLY_USED_PARSER_ATOMS_SIZE 37

class nsHtml5AtomTable {
 public:
  nsHtml5AtomTable();
  ~nsHtml5AtomTable();

  nsAtom* GetAtom(const nsAString& aKey);

  void Clear() {
    for (auto& mRecentlyUsedParserAtom : mRecentlyUsedParserAtoms) {
      mRecentlyUsedParserAtom = nullptr;
    }
  }

#ifdef DEBUG
  void SetPermittedLookupEventTarget(nsISerialEventTarget* aEventTarget) {
    mPermittedLookupEventTarget = aEventTarget;
  }
#endif

 private:
  RefPtr<nsAtom> mRecentlyUsedParserAtoms[RECENTLY_USED_PARSER_ATOMS_SIZE];
#ifdef DEBUG
  nsCOMPtr<nsISerialEventTarget> mPermittedLookupEventTarget;
#endif
};

#endif  // nsHtml5AtomTable_h
