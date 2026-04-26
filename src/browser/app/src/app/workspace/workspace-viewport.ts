import { annotateSidebarState, captureRegionEquals } from "../../app_shared";
import type {
  CaptureRegion,
  StateSnapshot,
  Workflow,
  WorkspaceGeometry,
} from "../../host_api";

const kMinWorkspaceZoom = 0.5;
const kMaxWorkspaceZoom = 14;

export type WorkspaceInteractionMode = "idle" | "pan" | "clip";

export interface WorkspaceCanvasPoint {
  x: number;
  y: number;
}

export interface BrowserWorkspaceState {
  initialized: boolean;
  workflow: Workflow;
  canvasWidth: number;
  canvasHeight: number;
  captureWidth: number;
  captureHeight: number;
  centerX: number;
  centerY: number;
  zoom: number;
  clip: CaptureRegion | null;
  clipDirty: boolean;
  clipDraft: CaptureRegion | null;
  pointerId: number | null;
  mode: WorkspaceInteractionMode;
  lastPointerX: number;
  lastPointerY: number;
  clipAnchor: WorkspaceCanvasPoint | null;
}

export interface WorkspaceInteractionStart {
  pointerId: number;
  point: WorkspaceCanvasPoint;
  button: number;
  shiftKey: boolean;
}

export interface WorkspaceInteractionResult {
  workspace: BrowserWorkspaceState;
  commitViewport: boolean;
}

export function supportsViewportIntent(workflow: Workflow): boolean {
  return workflow === "predict" || workflow === "annotate" || workflow === "live";
}

export function clamp(value: number, minValue: number, maxValue: number): number {
  return Math.min(maxValue, Math.max(minValue, value));
}

function cloneRegion(region: CaptureRegion): CaptureRegion {
  return { ...region };
}

function idleWorkspaceInteractionState(): Pick<
  BrowserWorkspaceState,
  | "clipDirty"
  | "clipDraft"
  | "pointerId"
  | "mode"
  | "lastPointerX"
  | "lastPointerY"
  | "clipAnchor"
> {
  return {
    clipDirty: false,
    clipDraft: null,
    pointerId: null,
    mode: "idle",
    lastPointerX: 0,
    lastPointerY: 0,
    clipAnchor: null,
  };
}

export function roundRegion(region: CaptureRegion): CaptureRegion {
  const x = Math.max(0, Math.round(region.x));
  const y = Math.max(0, Math.round(region.y));
  const right = Math.max(x + 1, Math.round(region.x + region.width));
  const bottom = Math.max(y + 1, Math.round(region.y + region.height));
  return {
    x,
    y,
    width: right - x,
    height: bottom - y,
  };
}

export function formatRegion(region: CaptureRegion | null): string {
  if (region === null) {
    return "none";
  }
  return `${Math.round(region.x)}, ${Math.round(region.y)}, ${Math.round(region.width)} x ${Math.round(region.height)}`;
}

export function captureDimensions(snapshot: StateSnapshot): {
  width: number;
  height: number;
} {
  return {
    width: Math.max(1, snapshot.source.capture_width, snapshot.annotation.capture_width),
    height: Math.max(1, snapshot.source.capture_height, snapshot.annotation.capture_height),
  };
}

export function snapshotClip(snapshot: StateSnapshot): CaptureRegion | null {
  return snapshot.source.has_crop ? cloneRegion(snapshot.source.crop) : null;
}

function regionFromDisplayBox(
  box: { x1: number; y1: number; x2: number; y2: number } | null | undefined,
): CaptureRegion | null {
  if (box === null || box === undefined) {
    return null;
  }

  const x = Math.min(box.x1, box.x2);
  const y = Math.min(box.y1, box.y2);
  const width = Math.abs(box.x2 - box.x1);
  const height = Math.abs(box.y2 - box.y1);
  if (width <= 0 || height <= 0) {
    return null;
  }
  return roundRegion({ x, y, width, height });
}

export function selectedAnnotationRegion(snapshot: StateSnapshot): CaptureRegion | null {
  return regionFromDisplayBox(annotateSidebarState(snapshot)?.selectedObject?.displayBox);
}

export function makeWorkspaceState(
  snapshot: StateSnapshot,
  canvasWidth = 1,
  canvasHeight = 1,
): BrowserWorkspaceState {
  const dimensions = captureDimensions(snapshot);
  return syncWorkspaceState(
    {
      initialized: false,
      workflow: snapshot.active_workflow,
      canvasWidth: Math.max(1, Math.round(canvasWidth)),
      canvasHeight: Math.max(1, Math.round(canvasHeight)),
      captureWidth: dimensions.width,
      captureHeight: dimensions.height,
      centerX: dimensions.width * 0.5,
      centerY: dimensions.height * 0.5,
      zoom: 1,
      clip: null,
      ...idleWorkspaceInteractionState(),
    },
    snapshot,
  );
}

