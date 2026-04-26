import type { AnnotationOverlayRectPrimitive } from "../../src/app/workspace/annotation-scene-overlay";

export function makeTestOverlayRectPrimitive(
  overrides: Partial<AnnotationOverlayRectPrimitive> = {},
): AnnotationOverlayRectPrimitive {
  return {
    kind: "rect",
    x: 32,
    y: 24,
    width: 128,
    height: 96,
    strokeStyle: "rgba(255, 200, 32, 1)",
    lineWidth: 2,
    fillStyle: null,
    dash: [],
    ...overrides,
  };
}
