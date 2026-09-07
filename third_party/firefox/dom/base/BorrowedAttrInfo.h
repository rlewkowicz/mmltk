/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BorrowedAttrInfo_h_
#define BorrowedAttrInfo_h_

class nsAttrName;
class nsAttrValue;

namespace mozilla::dom {

struct BorrowedAttrInfo {
  BorrowedAttrInfo() : mName(nullptr), mValue(nullptr) {}

  BorrowedAttrInfo(const nsAttrName* aName, const nsAttrValue* aValue);

  BorrowedAttrInfo(const BorrowedAttrInfo& aOther);

  const nsAttrName* mName;
  const nsAttrValue* mValue;

  explicit operator bool() const { return mName != nullptr; }
};

}  
#endif