export function workspaceNeedsReset(
  workspace: BrowserWorkspaceState,
  snapshot: StateSnapshot,
): boolean {
  const dimensions = captureDimensions(snapshot);
  return (
    !workspace.initialized ||
    workspace.workflow !== snapshot.active_workflow ||
    workspace.captureWidth !== dimensions.width ||
    workspace.captureHeight !== dimensions.height
  );
}

export function setWorkspaceCanvasSize(
  workspace: BrowserWorkspaceState,
  canvasWidth: number,
  canvasHeight: number,
): BrowserWorkspaceState {
  const width = Math.max(1, Math.round(canvasWidth));
  const height = Math.max(1, Math.round(canvasHeight));
  if (workspace.canvasWidth === width && workspace.canvasHeight === height) {
    return workspace;
  }
  return {
    ...workspace,
    canvasWidth: width,
    canvasHeight: height,
  };
}

function clampWorkspaceView(workspace: BrowserWorkspaceState): BrowserWorkspaceState {
  return {
    ...workspace,
    centerX: clamp(workspace.centerX, 0, workspace.captureWidth),
    centerY: clamp(workspace.centerY, 0, workspace.captureHeight),
    zoom: clamp(workspace.zoom, kMinWorkspaceZoom, kMaxWorkspaceZoom),
  };
}

export function fitWorkspaceToCapture(
  workspace: BrowserWorkspaceState,
): BrowserWorkspaceState {
  return clampWorkspaceView({
    ...workspace,
    centerX: workspace.captureWidth * 0.5,
    centerY: workspace.captureHeight * 0.5,
    zoom: 1,
  });
}

export function fitWorkspaceToClip(
  workspace: BrowserWorkspaceState,
  clip: CaptureRegion,
): BrowserWorkspaceState {
  if (clip.width <= 0 || clip.height <= 0) {
    return fitWorkspaceToCapture(workspace);
  }

  return clampWorkspaceView({
    ...workspace,
    centerX: clip.x + clip.width * 0.5,
    centerY: clip.y + clip.height * 0.5,
    zoom: clamp(
      Math.min(
        workspace.captureWidth / clip.width,
        workspace.captureHeight / clip.height,
      ) * 0.92,
      1,
      kMaxWorkspaceZoom,
    ),
  });
}

export function syncWorkspaceState(
  workspace: BrowserWorkspaceState,
  snapshot: StateSnapshot,
): BrowserWorkspaceState {
  if (workspaceNeedsReset(workspace, snapshot)) {
    const nextClip = snapshotClip(snapshot);
    const resetWorkspace: BrowserWorkspaceState = {
      ...workspace,
      initialized: true,
      workflow: snapshot.active_workflow,
      captureWidth: captureDimensions(snapshot).width,
      captureHeight: captureDimensions(snapshot).height,
      clip: nextClip,
      ...idleWorkspaceInteractionState(),
    };
    return nextClip === null
      ? fitWorkspaceToCapture(resetWorkspace)
      : fitWorkspaceToClip(resetWorkspace, nextClip);
  }

  if (
    workspace.mode === "idle" &&
    (snapshot.active_workflow === "predict" || snapshot.active_workflow === "live")
  ) {
    const nextClip = snapshotClip(snapshot);
    if (captureRegionEquals(workspace.clip, nextClip) && !workspace.clipDirty) {
      return workspace;
    }
    return {
      ...workspace,
      clip: nextClip,
      clipDirty: false,
    };
  }

  return workspace;
}

export function activeWorkspaceClip(
  workspace: BrowserWorkspaceState,
  snapshot: StateSnapshot,
): CaptureRegion | null {
  return workspace.clipDraft ?? workspace.clip ?? (snapshot.source.has_crop ? snapshot.source.crop : null);
}

export function getWorkspaceGeometry(
  workspace: BrowserWorkspaceState,
): WorkspaceGeometry {
  const canvasWidth = Math.max(1, workspace.canvasWidth);
  const canvasHeight = Math.max(1, workspace.canvasHeight);
  const padding = Math.max(24, Math.min(canvasWidth, canvasHeight) * 0.06);
  const fitScale = Math.max(
    0.0001,
    Math.min(
      (canvasWidth - padding * 2) / workspace.captureWidth,
      (canvasHeight - padding * 2) / workspace.captureHeight,
    ),
  );
  const scale = fitScale * workspace.zoom;
  return {
    canvasWidth,
    canvasHeight,
    fitScale,
    scale,
    frameX: canvasWidth * 0.5 - workspace.centerX * scale,
    frameY: canvasHeight * 0.5 - workspace.centerY * scale,
    frameWidth: workspace.captureWidth * scale,
    frameHeight: workspace.captureHeight * scale,
  };
}

export function capturePointFromCanvasPoint(
  workspace: BrowserWorkspaceState,
  point: WorkspaceCanvasPoint,
): WorkspaceCanvasPoint {
  const geometry = getWorkspaceGeometry(workspace);
  return {
    x: clamp((point.x - geometry.frameX) / geometry.scale, 0, workspace.captureWidth),
    y: clamp((point.y - geometry.frameY) / geometry.scale, 0, workspace.captureHeight),
  };
}

