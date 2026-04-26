import { test } from "node:test";
import * as assert from "node:assert/strict";

import type {
  AnnotationWorkspaceEditableHandleState,
  AnnotationWorkspaceObjectState,
  AnnotationWorkspaceSceneState,
} from "../src/browser_shell_state";
import type { CaptureRegion, WorkspaceGeometry } from "../src/host_api";
import {
  applyAnnotationWorkspaceCropDrag,
  buildAnnotationOverlayGpuDraws,
  buildAnnotationOverlayPrimitives,
  resolveAnnotationWorkspaceInteractionTarget,
  resolveAnnotationWorkspaceHit,
} from "../src/app/workspace/annotation-scene-overlay";
import { makeTestOverlayRectPrimitive } from "./support/workspace-render-fixtures";

function makeGeometry(): WorkspaceGeometry {
  return {
    canvasWidth: 1280,
    canvasHeight: 720,
    fitScale: 1,
    scale: 1,
    frameX: 0,
    frameY: 0,
    frameWidth: 1280,
    frameHeight: 720,
  };
}

function makeObject(
  index: number,
  shapeType: AnnotationWorkspaceObjectState["shapeType"],
  captureBox: AnnotationWorkspaceObjectState["captureBox"],
  geometry: AnnotationWorkspaceObjectState["geometry"],
): AnnotationWorkspaceObjectState {
  return {
    index,
    categoryIndex: index,
    shapeType,
    captureBox,
    fullyVisible: true,
    geometry,
  };
}

function makeHandle(
  role: AnnotationWorkspaceEditableHandleState["role"],
  capturePoint: AnnotationWorkspaceEditableHandleState["capturePoint"],
  tetherCapturePoint: AnnotationWorkspaceEditableHandleState["tetherCapturePoint"],
): AnnotationWorkspaceEditableHandleState {
  return {
    objectIndex: 1,
    elementIndex: 0,
    role,
    categoryIndex: 1,
    capturePoint,
    tetherCapturePoint,
    materialized: true,
  };
}

function makeSplineObject(): AnnotationWorkspaceObjectState {
  return makeObject(
    1,
    "spline",
    { x1: 320, y1: 120, x2: 760, y2: 420 },
    {
      capturePoints: [
        { x: 340, y: 180 },
        { x: 460, y: 300 },
        { x: 620, y: 210 },
        { x: 740, y: 340 },
      ],
      edges: [],
      closed: false,
    },
  );
}

function makeSplineHandles(includeTrailingKnot = true): AnnotationWorkspaceEditableHandleState[] {
  return [
    makeHandle("spline_knot", { x: 460, y: 300 }, null),
    ...(includeTrailingKnot
      ? [makeHandle("spline_knot", { x: 620, y: 210 }, null)]
      : []),
    makeHandle("spline_out_handle", { x: 520, y: 270 }, { x: 460, y: 300 }),
  ];
}

function makeSplineScene(
  documentGeneration: number,
  overrides: Partial<
    Pick<
      AnnotationWorkspaceSceneState,
      "selectedObjectIndex" | "visibleObjects" | "editableHandles"
    >
  > = {},
): AnnotationWorkspaceSceneState {
  return {
    documentGeneration,
    selectedObjectIndex: 1,
    captureWidth: 1280,
    captureHeight: 720,
    visibleObjects: [makeSplineObject()],
    editableHandles: makeSplineHandles(),
    brushPreview: null,
    visibleDenseMasks: [],
    selectedDenseMask: null,
    ...overrides,
  };
}

