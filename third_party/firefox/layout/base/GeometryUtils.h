/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GEOMETRYUTILS_H_
#define MOZILLA_GEOMETRYUTILS_H_

#include "nsCOMPtr.h"
#include "nsTArray.h"


class nsINode;

namespace mozilla {
class ErrorResult;

namespace dom {
struct BoxQuadOptions;
struct ConvertCoordinateOptions;
class DOMQuad;
class DOMRectReadOnly;
class DOMPoint;
struct DOMPointInit;
class OwningTextOrElementOrDocument;
class TextOrElementOrDocument;
enum class CallerType : uint32_t;
}  

typedef dom::TextOrElementOrDocument GeometryNode;
typedef dom::OwningTextOrElementOrDocument OwningGeometryNode;

void GetBoxQuads(nsINode* aNode, const dom::BoxQuadOptions& aOptions,
                 nsTArray<RefPtr<dom::DOMQuad>>& aResult,
                 dom::CallerType aCallerType, ErrorResult& aRv);

void GetBoxQuadsFromWindowOrigin(nsINode* aNode,
                                 const dom::BoxQuadOptions& aOptions,
                                 nsTArray<RefPtr<dom::DOMQuad>>& aResult,
                                 ErrorResult& aRv);

already_AddRefed<dom::DOMQuad> ConvertQuadFromNode(
    nsINode* aTo, dom::DOMQuad& aQuad, const GeometryNode& aFrom,
    const dom::ConvertCoordinateOptions& aOptions, dom::CallerType aCallerType,
    ErrorResult& aRv);

already_AddRefed<dom::DOMQuad> ConvertRectFromNode(
    nsINode* aTo, dom::DOMRectReadOnly& aRect, const GeometryNode& aFrom,
    const dom::ConvertCoordinateOptions& aOptions, dom::CallerType aCallerType,
    ErrorResult& aRv);

already_AddRefed<dom::DOMPoint> ConvertPointFromNode(
    nsINode* aTo, const dom::DOMPointInit& aPoint, const GeometryNode& aFrom,
    const dom::ConvertCoordinateOptions& aOptions, dom::CallerType aCallerType,
    ErrorResult& aRv);

}  

#endif /* MOZILLA_GEOMETRYUTILS_H_ */
