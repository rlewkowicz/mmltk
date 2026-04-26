import * as assert from "node:assert/strict";
import { test } from "node:test";

import type { WorkspaceRenderInput } from "../src/workspace_renderer";
import {
  buildWorkspaceCanvasLayout,
  buildWorkspaceRenderFrame,
  workspaceClipRectForInput,
  workspaceCanvasLayoutsEqual,
  workspaceOverlayBounds,
} from "../src/workspace_renderer";
import { makeTestOverlayRectPrimitive } from "./support/workspace-render-fixtures";

function makeInput(): WorkspaceRenderInput {
  return {
    workflow: "annotate",
    canvasWidth: 1280,
    canvasHeight: 720,
    captureWidth: 1920,
    captureHeight: 1080,
    frameX: 100,
    frameY: 40,
    frameWidth: 960,
    frameHeight: 540,
    clip: {
      x: 44,
      y: 48,
      width: 512,
      height: 144,
    },
    annotationCount: 2,
    overlayPrimitives: [],
  };
}

test("workspaceClipRectForInput projects crop coordinates into workspace frame space", () => {
  const clipRect = workspaceClipRectForInput(makeInput());

  assert.deepEqual(clipRect, {
    x: 122,
    y: 64,
    width: 256,
    height: 72,
  });
});

test("workspaceOverlayBounds unions annotation primitives in device space", () => {
  const bounds = workspaceOverlayBounds([
    makeTestOverlayRectPrimitive(),
    {
      kind: "circle",
      center: { x: 220, y: 180 },
      radius: 16,
      strokeStyle: "rgba(124, 198, 255, 1)",
      lineWidth: 4,
      fillStyle: null,
      dash: [],
    },
  ]);

  assert.deepEqual(bounds, {
    x: 31,
    y: 23,
    width: 207,
    height: 175,
  });
});

test("buildWorkspaceCanvasLayout scales viewport and overlay bounds into CSS space", () => {
  const input = makeInput();
  input.overlayPrimitives = [makeTestOverlayRectPrimitive()];

  const report = buildWorkspaceCanvasLayout(input, {
    devicePixelRatio: 2,
    surfaceBoundsCss: {
      x: 10,
      y: 20,
      width: 640,
      height: 360,
    },
  });

  assert.deepEqual(report.viewportBoundsDevice, {
    x: 100,
    y: 40,
    width: 960,
    height: 540,
  });
  assert.deepEqual(report.viewportBoundsCss, {
    x: 60,
    y: 40,
    width: 480,
    height: 270,
  });
  assert.deepEqual(report.clipBoundsCss, {
    x: 71,
    y: 52,
    width: 128,
    height: 36,
  });
  assert.deepEqual(report.overlayBoundsCss, {
    x: 25.5,
    y: 31.5,
    width: 65,
    height: 49,
  });
  assert.equal(report.scaleDevicePx, 0.5);
  assert.equal(report.scaleCssPx, 0.25);
});

test("buildWorkspaceRenderFrame packages the render input with canvas layout geometry", () => {
  const input = makeInput();
  const frame = buildWorkspaceRenderFrame(input, {
    devicePixelRatio: 2,
    surfaceBoundsCss: {
      x: 10,
      y: 20,
      width: 640,
      height: 360,
    },
  });

  assert.equal(frame.input, input);
  assert.deepEqual(frame.canvasLayout.viewportBoundsDevice, {
    x: 100,
    y: 40,
    width: 960,
    height: 540,
  });
  assert.deepEqual(frame.canvasLayout.viewportBoundsCss, {
    x: 60,
    y: 40,
    width: 480,
    height: 270,
  });
});

test("workspaceCanvasLayoutsEqual compares full layout payloads", () => {
  const left = buildWorkspaceCanvasLayout(makeInput(), {
    devicePixelRatio: 2,
    surfaceBoundsCss: {
      x: 10,
      y: 20,
      width: 640,
      height: 360,
    },
  });
  const right = buildWorkspaceCanvasLayout(makeInput(), {
    devicePixelRatio: 2,
    surfaceBoundsCss: {
      x: 10,
      y: 20,
      width: 640,
      height: 360,
    },
  });
  const changed = buildWorkspaceCanvasLayout(
    {
      ...makeInput(),
      frameX: 120,
    },
    {
      devicePixelRatio: 2,
      surfaceBoundsCss: {
        x: 10,
        y: 20,
        width: 640,
        height: 360,
      },
    },
  );

  assert.equal(workspaceCanvasLayoutsEqual(left, right), true);
  assert.equal(workspaceCanvasLayoutsEqual(left, changed), false);
});
