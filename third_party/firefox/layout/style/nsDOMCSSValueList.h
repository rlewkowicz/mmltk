/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsDOMCSSValueList_h_
#define nsDOMCSSValueList_h_

#include "CSSValue.h"
#include "nsTArray.h"

class nsDOMCSSValueList final : public mozilla::dom::CSSValue {
 public:
  explicit nsDOMCSSValueList(bool aCommaDelimited);

  void AppendCSSValue(already_AddRefed<CSSValue> aValue);

  uint16_t CssValueType() const final { return CSS_VALUE_LIST; }

  uint32_t Length() const { return mCSSValues.Length(); }
  void GetCssText(nsAString&) final;

 protected:
  virtual ~nsDOMCSSValueList();

  bool mCommaDelimited;  

  nsTArray<RefPtr<CSSValue>> mCSSValues;
};

#endif /* nsDOMCSSValueList_h_ */