test("resolveAnnotationWorkspaceHit prefers top-most geometry hits", () => {
  const scene: AnnotationWorkspaceSceneState = {
    documentGeneration: 7,
    selectedObjectIndex: 1,
    captureWidth: 1280,
    captureHeight: 720,
    visibleObjects: [
      makeObject(
        0,
        "box",
        { x1: 120, y1: 120, x2: 320, y2: 320 },
        {
          capturePoints: [],
          edges: [],
          closed: false,
        },
      ),
      makeObject(
        1,
        "spline",
        { x1: 180, y1: 160, x2: 420, y2: 320 },
        {
          capturePoints: [
            { x: 200, y: 220 },
            { x: 260, y: 250 },
            { x: 340, y: 210 },
          ],
          edges: [],
          closed: false,
        },
      ),
    ],
    editableHandles: [],
    brushPreview: null,
    visibleDenseMasks: [],
    selectedDenseMask: null,
  };

  const hit = resolveAnnotationWorkspaceHit(scene, makeGeometry(), {
    x: 260,
    y: 250,
  });

  assert.ok(hit !== null);
  assert.equal(hit.index, 1);
  assert.equal(hit.shapeType, "spline");
});

test("buildAnnotationOverlayPrimitives emits object overlays and editable handle primitives", () => {
  const scene = makeSplineScene(11, {
    visibleObjects: [
      makeObject(
        0,
        "box",
        { x1: 64, y1: 72, x2: 280, y2: 236 },
        {
          capturePoints: [],
          edges: [],
          closed: false,
        },
      ),
      makeSplineObject(),
    ],
    editableHandles: makeSplineHandles(false),
  });

  const primitives = buildAnnotationOverlayPrimitives(scene, makeGeometry(), 0);
  const rects = primitives.filter((primitive) => primitive.kind === "rect");
  const polylines = primitives.filter((primitive) => primitive.kind === "polyline");
  const markerSets = primitives.filter((primitive) => primitive.kind === "markers");
  const segmentSets = primitives.filter((primitive) => primitive.kind === "segments");

  assert.equal(rects.length, 2);
  assert.equal(polylines.length, 1);
  assert.ok(
    markerSets.some(
      (primitive) =>
        primitive.kind === "markers" && primitive.points.some((point) => point.x === 460),
    ),
  );
  assert.ok(
    segmentSets.some(
      (primitive) =>
        primitive.kind === "segments" &&
        primitive.segments.some(
          (segment) => segment.start.x === 460 && segment.end.x === 520,
      ),
    ),
  );
});

test("buildAnnotationOverlayPrimitives emits visible and selected dense masks ahead of vector overlays", () => {
  const scene: AnnotationWorkspaceSceneState = {
    documentGeneration: 12,
    selectedObjectIndex: 1,
    captureWidth: 1280,
    captureHeight: 720,
    visibleObjects: [
      makeObject(
        0,
        "mask",
        { x1: 220, y1: 150, x2: 224, y2: 152 },
        {
          capturePoints: [],
          edges: [],
          closed: false,
        },
      ),
      makeObject(
        1,
        "mask",
        { x1: 320, y1: 120, x2: 760, y2: 420 },
        {
          capturePoints: [],
          edges: [],
          closed: false,
        },
      ),
    ],
    editableHandles: [],
    brushPreview: null,
    visibleDenseMasks: [
      {
        objectIndex: 0,
        categoryIndex: 4,
        captureBox: { x1: 220, y1: 150, x2: 224, y2: 152 },
        maskRle: "0:2 4:3",
      },
      {
        objectIndex: 1,
        categoryIndex: 2,
        captureBox: { x1: 400, y1: 220, x2: 404, y2: 223 },
        maskRle: "0:2 5:1 8:4",
      },
    ],
    selectedDenseMask: {
      objectIndex: 1,
      categoryIndex: 2,
      captureBox: { x1: 400, y1: 220, x2: 404, y2: 223 },
      maskRle: "0:2 5:1 8:4",
    },
  };

  const primitives = buildAnnotationOverlayPrimitives(scene, makeGeometry(), 1);
  const maskPrimitives = primitives.filter(
    (primitive): primitive is Extract<(typeof primitives)[number], { kind: "mask" }> =>
      primitive.kind === "mask",
  );

  assert.equal(primitives[0]?.kind, "mask");
  assert.equal(primitives[1]?.kind, "mask");
  assert.equal(maskPrimitives.length, 2);
  assert.equal(maskPrimitives[0]?.fillStyle, "rgba(214, 112, 255, 0.16)");
  assert.deepEqual(maskPrimitives[0]?.runs, [
    { x: 220, y: 150, width: 2, height: 1 },
    { x: 220, y: 151, width: 3, height: 1 },
  ]);
  assert.equal(maskPrimitives[1]?.fillStyle, "rgba(255, 128, 88, 0.34)");
  assert.deepEqual(maskPrimitives[1]?.runs, [
    { x: 400, y: 220, width: 2, height: 1 },
    { x: 401, y: 221, width: 1, height: 1 },
    { x: 400, y: 222, width: 4, height: 1 },
  ]);
});

