import type { CaptureRegion, WorkspaceGeometry } from "../../host_api";
import type {
  AnnotationWorkspaceBox,
  AnnotationWorkspaceBrushPreviewState,
  AnnotationWorkspaceDenseMaskState,
  AnnotationWorkspaceEditableHandleState,
  AnnotationWorkspaceHandleRole,
  AnnotationWorkspaceObjectState,
  AnnotationWorkspacePoint,
  AnnotationWorkspaceSceneState,
} from "../../browser_shell_state";

export interface AnnotationOverlayPoint {
  x: number;
  y: number;
}

export interface AnnotationOverlayRectPrimitive {
  kind: "rect";
  x: number;
  y: number;
  width: number;
  height: number;
  strokeStyle: string;
  lineWidth: number;
  fillStyle: string | null;
  dash: readonly number[];
}

export interface AnnotationOverlayPolylinePrimitive {
  kind: "polyline";
  points: AnnotationOverlayPoint[];
  closed: boolean;
  strokeStyle: string;
  lineWidth: number;
  fillStyle: string | null;
  dash: readonly number[];
}

export interface AnnotationOverlaySegmentsPrimitive {
  kind: "segments";
  segments: Array<{
    start: AnnotationOverlayPoint;
    end: AnnotationOverlayPoint;
  }>;
  strokeStyle: string;
  lineWidth: number;
  dash: readonly number[];
}

export interface AnnotationOverlayMarkersPrimitive {
  kind: "markers";
  points: AnnotationOverlayPoint[];
  radius: number;
  fillStyle: string;
  strokeStyle: string | null;
  lineWidth: number;
  dash: readonly number[];
}

export interface AnnotationOverlayCirclePrimitive {
  kind: "circle";
  center: AnnotationOverlayPoint;
  radius: number;
  strokeStyle: string;
  lineWidth: number;
  fillStyle: string | null;
  dash: readonly number[];
}

export interface AnnotationOverlayMaskPrimitive {
  kind: "mask";
  runs: Array<{
    x: number;
    y: number;
    width: number;
    height: number;
  }>;
  fillStyle: string;
  lineWidth: number;
  dash: readonly number[];
}

export type AnnotationOverlayPrimitive =
  | AnnotationOverlayRectPrimitive
  | AnnotationOverlayPolylinePrimitive
  | AnnotationOverlaySegmentsPrimitive
  | AnnotationOverlayMarkersPrimitive
  | AnnotationOverlayCirclePrimitive
  | AnnotationOverlayMaskPrimitive;

export interface AnnotationOverlayGpuColor {
  red: number;
  green: number;
  blue: number;
  alpha: number;
}

