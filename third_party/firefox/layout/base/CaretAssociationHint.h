/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_CaretAssociationHint_h
#define mozilla_CaretAssociationHint_h

namespace mozilla {

template <typename PT, typename CT>
class RangeBoundaryBase;

namespace intl {
class BidiEmbeddingLevel;
};

enum class CaretAssociationHint { Before, After };

template <typename PT, typename CT>
CaretAssociationHint ComputeCaretAssociationHint(
    CaretAssociationHint aDefault, intl::BidiEmbeddingLevel aBidiLevel,
    const RangeBoundaryBase<PT, CT>& aCaretPoint);

}  

#endif