test("buildAnnotationOverlayPrimitives emits a scaled brush preview circle when present", () => {
  const geometry: WorkspaceGeometry = {
    ...makeGeometry(),
    scale: 2,
    frameWidth: 2560,
    frameHeight: 1440,
  };
  const scene: AnnotationWorkspaceSceneState = {
    documentGeneration: 12,
    selectedObjectIndex: null,
    captureWidth: 1280,
    captureHeight: 720,
    visibleObjects: [],
    editableHandles: [],
    brushPreview: {
      visible: true,
      capturePoint: { x: 320, y: 180 },
      radius: 24,
      erase: true,
    },
    visibleDenseMasks: [],
    selectedDenseMask: null,
  };

  const primitives = buildAnnotationOverlayPrimitives(scene, geometry, null);
  const circle = primitives.find((primitive) => primitive.kind === "circle");

  assert.ok(circle !== undefined);
  assert.equal(circle?.kind, "circle");
  if (circle?.kind === "circle") {
    assert.deepEqual(circle.center, { x: 640, y: 360 });
    assert.equal(circle.radius, 48);
    assert.equal(circle.strokeStyle, "rgba(255, 128, 128, 0.9)");
  }
});

test("buildAnnotationOverlayGpuDraws flattens canvas primitives into bounded GPU draws", () => {
  const draws = buildAnnotationOverlayGpuDraws([
    makeTestOverlayRectPrimitive({
      fillStyle: "rgba(255, 200, 32, 0.1)",
      dash: [10, 6],
    }),
    {
      kind: "segments",
      segments: [
        {
          start: { x: 200, y: 120 },
          end: { x: 260, y: 180 },
        },
      ],
      strokeStyle: "rgba(124, 198, 255, 0.98)",
      lineWidth: 2.5,
      dash: [8, 4],
    },
    {
      kind: "markers",
      points: [
        { x: 460, y: 300 },
        { x: 520, y: 270 },
      ],
      radius: 4.5,
      fillStyle: "rgba(255, 248, 236, 0.96)",
      strokeStyle: "rgba(28, 33, 38, 0.92)",
      lineWidth: 1.5,
      dash: [],
    },
    {
      kind: "mask",
      runs: [
        { x: 400, y: 220, width: 2, height: 1 },
        { x: 401, y: 221, width: 1, height: 1 },
      ],
      fillStyle: "rgba(255, 128, 88, 0.34)",
      lineWidth: 0,
      dash: [],
    },
  ]);

  const rectFills = draws.filter((draw) => draw.kind === "rect_fill");
  const lines = draws.filter((draw) => draw.kind === "line");
  const circles = draws.filter((draw) => draw.kind === "circle");

  assert.equal(rectFills.length, 3);
  assert.equal(lines.length, 5);
  assert.equal(circles.length, 2);
  assert.deepEqual(rectFills[0]?.bounds, {
    x: 32,
    y: 24,
    width: 128,
    height: 96,
  });
  if (rectFills[0]?.kind === "rect_fill") {
    assert.equal(rectFills[0].fillColor.red, 1);
    assert.equal(rectFills[0].fillColor.green, 200 / 255);
    assert.equal(rectFills[0].fillColor.blue, 32 / 255);
    assert.equal(rectFills[0].fillColor.alpha, 0.1);
  }
  if (lines[0]?.kind === "line") {
    assert.equal(lines[0].dashOn, 10);
    assert.equal(lines[0].dashOff, 6);
    assert.ok(lines[0].bounds.width >= 128);
  }
  if (circles[0]?.kind === "circle") {
    assert.deepEqual(circles[0].center, { x: 460, y: 300 });
    assert.equal(circles[0].radius, 4.5);
    assert.equal(circles[0].strokeColor.alpha, 0.92);
  }
});