export interface AnnotationOverlayGpuBounds {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface AnnotationOverlayGpuRectFillDraw {
  kind: "rect_fill";
  bounds: AnnotationOverlayGpuBounds;
  fillColor: AnnotationOverlayGpuColor;
}

export interface AnnotationOverlayGpuLineDraw {
  kind: "line";
  bounds: AnnotationOverlayGpuBounds;
  start: AnnotationOverlayPoint;
  end: AnnotationOverlayPoint;
  strokeColor: AnnotationOverlayGpuColor;
  lineWidth: number;
  dashOn: number;
  dashOff: number;
}

export interface AnnotationOverlayGpuCircleDraw {
  kind: "circle";
  bounds: AnnotationOverlayGpuBounds;
  center: AnnotationOverlayPoint;
  radius: number;
  fillColor: AnnotationOverlayGpuColor;
  strokeColor: AnnotationOverlayGpuColor;
  lineWidth: number;
}

export type AnnotationOverlayGpuDraw =
  | AnnotationOverlayGpuRectFillDraw
  | AnnotationOverlayGpuLineDraw
  | AnnotationOverlayGpuCircleDraw;

export type AnnotationWorkspaceRectDragKind =
  | "none"
  | "move"
  | "resize-top-left"
  | "resize-top-right"
  | "resize-bottom-left"
  | "resize-bottom-right";

export type AnnotationWorkspaceBoxDragKind =
  | Exclude<AnnotationWorkspaceRectDragKind, "none">
  | "create";

export interface AnnotationWorkspaceHandleReference {
  objectIndex: number;
  elementIndex: number;
  role: AnnotationWorkspaceHandleRole;
}

export interface AnnotationWorkspaceHandleTarget {
  kind: "editable_handle";
  handle: AnnotationWorkspaceEditableHandleState;
}

export interface AnnotationWorkspaceSplineSegmentTarget {
  kind: "spline_segment";
  object: AnnotationWorkspaceObjectState;
  segmentIndex: number;
}

export interface AnnotationWorkspaceObjectTarget {
  kind: "object";
  object: AnnotationWorkspaceObjectState;
}

export interface AnnotationWorkspaceBoxTarget {
  kind: "box";
  object: AnnotationWorkspaceObjectState;
  dragKind: Exclude<AnnotationWorkspaceRectDragKind, "none">;
}

export interface AnnotationWorkspaceCropTarget {
  kind: "crop";
  dragKind: AnnotationWorkspaceRectDragKind;
}

export type AnnotationWorkspaceInteractionTarget =
  | AnnotationWorkspaceHandleTarget
  | AnnotationWorkspaceSplineSegmentTarget
  | AnnotationWorkspaceObjectTarget
  | AnnotationWorkspaceBoxTarget
  | AnnotationWorkspaceCropTarget;

export interface AnnotationOverlayInteractionState {
  hoveredObjectIndex: number | null;
  hoveredHandle: AnnotationWorkspaceHandleReference | null;
  focusedHandle: AnnotationWorkspaceHandleReference | null;
  hoveredSplineSegment: {
    objectIndex: number;
    segmentIndex: number;
  } | null;
  activeSplineSegment: {
    objectIndex: number;
    segmentIndex: number;
  } | null;
}

export interface AnnotationWorkspaceCropDragState {
  kind: Exclude<AnnotationWorkspaceRectDragKind, "none">;
  startPoint: AnnotationOverlayPoint;
  startClip: CaptureRegion;
}

export interface AnnotationWorkspaceBoxDragPreview {
  objectIndex: number | null;
  captureBox: AnnotationWorkspaceBox;
  dragKind: AnnotationWorkspaceBoxDragKind;
}

const kSelectedStroke = "rgba(255, 220, 96, 0.98)";
const kSelectedFill = "rgba(255, 220, 96, 0.10)";
const kHoveredStroke = "rgba(124, 198, 255, 0.98)";
const kHoveredFill = "rgba(124, 198, 255, 0.10)";
const kHandleStroke = "rgba(28, 33, 38, 0.92)";
const kHandleFill = "rgba(255, 248, 236, 0.96)";
const kLatentHandleStroke = "rgba(196, 204, 216, 0.92)";
const kLatentHandleFill = "rgba(16, 20, 25, 0.28)";
const kHandleTetherStroke = "rgba(255, 248, 236, 0.56)";
const kLatentHandleTetherStroke = "rgba(196, 204, 216, 0.52)";
const kBrushPreviewPaintStroke = "rgba(128, 255, 170, 0.9)";
const kBrushPreviewEraseStroke = "rgba(255, 128, 128, 0.9)";
const kVisibleDenseMaskAlpha = 0.16;
const kSelectedDenseMaskAlpha = 0.34;

type OverlayColorStyle = {
  strokeStyle: string;
  fillStyle: string;
  lineWidth: number;
};

const kCropHandleFill = "rgba(255, 248, 236, 0.96)";
const kCropHandleStroke = "rgba(24, 28, 32, 0.92)";
const kTransparentOverlayColor: AnnotationOverlayGpuColor = {
  red: 0,
  green: 0,
  blue: 0,
  alpha: 0,
};

function clamp(value: number, minValue: number, maxValue: number): number {
  return Math.min(maxValue, Math.max(minValue, value));
}

function clampInteger(
  value: number,
  minValue: number,
  maxValue: number,
): number {
  return Math.min(maxValue, Math.max(minValue, Math.trunc(value)));
}

function rgba(red: number, green: number, blue: number, alpha: number): string {
  return `rgba(${red}, ${green}, ${blue}, ${alpha})`;
}

function rgbaColor(
  red: number,
  green: number,
  blue: number,
  alpha: number,
): AnnotationOverlayGpuColor {
  return {
    red: clamp(red / 255, 0, 1),
    green: clamp(green / 255, 0, 1),
    blue: clamp(blue / 255, 0, 1),
    alpha: clamp(alpha, 0, 1),
  };
}

function parseAnnotationOverlayColor(
  style: string | null,
): AnnotationOverlayGpuColor {
  if (style === null) {
    return kTransparentOverlayColor;
  }

  const value = style.trim().toLowerCase();
  if (value === "transparent") {
    return kTransparentOverlayColor;
  }

  const rgbaMatch = value.match(
    /^rgba?\(\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*(?:,\s*(-?\d+(?:\.\d+)?)\s*)?\)$/,
  );
  if (rgbaMatch !== null) {
    return rgbaColor(
      Number(rgbaMatch[1] ?? 0),
      Number(rgbaMatch[2] ?? 0),
      Number(rgbaMatch[3] ?? 0),
      Number(rgbaMatch[4] ?? 1),
    );
  }

  const hexMatch = value.match(/^#([\da-f]{6})$/i);
  if (hexMatch !== null) {
    return rgbaColor(
      Number.parseInt(hexMatch[1].slice(0, 2), 16),
      Number.parseInt(hexMatch[1].slice(2, 4), 16),
      Number.parseInt(hexMatch[1].slice(4, 6), 16),
      1,
    );
  }

  return kTransparentOverlayColor;
}

function dashPattern(
  dash: readonly number[],
): {
  dashOn: number;
  dashOff: number;
} {
  const dashOn = Math.max(0, dash[0] ?? 0);
  const dashOff = Math.max(0, dash[1] ?? dashOn);
  if (dashOn <= 0 || dashOff <= 0) {
    return {
      dashOn: 0,
      dashOff: 0,
    };
  }
  return {
    dashOn,
    dashOff,
  };
}

function expandedRectBounds(
  x: number,
  y: number,
  width: number,
  height: number,
): AnnotationOverlayGpuBounds | null {
  if (width <= 0 || height <= 0) {
    return null;
  }
  return { x, y, width, height };
}

function expandedLineBounds(
  start: AnnotationOverlayPoint,
  end: AnnotationOverlayPoint,
  lineWidth: number,
): AnnotationOverlayGpuBounds {
  const padding = Math.max(1.5, lineWidth * 0.75 + 1);
  const minX = Math.min(start.x, end.x) - padding;
  const minY = Math.min(start.y, end.y) - padding;
  const maxX = Math.max(start.x, end.x) + padding;
  const maxY = Math.max(start.y, end.y) + padding;
  return {
    x: minX,
    y: minY,
    width: Math.max(1, maxX - minX),
    height: Math.max(1, maxY - minY),
  };
}

function expandedCircleBounds(
  center: AnnotationOverlayPoint,
  radius: number,
  lineWidth: number,
): AnnotationOverlayGpuBounds {
  const extent = Math.max(1, radius + Math.max(0, lineWidth) * 0.5 + 1.5);
  return {
    x: center.x - extent,
    y: center.y - extent,
    width: extent * 2,
    height: extent * 2,
  };
}

function visibleCircleDrawColors(
  primitive: {
    fillStyle: string | null;
    strokeStyle: string | null;
    radius: number;
    lineWidth: number;
  },
): { fillColor: AnnotationOverlayGpuColor; strokeColor: AnnotationOverlayGpuColor } | null {
  const fillColor = parseAnnotationOverlayColor(primitive.fillStyle);
  const strokeColor = parseAnnotationOverlayColor(primitive.strokeStyle);
  if (
    primitive.radius <= 0 ||
    (fillColor.alpha <= 0 && (strokeColor.alpha <= 0 || primitive.lineWidth <= 0))
  ) {
    return null;
  }
  return { fillColor, strokeColor };
}

function pushLineDraw(
  draws: AnnotationOverlayGpuDraw[],
  start: AnnotationOverlayPoint,
  end: AnnotationOverlayPoint,
  strokeColor: AnnotationOverlayGpuColor,
  lineWidth: number,
  dash: readonly number[],
): void {
  if (strokeColor.alpha <= 0 || lineWidth <= 0) {
    return;
  }
  const { dashOn, dashOff } = dashPattern(dash);
  draws.push({
    kind: "line",
    bounds: expandedLineBounds(start, end, lineWidth),
    start,
    end,
    strokeColor,
    lineWidth,
    dashOn,
    dashOff,
  });
}

function categoryColor(categoryIndex: number): [number, number, number] {
  const palette: Array<[number, number, number]> = [
    [240, 196, 68],
    [88, 188, 255],
    [255, 128, 88],
    [96, 214, 146],
    [214, 112, 255],
    [255, 96, 152],
    [180, 214, 92],
    [255, 168, 64],
  ];
  return (
    palette[Math.abs(Math.trunc(categoryIndex)) % palette.length] ?? palette[0]
  );
}

function objectStyle(
  categoryIndex: number,
  selected: boolean,
  hovered: boolean,
): OverlayColorStyle {
  if (selected) {
    return {
      strokeStyle: kSelectedStroke,
      fillStyle: kSelectedFill,
      lineWidth: 3,
    };
  }
  if (hovered) {
    return {
      strokeStyle: kHoveredStroke,
      fillStyle: kHoveredFill,
      lineWidth: 2.5,
    };
  }
  const [red, green, blue] = categoryColor(categoryIndex);
  return {
    strokeStyle: rgba(red, green, blue, 0.92),
    fillStyle: rgba(red, green, blue, 0.08),
    lineWidth: 2,
  };
}

function denseMaskFillStyle(categoryIndex: number, alpha: number): string {
  const [red, green, blue] = categoryColor(categoryIndex);
  return rgba(red, green, blue, alpha);
}

function annotationCapturePointToCanvasPoint(
  geometry: WorkspaceGeometry,
  point: AnnotationWorkspacePoint,
): AnnotationOverlayPoint {
  return {
    x: geometry.frameX + point.x * geometry.scale,
    y: geometry.frameY + point.y * geometry.scale,
  };
}

function annotationCaptureBoxToCanvasRect(
  geometry: WorkspaceGeometry,
  box: AnnotationWorkspaceBox,
): {
  x: number;
  y: number;
  width: number;
  height: number;
} {
  const left = Math.min(box.x1, box.x2);
  const top = Math.min(box.y1, box.y2);
  const right = Math.max(box.x1, box.x2);
  const bottom = Math.max(box.y1, box.y2);
  return {
    x: geometry.frameX + left * geometry.scale,
    y: geometry.frameY + top * geometry.scale,
    width: Math.max(0, (right - left) * geometry.scale),
    height: Math.max(0, (bottom - top) * geometry.scale),
  };
}

function annotationCaptureRegionToCanvasRect(
  geometry: WorkspaceGeometry,
  region: CaptureRegion,
): {
  x: number;
  y: number;
  width: number;
  height: number;
} {
  return {
    x: geometry.frameX + region.x * geometry.scale,
    y: geometry.frameY + region.y * geometry.scale,
    width: region.width * geometry.scale,
    height: region.height * geometry.scale,
  };
}

function pointDistanceSquared(
  left: AnnotationOverlayPoint,
  right: AnnotationOverlayPoint,
): number {
  const dx = left.x - right.x;
  const dy = left.y - right.y;
  return dx * dx + dy * dy;
}

function pointInExpandedRect(
  point: AnnotationOverlayPoint,
  rect: { x: number; y: number; width: number; height: number },
  padding: number,
): boolean {
  return (
    point.x >= rect.x - padding &&
    point.x <= rect.x + rect.width + padding &&
    point.y >= rect.y - padding &&
    point.y <= rect.y + rect.height + padding
  );
}

function distanceToSegmentSquared(
  point: AnnotationOverlayPoint,
  start: AnnotationOverlayPoint,
  end: AnnotationOverlayPoint,
): number {
  const dx = end.x - start.x;
  const dy = end.y - start.y;
  const lengthSquared = dx * dx + dy * dy;
  if (lengthSquared <= 0.0001) {
    return pointDistanceSquared(point, start);
  }

  const t = clamp(
    ((point.x - start.x) * dx + (point.y - start.y) * dy) / lengthSquared,
    0,
    1,
  );
  const projection = {
    x: start.x + dx * t,
    y: start.y + dy * t,
  };
  return pointDistanceSquared(point, projection);
}

function canvasPolylinePoints(
  geometry: WorkspaceGeometry,
  points: ReadonlyArray<AnnotationWorkspacePoint>,
): AnnotationOverlayPoint[] {
  return points.map((point) =>
    annotationCapturePointToCanvasPoint(geometry, point),
  );
}

function boxCornerPoints(rect: {
  x: number;
  y: number;
  width: number;
  height: number;
}): AnnotationOverlayPoint[] {
  return [
    { x: rect.x, y: rect.y },
    { x: rect.x + rect.width, y: rect.y },
    { x: rect.x, y: rect.y + rect.height },
    { x: rect.x + rect.width, y: rect.y + rect.height },
  ];
}

function handleReferenceFromHandle(
  handle: AnnotationWorkspaceEditableHandleState,
): AnnotationWorkspaceHandleReference {
  return {
    objectIndex: handle.objectIndex,
    elementIndex: handle.elementIndex,
    role: handle.role,
  };
}

function handleReferenceMatches(
  handle: AnnotationWorkspaceEditableHandleState,
  reference: AnnotationWorkspaceHandleReference | null,
): boolean {
  return (
    reference !== null &&
    handle.objectIndex === reference.objectIndex &&
    handle.elementIndex === reference.elementIndex &&
    handle.role === reference.role
  );
}

function findEditableHandle(
  scene: AnnotationWorkspaceSceneState | null,
  reference: AnnotationWorkspaceHandleReference | null,
): AnnotationWorkspaceEditableHandleState | null {
  if (scene === null || reference === null) {
    return null;
  }
  for (const handle of scene.editableHandles) {
    if (handleReferenceMatches(handle, reference)) {
      return handle;
    }
  }
  return null;
}

function interactionStateFromValue(
  value: AnnotationOverlayInteractionState | number | null | undefined,
): AnnotationOverlayInteractionState {
  if (typeof value === "number") {
    return {
      hoveredObjectIndex: value,
      hoveredHandle: null,
      focusedHandle: null,
      hoveredSplineSegment: null,
      activeSplineSegment: null,
    };
  }
  return {
    hoveredObjectIndex: value?.hoveredObjectIndex ?? null,
    hoveredHandle: value?.hoveredHandle ?? null,
    focusedHandle: value?.focusedHandle ?? null,
    hoveredSplineSegment: value?.hoveredSplineSegment ?? null,
    activeSplineSegment: value?.activeSplineSegment ?? null,
  };
}

function canvasHandleHitRadius(
  handle: AnnotationWorkspaceEditableHandleState,
): number {
  return handle.materialized ? 10 : 9;
}

function splineKnotHandles(
  scene: AnnotationWorkspaceSceneState,
  objectIndex: number,
): AnnotationWorkspaceEditableHandleState[] {
  return scene.editableHandles
    .filter(
      (handle) =>
        handle.objectIndex === objectIndex && handle.role === "spline_knot",
    )
    .sort((left, right) => left.elementIndex - right.elementIndex);
}

function splineSegmentPoints(
  scene: AnnotationWorkspaceSceneState,
  geometry: WorkspaceGeometry,
  object: AnnotationWorkspaceObjectState,
): Array<{
  segmentIndex: number;
  start: AnnotationOverlayPoint;
  end: AnnotationOverlayPoint;
}> {
  if (
    scene.selectedObjectIndex !== object.index ||
    object.shapeType !== "spline"
  ) {
    return [];
  }

  const handles = splineKnotHandles(scene, object.index);
  if (handles.length < 2) {
    return [];
  }

  const segments: Array<{
    segmentIndex: number;
    start: AnnotationOverlayPoint;
    end: AnnotationOverlayPoint;
  }> = [];
  for (let index = 0; index + 1 < handles.length; index += 1) {
    segments.push({
      segmentIndex: index,
      start: annotationCapturePointToCanvasPoint(
        geometry,
        handles[index].capturePoint,
      ),
      end: annotationCapturePointToCanvasPoint(
        geometry,
        handles[index + 1].capturePoint,
      ),
    });
  }
  if (object.geometry.closed && handles.length > 2) {
    segments.push({
      segmentIndex: handles.length - 1,
      start: annotationCapturePointToCanvasPoint(
        geometry,
        handles[handles.length - 1].capturePoint,
      ),
      end: annotationCapturePointToCanvasPoint(
        geometry,
        handles[0].capturePoint,
      ),
    });
  }
  return segments;
}

function geometryHit(
  object: AnnotationWorkspaceObjectState,
  geometry: WorkspaceGeometry,
  point: AnnotationOverlayPoint,
  hitRadiusPx: number,
): boolean {
  const objectPoints = canvasPolylinePoints(
    geometry,
    object.geometry.capturePoints,
  );
  const hitRadiusSquared = hitRadiusPx * hitRadiusPx;

  for (const objectPoint of objectPoints) {
    if (pointDistanceSquared(point, objectPoint) <= hitRadiusSquared) {
      return true;
    }
  }

  if (object.geometry.edges.length > 0) {
    for (const edge of object.geometry.edges) {
      const start = objectPoints[edge.sourceIndex] ?? null;
      const end = objectPoints[edge.targetIndex] ?? null;
      if (start === null || end === null) {
        continue;
      }
      if (distanceToSegmentSquared(point, start, end) <= hitRadiusSquared) {
        return true;
      }
    }
    return false;
  }

  for (let index = 1; index < objectPoints.length; index += 1) {
    if (
      distanceToSegmentSquared(
        point,
        objectPoints[index - 1],
        objectPoints[index],
      ) <= hitRadiusSquared
    ) {
      return true;
    }
  }

  if (object.geometry.closed && objectPoints.length > 2) {
    if (
      distanceToSegmentSquared(
        point,
        objectPoints[objectPoints.length - 1],
        objectPoints[0],
      ) <= hitRadiusSquared
    ) {
      return true;
    }
  }

  return false;
}

export function resolveAnnotationWorkspaceHit(
  scene: AnnotationWorkspaceSceneState | null,
  geometry: WorkspaceGeometry,
  point: AnnotationOverlayPoint,
): AnnotationWorkspaceObjectState | null {
  if (scene === null) {
    return null;
  }

  const hitRadiusPx = clamp(geometry.scale * 4, 8, 20);
  const boxPaddingPx = Math.max(6, hitRadiusPx * 0.75);
  for (let index = scene.visibleObjects.length - 1; index >= 0; index -= 1) {
    const object = scene.visibleObjects[index] ?? null;
    if (object === null) {
      continue;
    }

    if (geometryHit(object, geometry, point, hitRadiusPx)) {
      return object;
    }

    const rect = annotationCaptureBoxToCanvasRect(geometry, object.captureBox);
    if (pointInExpandedRect(point, rect, boxPaddingPx)) {
      return object;
    }
  }

  return null;
}

export function resolveAnnotationEditableHandleHit(
  scene: AnnotationWorkspaceSceneState | null,
  geometry: WorkspaceGeometry,
  point: AnnotationOverlayPoint,
): AnnotationWorkspaceHandleTarget | null {
  if (scene === null) {
    return null;
  }

  let bestHandle: AnnotationWorkspaceEditableHandleState | null = null;
  let bestDistanceSquared = Number.POSITIVE_INFINITY;
  for (const handle of scene.editableHandles) {
    const handlePoint = annotationCapturePointToCanvasPoint(
      geometry,
      handle.capturePoint,
    );
    const radius = canvasHandleHitRadius(handle);
    const distanceSquared = pointDistanceSquared(point, handlePoint);
    if (
      distanceSquared <= radius * radius &&
      (bestHandle === null ||
        distanceSquared < bestDistanceSquared ||
        (Math.abs(distanceSquared - bestDistanceSquared) < 0.001 &&
          handle.materialized &&
          !bestHandle.materialized))
    ) {
      bestHandle = handle;
      bestDistanceSquared = distanceSquared;
    }
  }

  return bestHandle === null
    ? null
    : {
        kind: "editable_handle",
        handle: bestHandle,
      };
}

export function resolveAnnotationSplineSegmentHit(
  scene: AnnotationWorkspaceSceneState | null,
  geometry: WorkspaceGeometry,
  point: AnnotationOverlayPoint,
): AnnotationWorkspaceSplineSegmentTarget | null {
  if (scene === null || scene.selectedObjectIndex === null) {
    return null;
  }

  const hitRadiusSquared = clamp(geometry.scale * 4, 8, 20) ** 2;
  for (let index = scene.visibleObjects.length - 1; index >= 0; index -= 1) {
    const object = scene.visibleObjects[index] ?? null;
    if (
      object === null ||
      object.index !== scene.selectedObjectIndex ||
      object.shapeType !== "spline"
    ) {
      continue;
    }

    let bestSegmentIndex: number | null = null;
    let bestDistanceSquared = Number.POSITIVE_INFINITY;
    for (const segment of splineSegmentPoints(scene, geometry, object)) {
      const distanceSquared = distanceToSegmentSquared(
        point,
        segment.start,
        segment.end,
      );
      if (
        distanceSquared <= hitRadiusSquared &&
        distanceSquared < bestDistanceSquared
      ) {
        bestSegmentIndex = segment.segmentIndex;
        bestDistanceSquared = distanceSquared;
      }
    }

    if (bestSegmentIndex !== null) {
      return {
        kind: "spline_segment",
        object,
        segmentIndex: bestSegmentIndex,
      };
    }
  }

  return null;
}

function resolveAnnotationRectDragKind(
  point: AnnotationOverlayPoint,
  rect: { x: number; y: number; width: number; height: number },
  scale: number,
): AnnotationWorkspaceRectDragKind {
  if (rect.width <= 0 || rect.height <= 0) {
    return "none";
  }

  const hitPad = clamp(scale * 4, 6, 18);
  const cornerSize = clamp(hitPad * 3, 18, 42);
  const nearLeft = point.x >= rect.x - hitPad && point.x <= rect.x + hitPad;
  const nearRight =
    point.x >= rect.x + rect.width - hitPad &&
    point.x <= rect.x + rect.width + hitPad;
  const nearTop = point.y >= rect.y - hitPad && point.y <= rect.y + hitPad;
  const nearBottom =
    point.y >= rect.y + rect.height - hitPad &&
    point.y <= rect.y + rect.height + hitPad;
  const inYRange =
    point.y >= rect.y - hitPad && point.y <= rect.y + rect.height + hitPad;
  const inXRange =
    point.x >= rect.x - hitPad && point.x <= rect.x + rect.width + hitPad;
  const insideX = point.x >= rect.x && point.x <= rect.x + rect.width;
  const insideY = point.y >= rect.y && point.y <= rect.y + rect.height;

  if (
    nearTop &&
    nearLeft &&
    point.x < rect.x + cornerSize &&
    point.y < rect.y + cornerSize
  ) {
    return "resize-top-left";
  }
  if (
    nearTop &&
    nearRight &&
    point.x > rect.x + rect.width - cornerSize &&
    point.y < rect.y + cornerSize
  ) {
    return "resize-top-right";
  }
  if (
    nearBottom &&
    nearLeft &&
    point.x < rect.x + cornerSize &&
    point.y > rect.y + rect.height - cornerSize
  ) {
    return "resize-bottom-left";
  }
  if (
    nearBottom &&
    nearRight &&
    point.x > rect.x + rect.width - cornerSize &&
    point.y > rect.y + rect.height - cornerSize
  ) {
    return "resize-bottom-right";
  }
  if (
    (nearLeft && inYRange) ||
    (nearRight && inYRange) ||
    (nearTop && inXRange) ||
    (nearBottom && inXRange) ||
    (insideX && insideY)
  ) {
    return "move";
  }
  return "none";
}

export function resolveAnnotationCropDragKind(
  geometry: WorkspaceGeometry,
  clip: CaptureRegion | null,
  point: AnnotationOverlayPoint,
): AnnotationWorkspaceRectDragKind {
  if (clip === null || clip.width <= 0 || clip.height <= 0) {
    return "none";
  }

  return resolveAnnotationRectDragKind(
    point,
    annotationCaptureRegionToCanvasRect(geometry, clip),
    geometry.scale,
  );
}

export function resolveAnnotationWorkspaceBoxDragKind(
  geometry: WorkspaceGeometry,
  box: AnnotationWorkspaceBox,
  point: AnnotationOverlayPoint,
): Exclude<AnnotationWorkspaceRectDragKind, "none"> | "none" {
  return resolveAnnotationRectDragKind(
    point,
    annotationCaptureBoxToCanvasRect(geometry, box),
    geometry.scale,
  );
}

export function resolveAnnotationWorkspaceBoxHit(
  scene: AnnotationWorkspaceSceneState | null,
  geometry: WorkspaceGeometry,
  point: AnnotationOverlayPoint,
): AnnotationWorkspaceBoxTarget | null {
  if (scene === null) {
    return null;
  }

  for (let index = scene.visibleObjects.length - 1; index >= 0; index -= 1) {
    const object = scene.visibleObjects[index] ?? null;
    if (
      object === null ||
      (object.shapeType !== "box" && object.shapeType !== "mask")
    ) {
      continue;
    }

    const dragKind = resolveAnnotationWorkspaceBoxDragKind(
      geometry,
      object.captureBox,
      point,
    );
    if (dragKind === "none") {
      continue;
    }

    return {
      kind: "box",
      object,
      dragKind,
    };
  }

  return null;
}

export function applyAnnotationWorkspaceBoxDrag(
  drag: {
    kind: AnnotationWorkspaceBoxDragKind;
    startCapturePoint: AnnotationWorkspacePoint;
    startBox: AnnotationWorkspaceBox;
  },
  point: AnnotationWorkspacePoint,
  captureWidth: number,
  captureHeight: number,
  minExtent = 1,
): AnnotationWorkspaceBox {
  const minimumExtent = Math.max(1, Math.trunc(minExtent));
  const maxWidth = Math.max(1, Math.trunc(captureWidth));
  const maxHeight = Math.max(1, Math.trunc(captureHeight));
  const currentX = clampInteger(Math.round(point.x), 0, maxWidth - 1);
  const currentY = clampInteger(Math.round(point.y), 0, maxHeight - 1);
  const startX = clampInteger(
    Math.round(drag.startCapturePoint.x),
    0,
    maxWidth - 1,
  );
  const startY = clampInteger(
    Math.round(drag.startCapturePoint.y),
    0,
    maxHeight - 1,
  );
  const startBox = {
    x1: Math.min(drag.startBox.x1, drag.startBox.x2),
    y1: Math.min(drag.startBox.y1, drag.startBox.y2),
    x2: Math.max(drag.startBox.x1, drag.startBox.x2),
    y2: Math.max(drag.startBox.y1, drag.startBox.y2),
  };

  if (drag.kind === "create") {
    return {
      x1: Math.min(startX, currentX),
      y1: Math.min(startY, currentY),
      x2: Math.min(
        maxWidth,
        Math.max(startX + minimumExtent, currentX + minimumExtent),
      ),
      y2: Math.min(
        maxHeight,
        Math.max(startY + minimumExtent, currentY + minimumExtent),
      ),
    };
  }

  if (drag.kind === "move") {
    const width = Math.max(minimumExtent, startBox.x2 - startBox.x1);
    const height = Math.max(minimumExtent, startBox.y2 - startBox.y1);
    const nextX = clampInteger(
      startBox.x1 + (currentX - startX),
      0,
      Math.max(0, maxWidth - width),
    );
    const nextY = clampInteger(
      startBox.y1 + (currentY - startY),
      0,
      Math.max(0, maxHeight - height),
    );
    return {
      x1: nextX,
      y1: nextY,
      x2: nextX + width,
      y2: nextY + height,
    };
  }

  let nextX1 = startBox.x1;
  let nextY1 = startBox.y1;
  let nextX2 = startBox.x2;
  let nextY2 = startBox.y2;

  if (drag.kind === "resize-top-left" || drag.kind === "resize-bottom-left") {
    nextX1 = clampInteger(currentX, 0, startBox.x2 - minimumExtent);
  }
  if (drag.kind === "resize-top-left" || drag.kind === "resize-top-right") {
    nextY1 = clampInteger(currentY, 0, startBox.y2 - minimumExtent);
  }
  if (drag.kind === "resize-top-right" || drag.kind === "resize-bottom-right") {
    nextX2 = clampInteger(
      currentX + minimumExtent,
      startBox.x1 + minimumExtent,
      maxWidth,
    );
  }
  if (
    drag.kind === "resize-bottom-left" ||
    drag.kind === "resize-bottom-right"
  ) {
    nextY2 = clampInteger(
      currentY + minimumExtent,
      startBox.y1 + minimumExtent,
      maxHeight,
    );
  }

  return {
    x1: nextX1,
    y1: nextY1,
    x2: nextX2,
    y2: nextY2,
  };
}

export function resolveAnnotationWorkspaceInteractionTarget(
  scene: AnnotationWorkspaceSceneState | null,
  geometry: WorkspaceGeometry,
  point: AnnotationOverlayPoint,
  clip: CaptureRegion | null = null,
): AnnotationWorkspaceInteractionTarget | null {
  const editableHandleHit = resolveAnnotationEditableHandleHit(
    scene,
    geometry,
    point,
  );
  if (editableHandleHit !== null) {
    return editableHandleHit;
  }

  const splineSegmentHit = resolveAnnotationSplineSegmentHit(
    scene,
    geometry,
    point,
  );
  if (splineSegmentHit !== null) {
    return splineSegmentHit;
  }

  const boxHit = resolveAnnotationWorkspaceBoxHit(scene, geometry, point);
  if (boxHit !== null) {
    return boxHit;
  }

  const objectHit = resolveAnnotationWorkspaceHit(scene, geometry, point);
  if (objectHit !== null) {
    return {
      kind: "object",
      object: objectHit,
    };
  }

  const cropDragKind = resolveAnnotationCropDragKind(geometry, clip, point);
  if (cropDragKind !== "none") {
    return {
      kind: "crop",
      dragKind: cropDragKind,
    };
  }

  return null;
}

export function annotationWorkspaceCursorForDragKind(
  dragKind: AnnotationWorkspaceRectDragKind,
  active = false,
): string {
  if (dragKind === "move") {
    return active ? "grabbing" : "grab";
  }
  if (dragKind === "resize-top-left" || dragKind === "resize-bottom-right") {
    return "nwse-resize";
  }
  if (dragKind === "resize-top-right" || dragKind === "resize-bottom-left") {
    return "nesw-resize";
  }
  return "default";
}

export function applyAnnotationWorkspaceCropDrag(
  drag: AnnotationWorkspaceCropDragState,
  geometry: WorkspaceGeometry,
  point: AnnotationOverlayPoint,
  captureWidth: number,
  captureHeight: number,
  minExtent = 1,
): CaptureRegion {
  const scale = Math.max(geometry.scale, 0.0001);
  const dx = Math.trunc((point.x - drag.startPoint.x) / scale);
  const dy = Math.trunc((point.y - drag.startPoint.y) / scale);
  const minimumExtent = Math.max(1, Math.trunc(minExtent));
  const maxWidth = Math.max(1, Math.trunc(captureWidth));
  const maxHeight = Math.max(1, Math.trunc(captureHeight));

  let nextX = drag.startClip.x;
  let nextY = drag.startClip.y;
  let nextWidth = drag.startClip.width;
  let nextHeight = drag.startClip.height;

  if (drag.kind === "move") {
    nextX = clampInteger(
      drag.startClip.x + dx,
      0,
      Math.max(0, maxWidth - drag.startClip.width),
    );
    nextY = clampInteger(
      drag.startClip.y + dy,
      0,
      Math.max(0, maxHeight - drag.startClip.height),
    );
  } else {
    const startRight = drag.startClip.x + drag.startClip.width;
    const startBottom = drag.startClip.y + drag.startClip.height;
    let nextRight = startRight;
    let nextBottom = startBottom;

    if (drag.kind === "resize-top-left" || drag.kind === "resize-bottom-left") {
      nextX = clampInteger(
        drag.startClip.x + dx,
        0,
        startRight - minimumExtent,
      );
    }
    if (drag.kind === "resize-top-left" || drag.kind === "resize-top-right") {
      nextY = clampInteger(
        drag.startClip.y + dy,
        0,
        startBottom - minimumExtent,
      );
    }
    if (
      drag.kind === "resize-top-right" ||
      drag.kind === "resize-bottom-right"
    ) {
      nextRight = clampInteger(
        startRight + dx,
        drag.startClip.x + minimumExtent,
        maxWidth,
      );
    }
    if (
      drag.kind === "resize-bottom-left" ||
      drag.kind === "resize-bottom-right"
    ) {
      nextBottom = clampInteger(
        startBottom + dy,
        drag.startClip.y + minimumExtent,
        maxHeight,
      );
    }

    nextWidth = Math.max(minimumExtent, nextRight - nextX);
    nextHeight = Math.max(minimumExtent, nextBottom - nextY);
  }

  return {
    x: clampInteger(nextX, 0, Math.max(0, maxWidth - minimumExtent)),
    y: clampInteger(nextY, 0, Math.max(0, maxHeight - minimumExtent)),
    width: clampInteger(nextWidth, minimumExtent, maxWidth),
    height: clampInteger(nextHeight, minimumExtent, maxHeight),
  };
}

function handleTetherSegments(
  geometry: WorkspaceGeometry,
  handles: ReadonlyArray<AnnotationWorkspaceEditableHandleState>,
): Array<{
  start: AnnotationOverlayPoint;
  end: AnnotationOverlayPoint;
}> {
  const segments: Array<{
    start: AnnotationOverlayPoint;
    end: AnnotationOverlayPoint;
  }> = [];
  for (const handle of handles) {
    if (handle.tetherCapturePoint === null) {
      continue;
    }
    segments.push({
      start: annotationCapturePointToCanvasPoint(
        geometry,
        handle.tetherCapturePoint,
      ),
      end: annotationCapturePointToCanvasPoint(geometry, handle.capturePoint),
    });
  }
  return segments;
}

function brushPreviewPrimitive(
  geometry: WorkspaceGeometry,
  brushPreview: AnnotationWorkspaceBrushPreviewState | null,
): AnnotationOverlayCirclePrimitive | null {
  if (brushPreview === null || !brushPreview.visible) {
    return null;
  }
  return {
    kind: "circle",
    center: annotationCapturePointToCanvasPoint(
      geometry,
      brushPreview.capturePoint,
    ),
    radius: Math.max(2, brushPreview.radius * geometry.scale),
    strokeStyle: brushPreview.erase
      ? kBrushPreviewEraseStroke
      : kBrushPreviewPaintStroke,
    lineWidth: 1.5,
    fillStyle: null,
    dash: [],
  };
}

function denseMaskPrimitive(
  geometry: WorkspaceGeometry,
  denseMask: AnnotationWorkspaceDenseMaskState | null,
  alpha: number,
): AnnotationOverlayMaskPrimitive | null {
  if (denseMask === null) {
    return null;
  }
  const maskWidth = Math.max(
    0,
    Math.trunc(denseMask.captureBox.x2 - denseMask.captureBox.x1),
  );
  const maskHeight = Math.max(
    0,
    Math.trunc(denseMask.captureBox.y2 - denseMask.captureBox.y1),
  );
  if (maskWidth <= 0 || maskHeight <= 0) {
    return null;
  }

  const totalPixels = maskWidth * maskHeight;
  const runs: AnnotationOverlayMaskPrimitive["runs"] = [];
  for (const token of denseMask.maskRle.trim().split(/\s+/)) {
    if (token.length === 0) {
      continue;
    }
    const separator = token.indexOf(":");
    if (separator <= 0 || separator + 1 >= token.length) {
      continue;
    }
    const start = Math.max(0, Math.trunc(Number(token.slice(0, separator))));
    const length = Math.max(0, Math.trunc(Number(token.slice(separator + 1))));
    if (!Number.isFinite(start) || !Number.isFinite(length) || length <= 0) {
      continue;
    }
    let cursor = Math.min(start, totalPixels);
    const boundedEnd = Math.min(totalPixels, cursor + length);
    while (cursor < boundedEnd) {
      const row = Math.floor(cursor / maskWidth);
      const column = cursor - row * maskWidth;
      const rowLength = Math.min(boundedEnd - cursor, maskWidth - column);
      const x =
        geometry.frameX + (denseMask.captureBox.x1 + column) * geometry.scale;
      const y = geometry.frameY + (denseMask.captureBox.y1 + row) * geometry.scale;
      runs.push({
        x,
        y,
        width: rowLength * geometry.scale,
        height: geometry.scale,
      });
      cursor += rowLength;
    }
  }

  if (runs.length === 0) {
    return null;
  }
  return {
    kind: "mask",
    runs,
    fillStyle: denseMaskFillStyle(denseMask.categoryIndex, alpha),
    lineWidth: 0,
    dash: [],
  };
}

export function buildAnnotationOverlayPrimitives(
  scene: AnnotationWorkspaceSceneState | null,
  geometry: WorkspaceGeometry,
  interactionInput: AnnotationOverlayInteractionState | number | null = null,
  boxPreview: AnnotationWorkspaceBoxDragPreview | null = null,
): AnnotationOverlayPrimitive[] {
  if (scene === null) {
    return [];
  }

  const interaction = interactionStateFromValue(interactionInput);
  const primitives: AnnotationOverlayPrimitive[] = [];
  const selectedDenseMaskObjectIndex = scene.selectedDenseMask?.objectIndex ?? null;
  for (const denseMask of scene.visibleDenseMasks) {
    if (
      selectedDenseMaskObjectIndex !== null &&
      denseMask.objectIndex === selectedDenseMaskObjectIndex
    ) {
      continue;
    }
    const maskPrimitive = denseMaskPrimitive(
      geometry,
      denseMask,
      kVisibleDenseMaskAlpha,
    );
    if (maskPrimitive !== null) {
      primitives.push(maskPrimitive);
    }
  }
  if (
    scene.selectedDenseMask !== null &&
    scene.selectedDenseMask.objectIndex === scene.selectedObjectIndex
  ) {
    const maskPrimitive = denseMaskPrimitive(
      geometry,
      scene.selectedDenseMask,
      kSelectedDenseMaskAlpha,
    );
    if (maskPrimitive !== null) {
      primitives.push(maskPrimitive);
    }
  }
  for (const object of scene.visibleObjects) {
    const selected = scene.selectedObjectIndex === object.index;
    const hovered = interaction.hoveredObjectIndex === object.index;
    const style = objectStyle(object.categoryIndex, selected, hovered);
    const previewCaptureBox =
      boxPreview !== null &&
      boxPreview.objectIndex === object.index &&
      (object.shapeType === "box" || object.shapeType === "mask")
        ? boxPreview.captureBox
        : object.captureBox;
    const previewDash =
      boxPreview !== null &&
      boxPreview.objectIndex === object.index &&
      (object.shapeType === "box" || object.shapeType === "mask")
        ? [10, 6]
        : [];

    if (
      object.shapeType === "box" ||
      object.shapeType === "mask" ||
      selected ||
      hovered
    ) {
      const rect = annotationCaptureBoxToCanvasRect(
        geometry,
        previewCaptureBox,
      );
      primitives.push({
        kind: "rect",
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height,
        strokeStyle: style.strokeStyle,
        lineWidth: style.lineWidth,
        fillStyle: selected || hovered ? style.fillStyle : null,
        dash: previewDash,
      });
      if (selected || hovered) {
        primitives.push({
          kind: "markers",
          points: boxCornerPoints(rect),
          radius: selected ? 4.5 : 4,
          fillStyle: selected ? kCropHandleFill : style.strokeStyle,
          strokeStyle: selected ? kCropHandleStroke : "rgba(20, 24, 28, 0.9)",
          lineWidth: selected ? 1.6 : 1.2,
          dash: previewDash,
        });
      }
    }

    const points = canvasPolylinePoints(
      geometry,
      object.geometry.capturePoints,
    );
    if (object.geometry.edges.length > 0) {
      const segments = object.geometry.edges
        .map((edge) => {
          const start = points[edge.sourceIndex] ?? null;
          const end = points[edge.targetIndex] ?? null;
          if (start === null || end === null) {
            return null;
          }
          return { start, end };
        })
        .filter(
          (
            segment,
          ): segment is {
            start: AnnotationOverlayPoint;
            end: AnnotationOverlayPoint;
          } => segment !== null,
        );
      if (segments.length > 0) {
        primitives.push({
          kind: "segments",
          segments,
          strokeStyle: style.strokeStyle,
          lineWidth: style.lineWidth,
          dash: [],
        });
      }
    } else if (points.length > 1) {
      primitives.push({
        kind: "polyline",
        points,
        closed: object.geometry.closed,
        strokeStyle: style.strokeStyle,
        lineWidth: style.lineWidth,
        fillStyle: null,
        dash: [],
      });
    }

    if (
      points.length > 0 &&
      (object.shapeType === "point" ||
        object.shapeType === "skeleton" ||
        object.geometry.edges.length > 0)
    ) {
      primitives.push({
        kind: "markers",
        points,
        radius: selected ? 5 : hovered ? 4.5 : 4,
        fillStyle: style.strokeStyle,
        strokeStyle: "rgba(20, 24, 28, 0.9)",
        lineWidth: 1.25,
        dash: [],
      });
    }
  }

  const activeSplineSegment =
    interaction.activeSplineSegment !== null
      ? (scene.visibleObjects.find(
          (object) =>
            object.index === interaction.activeSplineSegment?.objectIndex &&
            object.shapeType === "spline",
        ) ?? null)
      : null;
  if (
    activeSplineSegment !== null &&
    interaction.activeSplineSegment !== null
  ) {
    const segment = splineSegmentPoints(
      scene,
      geometry,
      activeSplineSegment,
    ).find(
      (entry) =>
        entry.segmentIndex === interaction.activeSplineSegment?.segmentIndex,
    );
    if (segment !== undefined) {
      primitives.push({
        kind: "segments",
        segments: [{ start: segment.start, end: segment.end }],
        strokeStyle: kSelectedStroke,
        lineWidth: 3,
        dash: [],
      });
    }
  }

  const hoveredSplineSegment =
    interaction.hoveredSplineSegment !== null &&
    (interaction.activeSplineSegment === null ||
      interaction.hoveredSplineSegment.objectIndex !==
        interaction.activeSplineSegment.objectIndex ||
      interaction.hoveredSplineSegment.segmentIndex !==
        interaction.activeSplineSegment.segmentIndex)
      ? (scene.visibleObjects.find(
          (object) =>
            object.index === interaction.hoveredSplineSegment?.objectIndex &&
            object.shapeType === "spline",
        ) ?? null)
      : null;
  if (
    hoveredSplineSegment !== null &&
    interaction.hoveredSplineSegment !== null
  ) {
    const segment = splineSegmentPoints(
      scene,
      geometry,
      hoveredSplineSegment,
    ).find(
      (entry) =>
        entry.segmentIndex === interaction.hoveredSplineSegment?.segmentIndex,
    );
    if (segment !== undefined) {
      primitives.push({
        kind: "segments",
        segments: [{ start: segment.start, end: segment.end }],
        strokeStyle: kHoveredStroke,
        lineWidth: 2.5,
        dash: [8, 4],
      });
    }
  }

  const materializedHandles = scene.editableHandles.filter(
    (handle) => handle.materialized,
  );
  const latentHandles = scene.editableHandles.filter(
    (handle) => !handle.materialized,
  );
  const materializedSegments = handleTetherSegments(
    geometry,
    materializedHandles,
  );
  const latentSegments = handleTetherSegments(geometry, latentHandles);
  if (materializedSegments.length > 0) {
    primitives.push({
      kind: "segments",
      segments: materializedSegments,
      strokeStyle: kHandleTetherStroke,
      lineWidth: 1.5,
      dash: [],
    });
  }
  if (latentSegments.length > 0) {
    primitives.push({
      kind: "segments",
      segments: latentSegments,
      strokeStyle: kLatentHandleTetherStroke,
      lineWidth: 1.25,
      dash: [7, 5],
    });
  }

  if (materializedHandles.length > 0) {
    primitives.push({
      kind: "markers",
      points: materializedHandles.map((handle) =>
        annotationCapturePointToCanvasPoint(geometry, handle.capturePoint),
      ),
      radius: 4.5,
      fillStyle: kHandleFill,
      strokeStyle: kHandleStroke,
      lineWidth: 1.5,
      dash: [],
    });
  }
  if (latentHandles.length > 0) {
    primitives.push({
      kind: "markers",
      points: latentHandles.map((handle) =>
        annotationCapturePointToCanvasPoint(geometry, handle.capturePoint),
      ),
      radius: 4,
      fillStyle: kLatentHandleFill,
      strokeStyle: kLatentHandleStroke,
      lineWidth: 1.25,
      dash: [],
    });
  }

  const focusedHandle = findEditableHandle(scene, interaction.focusedHandle);
  if (focusedHandle !== null) {
    primitives.push({
      kind: "markers",
      points: [
        annotationCapturePointToCanvasPoint(
          geometry,
          focusedHandle.capturePoint,
        ),
      ],
      radius: 6,
      fillStyle: kSelectedStroke,
      strokeStyle: kHandleStroke,
      lineWidth: 2,
      dash: [],
    });
  }

  const hoveredHandle = findEditableHandle(scene, interaction.hoveredHandle);
  if (
    hoveredHandle !== null &&
    (focusedHandle === null ||
      !handleReferenceMatches(
        hoveredHandle,
        handleReferenceFromHandle(focusedHandle),
      ))
  ) {
    primitives.push({
      kind: "markers",
      points: [
        annotationCapturePointToCanvasPoint(
          geometry,
          hoveredHandle.capturePoint,
        ),
      ],
      radius: 5.25,
      fillStyle: kHoveredStroke,
      strokeStyle: kHandleStroke,
      lineWidth: 2,
      dash: [],
    });
  }

  if (boxPreview !== null && boxPreview.objectIndex === null) {
    const rect = annotationCaptureBoxToCanvasRect(
      geometry,
      boxPreview.captureBox,
    );
    primitives.push({
      kind: "rect",
      x: rect.x,
      y: rect.y,
      width: rect.width,
      height: rect.height,
      strokeStyle: kSelectedStroke,
      lineWidth: 3,
      fillStyle: kSelectedFill,
      dash: [10, 6],
    });
    primitives.push({
      kind: "markers",
      points: boxCornerPoints(rect),
      radius: 4.5,
      fillStyle: kCropHandleFill,
      strokeStyle: kCropHandleStroke,
      lineWidth: 1.6,
      dash: [10, 6],
    });
  }

  const brushPreview = brushPreviewPrimitive(geometry, scene.brushPreview);
  if (brushPreview !== null) {
    primitives.push(brushPreview);
  }

  return primitives;
}

export function buildAnnotationOverlayGpuDraws(
  primitives: ReadonlyArray<AnnotationOverlayPrimitive>,
): AnnotationOverlayGpuDraw[] {
  const draws: AnnotationOverlayGpuDraw[] = [];

  for (const primitive of primitives) {
    if (primitive.kind === "mask") {
      const fillColor = parseAnnotationOverlayColor(primitive.fillStyle);
      if (fillColor.alpha <= 0) {
        continue;
      }
      for (const run of primitive.runs) {
        const bounds = expandedRectBounds(run.x, run.y, run.width, run.height);
        if (bounds === null) {
          continue;
        }
        draws.push({
          kind: "rect_fill",
          bounds,
          fillColor,
        });
      }
      continue;
    }

    if (primitive.kind === "rect") {
      const fillColor = parseAnnotationOverlayColor(primitive.fillStyle);
      const strokeColor = parseAnnotationOverlayColor(primitive.strokeStyle);
      const bounds = expandedRectBounds(
        primitive.x,
        primitive.y,
        primitive.width,
        primitive.height,
      );
      if (bounds === null) {
        continue;
      }
      if (fillColor.alpha > 0) {
        draws.push({
          kind: "rect_fill",
          bounds,
          fillColor,
        });
      }
      pushLineDraw(
        draws,
        { x: primitive.x, y: primitive.y },
        { x: primitive.x + primitive.width, y: primitive.y },
        strokeColor,
        primitive.lineWidth,
        primitive.dash,
      );
      pushLineDraw(
        draws,
        { x: primitive.x + primitive.width, y: primitive.y },
        { x: primitive.x + primitive.width, y: primitive.y + primitive.height },
        strokeColor,
        primitive.lineWidth,
        primitive.dash,
      );
      pushLineDraw(
        draws,
        { x: primitive.x + primitive.width, y: primitive.y + primitive.height },
        { x: primitive.x, y: primitive.y + primitive.height },
        strokeColor,
        primitive.lineWidth,
        primitive.dash,
      );
      pushLineDraw(
        draws,
        { x: primitive.x, y: primitive.y + primitive.height },
        { x: primitive.x, y: primitive.y },
        strokeColor,
        primitive.lineWidth,
        primitive.dash,
      );
      continue;
    }

    if (primitive.kind === "polyline") {
      const strokeColor = parseAnnotationOverlayColor(primitive.strokeStyle);
      for (let index = 1; index < primitive.points.length; index += 1) {
        pushLineDraw(
          draws,
          primitive.points[index - 1],
          primitive.points[index],
          strokeColor,
          primitive.lineWidth,
          primitive.dash,
        );
      }
      if (primitive.closed && primitive.points.length > 2) {
        pushLineDraw(
          draws,
          primitive.points[primitive.points.length - 1],
          primitive.points[0],
          strokeColor,
          primitive.lineWidth,
          primitive.dash,
        );
      }
      continue;
    }

    if (primitive.kind === "segments") {
      const strokeColor = parseAnnotationOverlayColor(primitive.strokeStyle);
      for (const segment of primitive.segments) {
        pushLineDraw(
          draws,
          segment.start,
          segment.end,
          strokeColor,
          primitive.lineWidth,
          primitive.dash,
        );
      }
      continue;
    }

    if (primitive.kind === "circle") {
      const colors = visibleCircleDrawColors(primitive);
      if (colors === null) {
        continue;
      }
      draws.push({
        kind: "circle",
        bounds: expandedCircleBounds(
          primitive.center,
          primitive.radius,
          primitive.lineWidth,
        ),
        center: primitive.center,
        radius: primitive.radius,
        fillColor: colors.fillColor,
        strokeColor: colors.strokeColor,
        lineWidth: primitive.lineWidth,
      });
      continue;
    }

    const colors = visibleCircleDrawColors(primitive);
    if (colors === null) {
      continue;
    }
    for (const point of primitive.points) {
      draws.push({
        kind: "circle",
        bounds: expandedCircleBounds(point, primitive.radius, primitive.lineWidth),
        center: point,
        radius: primitive.radius,
        fillColor: colors.fillColor,
        strokeColor: colors.strokeColor,
        lineWidth: primitive.lineWidth,
      });
    }
  }

  return draws;
}