function regionFromPoints(
  workspace: BrowserWorkspaceState,
  anchor: WorkspaceCanvasPoint,
  current: WorkspaceCanvasPoint,
): CaptureRegion {
  const left = clamp(
    Math.floor(Math.min(anchor.x, current.x)),
    0,
    Math.max(0, workspace.captureWidth - 1),
  );
  const top = clamp(
    Math.floor(Math.min(anchor.y, current.y)),
    0,
    Math.max(0, workspace.captureHeight - 1),
  );
  const right = clamp(
    Math.ceil(Math.max(anchor.x, current.x)),
    left + 1,
    workspace.captureWidth,
  );
  const bottom = clamp(
    Math.ceil(Math.max(anchor.y, current.y)),
    top + 1,
    workspace.captureHeight,
  );
  return {
    x: left,
    y: top,
    width: right - left,
    height: bottom - top,
  };
}

export function beginWorkspaceInteraction(
  workspace: BrowserWorkspaceState,
  snapshot: StateSnapshot,
  start: WorkspaceInteractionStart,
): BrowserWorkspaceState {
  if (!supportsViewportIntent(snapshot.active_workflow)) {
    return workspace;
  }
  if (start.button !== 0 && start.button !== 2) {
    return workspace;
  }

  let nextWorkspace: BrowserWorkspaceState = {
    ...workspace,
    pointerId: start.pointerId,
    lastPointerX: start.point.x,
    lastPointerY: start.point.y,
    clipDraft: null,
    clipAnchor: null,
  };

  if (start.shiftKey || start.button === 2) {
    nextWorkspace = {
      ...nextWorkspace,
      mode: "clip",
      clipAnchor: capturePointFromCanvasPoint(nextWorkspace, start.point),
    };
  } else {
    nextWorkspace = {
      ...nextWorkspace,
      mode: "pan",
    };
  }

  return nextWorkspace;
}

export function moveWorkspaceInteraction(
  workspace: BrowserWorkspaceState,
  pointerId: number,
  point: WorkspaceCanvasPoint,
): BrowserWorkspaceState {
  if (workspace.pointerId !== pointerId) {
    return workspace;
  }

  let nextWorkspace: BrowserWorkspaceState = {
    ...workspace,
    lastPointerX: point.x,
    lastPointerY: point.y,
  };

  if (workspace.mode === "pan") {
    const geometry = getWorkspaceGeometry(workspace);
    nextWorkspace = clampWorkspaceView({
      ...nextWorkspace,
      centerX:
        workspace.centerX - (point.x - workspace.lastPointerX) / geometry.scale,
      centerY:
        workspace.centerY - (point.y - workspace.lastPointerY) / geometry.scale,
    });
  } else if (workspace.mode === "clip" && workspace.clipAnchor !== null) {
    nextWorkspace = {
      ...nextWorkspace,
      clipDraft: regionFromPoints(
        workspace,
        workspace.clipAnchor,
        capturePointFromCanvasPoint(workspace, point),
      ),
    };
  }

  return nextWorkspace;
}

export function finishWorkspaceInteraction(
  workspace: BrowserWorkspaceState,
  pointerId: number,
  shouldCommit: boolean,
): WorkspaceInteractionResult {
  if (workspace.pointerId !== pointerId) {
    return {
      workspace,
      commitViewport: false,
    };
  }

  let nextWorkspace = workspace;
  if (workspace.mode === "clip") {
    const draft = workspace.clipDraft;
    nextWorkspace = {
      ...nextWorkspace,
      clipDraft: null,
      clipAnchor: null,
    };
    if (shouldCommit && draft !== null && draft.width > 1 && draft.height > 1) {
      const nextClip = roundRegion(draft);
      nextWorkspace = fitWorkspaceToClip(
        {
          ...nextWorkspace,
          clip: nextClip,
          clipDirty: true,
        },
        nextClip,
      );
    }
  }

  return {
    workspace: {
      ...nextWorkspace,
      pointerId: null,
      mode: "idle",
    },
    commitViewport: shouldCommit,
  };
}

export function zoomWorkspaceAtCanvasPoint(
  workspace: BrowserWorkspaceState,
  point: WorkspaceCanvasPoint,
  deltaY: number,
): BrowserWorkspaceState {
  const capturePoint = capturePointFromCanvasPoint(workspace, point);
  const zoomFactor = Math.exp(-deltaY * 0.0014);
  const nextZoom = clamp(workspace.zoom * zoomFactor, kMinWorkspaceZoom, kMaxWorkspaceZoom);
  if (Math.abs(nextZoom - workspace.zoom) < 0.0001) {
    return workspace;
  }

  const zoomedWorkspace = {
    ...workspace,
    zoom: nextZoom,
  };
  const geometry = getWorkspaceGeometry(zoomedWorkspace);
  return clampWorkspaceView({
    ...zoomedWorkspace,
    centerX:
      capturePoint.x - (point.x - geometry.canvasWidth * 0.5) / geometry.scale,
    centerY:
      capturePoint.y - (point.y - geometry.canvasHeight * 0.5) / geometry.scale,
  });
}