test("resolveAnnotationWorkspaceInteractionTarget prioritizes editable handles and spline segments", () => {
  const scene = makeSplineScene(12);

  const handleTarget = resolveAnnotationWorkspaceInteractionTarget(
    scene,
    makeGeometry(),
    { x: 520, y: 270 },
    null,
  );
  assert.ok(handleTarget !== null);
  assert.equal(handleTarget?.kind, "editable_handle");

  const segmentTarget = resolveAnnotationWorkspaceInteractionTarget(
    scene,
    makeGeometry(),
    { x: 540, y: 255 },
    null,
  );
  assert.ok(segmentTarget !== null);
  assert.equal(segmentTarget?.kind, "spline_segment");
  if (segmentTarget?.kind === "spline_segment") {
    assert.equal(segmentTarget.segmentIndex, 0);
    assert.equal(segmentTarget.object.index, 1);
  }
});

test("resolveAnnotationWorkspaceInteractionTarget falls back to crop handles", () => {
  const clip: CaptureRegion = {
    x: 128,
    y: 96,
    width: 512,
    height: 320,
  };

  const target = resolveAnnotationWorkspaceInteractionTarget(
    null,
    makeGeometry(),
    { x: 128, y: 96 },
    clip,
  );
  assert.ok(target !== null);
  assert.equal(target?.kind, "crop");
  if (target?.kind === "crop") {
    assert.equal(target.dragKind, "resize-top-left");
  }
});

test("applyAnnotationWorkspaceCropDrag moves and resizes clip regions in capture space", () => {
  const moved = applyAnnotationWorkspaceCropDrag(
    {
      kind: "move",
      startPoint: { x: 200, y: 160 },
      startClip: { x: 120, y: 80, width: 240, height: 180 },
    },
    makeGeometry(),
    { x: 260, y: 220 },
    1280,
    720,
  );
  assert.deepEqual(moved, {
    x: 180,
    y: 140,
    width: 240,
    height: 180,
  });

  const resized = applyAnnotationWorkspaceCropDrag(
    {
      kind: "resize-bottom-right",
      startPoint: { x: 360, y: 260 },
      startClip: { x: 120, y: 80, width: 240, height: 180 },
    },
    makeGeometry(),
    { x: 420, y: 320 },
    1280,
    720,
  );
  assert.deepEqual(resized, {
    x: 120,
    y: 80,
    width: 300,
    height: 240,
  });
});

test("buildAnnotationOverlayPrimitives adds active and hovered selection affordances", () => {
  const scene = makeSplineScene(13);

  const primitives = buildAnnotationOverlayPrimitives(scene, makeGeometry(), {
    hoveredObjectIndex: 1,
    hoveredHandle: {
      objectIndex: 1,
      elementIndex: 0,
      role: "spline_out_handle",
    },
    focusedHandle: {
      objectIndex: 1,
      elementIndex: 0,
      role: "spline_knot",
    },
    hoveredSplineSegment: {
      objectIndex: 1,
      segmentIndex: 0,
    },
    activeSplineSegment: {
      objectIndex: 1,
      segmentIndex: 0,
    },
  });

  const markerSets = primitives.filter((primitive) => primitive.kind === "markers");
  const segmentSets = primitives.filter((primitive) => primitive.kind === "segments");

  assert.ok(
    markerSets.some(
      (primitive) =>
        primitive.kind === "markers" &&
        primitive.radius >= 6 &&
        primitive.points.some((point) => point.x === 460 && point.y === 300),
    ),
  );
  assert.ok(
    markerSets.some(
      (primitive) =>
        primitive.kind === "markers" &&
        primitive.radius >= 5 &&
        primitive.points.some((point) => point.x === 520 && point.y === 270),
    ),
  );
  assert.ok(
    segmentSets.some(
      (primitive) =>
        primitive.kind === "segments" &&
        primitive.lineWidth >= 3 &&
        primitive.segments.some(
          (segment) => segment.start.x === 460 && segment.end.x === 620,
        ),
    ),
  );
});
