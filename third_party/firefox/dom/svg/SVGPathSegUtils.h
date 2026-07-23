/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_SVG_SVGPATHSEGUTILS_H_
#define DOM_SVG_SVGPATHSEGUTILS_H_

#include "mozilla/Span.h"
#include "mozilla/gfx/Point.h"
#include "mozilla/gfx/Rect.h"

namespace mozilla {

template <typename H, typename V>
struct StyleGenericPosition;

template <typename Position, typename LP>
struct StyleCommandEndPoint;
template <typename T>
using StyleEndPoint = StyleCommandEndPoint<StyleGenericPosition<T, T>, T>;

template <typename Position, typename LP>
struct StyleControlPoint;
template <typename T>
using StyleCurveControlPoint = StyleControlPoint<StyleGenericPosition<T, T>, T>;

template <typename Angle, typename Position, typename LP>
struct StyleGenericShapeCommand;
using StylePathCommand =
    StyleGenericShapeCommand<float, StyleGenericPosition<float, float>, float>;

struct MOZ_STACK_CLASS SVGPathTraversalState {
  using Point = gfx::Point;

  enum class TraversalMode { UpdateAll, UpdateOnlyStartAndCurrentPos };

  bool ShouldUpdateLengthAndControlPoints() const {
    return mode == TraversalMode::UpdateAll;
  }

  Point start;  

  Point pos;  

  Point cp1;  

  Point cp2;  

  float length = 0.0f;

  TraversalMode mode = TraversalMode::UpdateAll;
};

class SVGPathSegUtils {
 private:
  SVGPathSegUtils() = default;  

 public:
  static void TraversePathSegment(const StylePathCommand&,
                                  SVGPathTraversalState&);

  static Maybe<gfx::Rect> SVGPathToAxisAlignedRect(
      Span<const StylePathCommand>);
};

}  

#endif  // DOM_SVG_SVGPATHSEGUTILS_H_
