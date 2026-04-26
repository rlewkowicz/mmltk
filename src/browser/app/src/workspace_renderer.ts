import type { CaptureRegion, Workflow } from "./host_api";
import { captureRegionEquals, rectLikeEquals } from "./app_shared";
import type { AnnotationOverlayPrimitive } from "./app/workspace/annotation-scene-overlay";

export interface WorkspaceRenderInput {
  workflow: Workflow;
  canvasWidth: number;
  canvasHeight: number;
  captureWidth: number;
  captureHeight: number;
  frameX: number;
  frameY: number;
  frameWidth: number;
  frameHeight: number;
  clip: CaptureRegion | null;
  annotationCount: number;
  overlayPrimitives: ReadonlyArray<AnnotationOverlayPrimitive>;
}

export interface WorkspaceSurfaceRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface WorkspaceSurfaceLayoutOptions {
  devicePixelRatio: number;
  surfaceBoundsCss: WorkspaceSurfaceRect;
}

export interface WorkspaceCanvasLayout {
  workflow: Workflow;
  devicePixelRatio: number;
  surfaceBoundsCss: WorkspaceSurfaceRect;
  canvasWidth: number;
  canvasHeight: number;
  captureWidth: number;
  captureHeight: number;
  viewportBoundsDevice: CaptureRegion;
  viewportBoundsCss: WorkspaceSurfaceRect;
  clipBoundsDevice: CaptureRegion | null;
  clipBoundsCss: WorkspaceSurfaceRect | null;
  overlayBoundsDevice: CaptureRegion | null;
  overlayBoundsCss: WorkspaceSurfaceRect | null;
  annotationCount: number;
  overlayPrimitiveCount: number;
  scaleDevicePx: number;
  scaleCssPx: number;
}

export interface WorkspaceRenderFrame {
  input: WorkspaceRenderInput;
  canvasLayout: WorkspaceCanvasLayout;
}

function clampDimension(value: number): number {
  if (!Number.isFinite(value)) {
    return 1;
  }
  return Math.max(1, Math.round(value));
}

function maxFinite(value: number, fallback: number): number {
  return Number.isFinite(value) ? value : fallback;
}

function unionBounds(
  current: CaptureRegion | null,
  next: CaptureRegion | null,
): CaptureRegion | null {
  if (next === null) {
    return current;
  }
  if (current === null) {
    return next;
  }
  const left = Math.min(current.x, next.x);
  const top = Math.min(current.y, next.y);
  const right = Math.max(current.x + current.width, next.x + next.width);
  const bottom = Math.max(current.y + current.height, next.y + next.height);
  return {
    x: left,
    y: top,
    width: Math.max(0, right - left),
    height: Math.max(0, bottom - top),
  };
}

function expandBounds(
  bounds: CaptureRegion | null,
  amount: number,
): CaptureRegion | null {
  if (bounds === null) {
    return null;
  }
  const inset = Math.max(0, amount);
  return {
    x: bounds.x - inset,
    y: bounds.y - inset,
    width: bounds.width + inset * 2,
    height: bounds.height + inset * 2,
  };
}

function pointsBounds(
  points: ReadonlyArray<{
    x: number;
    y: number;
  }>,
): CaptureRegion | null {
  if (points.length === 0) {
    return null;
  }
  let left = points[0].x;
  let top = points[0].y;
  let right = points[0].x;
  let bottom = points[0].y;
  for (let index = 1; index < points.length; index += 1) {
    const point = points[index];
    if (point === undefined) {
      continue;
    }
    left = Math.min(left, point.x);
    top = Math.min(top, point.y);
    right = Math.max(right, point.x);
    bottom = Math.max(bottom, point.y);
  }
  return {
    x: left,
    y: top,
    width: Math.max(0, right - left),
    height: Math.max(0, bottom - top),
  };
}

function primitiveBounds(
  primitive: AnnotationOverlayPrimitive,
): CaptureRegion | null {
  switch (primitive.kind) {
    case "rect":
      return expandBounds(
        {
          x: primitive.x,
          y: primitive.y,
          width: primitive.width,
          height: primitive.height,
        },
        primitive.lineWidth * 0.5,
      );
    case "polyline":
      return expandBounds(pointsBounds(primitive.points), primitive.lineWidth * 0.5);
    case "segments": {
      let bounds: CaptureRegion | null = null;
      for (const segment of primitive.segments) {
        bounds = unionBounds(
          bounds,
          pointsBounds([segment.start, segment.end]),
        );
      }
      return expandBounds(bounds, primitive.lineWidth * 0.5);
    }
    case "markers":
      return expandBounds(pointsBounds(primitive.points), primitive.radius);
    case "circle":
      return expandBounds(
        {
          x: primitive.center.x - primitive.radius,
          y: primitive.center.y - primitive.radius,
          width: primitive.radius * 2,
          height: primitive.radius * 2,
        },
        primitive.lineWidth * 0.5,
      );
    case "mask": {
      let bounds: CaptureRegion | null = null;
      for (const run of primitive.runs) {
        bounds = unionBounds(bounds, {
          x: run.x,
          y: run.y,
          width: run.width,
          height: run.height,
        });
      }
      return bounds;
    }
  }
}

function scaleRegionToCss(
  region: CaptureRegion,
  canvasWidth: number,
  canvasHeight: number,
  surfaceBoundsCss: WorkspaceSurfaceRect,
): WorkspaceSurfaceRect {
  const widthScale =
    canvasWidth > 0 ? surfaceBoundsCss.width / canvasWidth : 0;
  const heightScale =
    canvasHeight > 0 ? surfaceBoundsCss.height / canvasHeight : 0;
  return {
    x: surfaceBoundsCss.x + region.x * widthScale,
    y: surfaceBoundsCss.y + region.y * heightScale,
    width: region.width * widthScale,
    height: region.height * heightScale,
  };
}

