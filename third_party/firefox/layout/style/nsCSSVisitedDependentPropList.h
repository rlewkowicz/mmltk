/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsCSSVisitedDependentPropList_h_
#define nsCSSVisitedDependentPropList_h_



#define FOR_EACH_VISITED_DEPENDENT_STYLE_STRUCT(MACRO) \
  MACRO(Background, (mBackgroundColor)) \
  MACRO(Border, (mBorderTopColor, \
                 mBorderRightColor, \
                 mBorderBottomColor, \
                 mBorderLeftColor)) \
  MACRO(Outline, (mOutlineColor)) \
  MACRO(Column, (mColumnRuleColor)) \
  MACRO(Text, (mColor)) \
  MACRO(Text, (mTextEmphasisColor, \
               mWebkitTextFillColor, \
               mWebkitTextStrokeColor)) \
  MACRO(TextReset, (mTextDecorationColor)) \
  MACRO(SVG, (mFill, mStroke)) \
  MACRO(UI, (mCaretColor))

#endif // nsCSSVisitedDependentPropList_h_