export function workspaceClipRectForInput(
  input: WorkspaceRenderInput,
): CaptureRegion | null {
  if (
    input.clip === null ||
    input.captureWidth <= 0 ||
    input.captureHeight <= 0 ||
    input.frameWidth <= 0 ||
    input.frameHeight <= 0
  ) {
    return null;
  }
  return {
    x: input.frameX + (input.clip.x / input.captureWidth) * input.frameWidth,
    y: input.frameY + (input.clip.y / input.captureHeight) * input.frameHeight,
    width: (input.clip.width / input.captureWidth) * input.frameWidth,
    height: (input.clip.height / input.captureHeight) * input.frameHeight,
  };
}

export function workspaceOverlayBounds(
  primitives: ReadonlyArray<AnnotationOverlayPrimitive>,
): CaptureRegion | null {
  let bounds: CaptureRegion | null = null;
  for (const primitive of primitives) {
    bounds = unionBounds(bounds, primitiveBounds(primitive));
  }
  return bounds;
}

export function buildWorkspaceCanvasLayout(
  input: WorkspaceRenderInput,
  options: WorkspaceSurfaceLayoutOptions,
): WorkspaceCanvasLayout {
  const canvasWidth = clampDimension(input.canvasWidth);
  const canvasHeight = clampDimension(input.canvasHeight);
  const surfaceBoundsCss = {
    x: maxFinite(options.surfaceBoundsCss.x, 0),
    y: maxFinite(options.surfaceBoundsCss.y, 0),
    width: maxFinite(options.surfaceBoundsCss.width, 0),
    height: maxFinite(options.surfaceBoundsCss.height, 0),
  };
  const viewportBoundsDevice: CaptureRegion = {
    x: maxFinite(input.frameX, 0),
    y: maxFinite(input.frameY, 0),
    width: Math.max(0, maxFinite(input.frameWidth, 0)),
    height: Math.max(0, maxFinite(input.frameHeight, 0)),
  };
  const clipBoundsDevice = workspaceClipRectForInput(input);
  const overlayBoundsDevice =
    workspaceOverlayBounds(input.overlayPrimitives) ?? viewportBoundsDevice;
  const viewportBoundsCss = scaleRegionToCss(
    viewportBoundsDevice,
    canvasWidth,
    canvasHeight,
    surfaceBoundsCss,
  );
  return {
    workflow: input.workflow,
    devicePixelRatio: Math.max(0.0001, maxFinite(options.devicePixelRatio, 1)),
    surfaceBoundsCss,
    canvasWidth,
    canvasHeight,
    captureWidth: clampDimension(input.captureWidth),
    captureHeight: clampDimension(input.captureHeight),
    viewportBoundsDevice,
    viewportBoundsCss,
    clipBoundsDevice,
    clipBoundsCss:
      clipBoundsDevice === null
        ? null
        : scaleRegionToCss(
            clipBoundsDevice,
            canvasWidth,
            canvasHeight,
            surfaceBoundsCss,
          ),
    overlayBoundsDevice,
    overlayBoundsCss:
      overlayBoundsDevice === null
        ? null
        : scaleRegionToCss(
            overlayBoundsDevice,
            canvasWidth,
            canvasHeight,
            surfaceBoundsCss,
          ),
    annotationCount: Math.max(0, Math.round(input.annotationCount)),
    overlayPrimitiveCount: input.overlayPrimitives.length,
    scaleDevicePx:
      input.captureWidth > 0 ? viewportBoundsDevice.width / input.captureWidth : 0,
    scaleCssPx:
      input.captureWidth > 0 ? viewportBoundsCss.width / input.captureWidth : 0,
  };
}

export function buildWorkspaceRenderFrame(
  input: WorkspaceRenderInput,
  options: WorkspaceSurfaceLayoutOptions,
): WorkspaceRenderFrame {
  return {
    input,
    canvasLayout: buildWorkspaceCanvasLayout(input, options),
  };
}

export function workspaceCanvasLayoutsEqual(
  left: WorkspaceCanvasLayout | null,
  right: WorkspaceCanvasLayout | null,
): boolean {
  if (left === right) {
    return true;
  }
  if (left === null || right === null) {
    return false;
  }
  return (
    left.workflow === right.workflow &&
    left.devicePixelRatio === right.devicePixelRatio &&
    rectLikeEquals(left.surfaceBoundsCss, right.surfaceBoundsCss) &&
    left.canvasWidth === right.canvasWidth &&
    left.canvasHeight === right.canvasHeight &&
    left.captureWidth === right.captureWidth &&
    left.captureHeight === right.captureHeight &&
    captureRegionEquals(left.viewportBoundsDevice, right.viewportBoundsDevice) &&
    rectLikeEquals(left.viewportBoundsCss, right.viewportBoundsCss) &&
    captureRegionEquals(left.clipBoundsDevice, right.clipBoundsDevice) &&
    rectLikeEquals(left.clipBoundsCss, right.clipBoundsCss) &&
    captureRegionEquals(left.overlayBoundsDevice, right.overlayBoundsDevice) &&
    rectLikeEquals(left.overlayBoundsCss, right.overlayBoundsCss) &&
    left.annotationCount === right.annotationCount &&
    left.overlayPrimitiveCount === right.overlayPrimitiveCount &&
    left.scaleDevicePx === right.scaleDevicePx &&
    left.scaleCssPx === right.scaleCssPx
  );
}
