import {
  computed,
  DestroyRef,
  effect,
  inject,
  Injectable,
  signal,
  untracked,
} from "@angular/core";

import {
  capabilitySummaryText,
  captureRegionEquals,
  type CapabilityStatusViewState,
} from "../../app_shared";
import type {
  BrowserHostTransport,
  BrowserRuntimeCapabilities,
  CaptureRegion,
  StateSnapshot,
  Workflow,
} from "../../host_api";
import {
  runtimeWorkspaceSurfaceBridgeViability,
} from "../../runtime_frame_contract";
import {
  buildWorkspaceRenderFrame,
  type WorkspaceCanvasLayout,
  type WorkspaceRenderInput,
  workspaceCanvasLayoutsEqual,
} from "../../workspace_renderer";
import { WorkspaceWebGpuRenderer } from "../workspace/workspace-webgpu-renderer";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import { BrowserWorkflowRouteState } from "./browser-workflow-route.service";
import {
  activeWorkspaceClip,
  beginWorkspaceInteraction,
  finishWorkspaceInteraction,
  fitWorkspaceToCapture,
  fitWorkspaceToClip,
  getWorkspaceGeometry,
  makeWorkspaceState,
  moveWorkspaceInteraction as advanceWorkspaceInteraction,
  roundRegion,
  selectedAnnotationRegion,
  setWorkspaceCanvasSize,
  supportsViewportIntent,
  syncWorkspaceState,
  workspaceNeedsReset,
  zoomWorkspaceAtCanvasPoint,
  type BrowserWorkspaceState,
} from "../workspace/workspace-viewport";

const kViewportCommitDelayMs = 140;
const kWorkspaceSurfaceCommitDelayMs = 48;

type WorkspaceRenderInputFactory = () => WorkspaceRenderInput;

export interface WorkspaceDiagnosticsVm {
  framePathLabel: string;
  overlayPathLabel: string;
  detail: string;
  statusItems: CapabilityStatusViewState[];
}

export interface WorkspaceVm {
  title: string;
  gpuStatus: string;
}

export interface WorkspaceCanvasChromeVm {
  workflowLabel: string;
  statusLabel: string;
  fallbackLabel: string | null;
  viewportNote: string | null;
}

export interface WorkspaceHardErrorVm {
  code: "workspace_surface_bridge" | "workspace_surface_zero_copy";
  title: string;
  detail: string;
  recoveryHint: string;
}

interface WorkspaceFramePathVm {
  kind:
    | "mock_workspace_pending"
    | "mock_workspace_surface"
    | "workspace_canvas_pending"
    | "workspace_surface_bridge_pending"
    | "workspace_surface_bridge_blocked"
    | "workspace_zero_copy_pending"
    | "workspace_zero_copy_blocked"
    | "workspace_surface_pending"
    | "workspace_surface_bridge_target";
  label: string;
  detail: string;
  status: CapabilityStatusViewState["status"];
  fallbackLabel: string | null;
}

type WorkspaceFramePathStatusSurface = "canvas" | "gpu";

const kWorkspaceFramePathStatusLabels: Partial<
  Record<
    WorkspaceFramePathVm["kind"],
    Record<WorkspaceFramePathStatusSurface, string>
  >
> = {
  workspace_surface_bridge_target: {
    canvas: "surface bridge",
    gpu: "surface bridge",
  },
  workspace_surface_bridge_blocked: {
    canvas: "bridge blocked",
    gpu: "bridge blocked",
  },
  workspace_zero_copy_blocked: {
    canvas: "zero-copy blocked",
    gpu: "zero-copy blocked",
  },
  workspace_surface_bridge_pending: {
    canvas: "bridge pending",
    gpu: "bridge pending",
  },
  workspace_zero_copy_pending: {
    canvas: "zero-copy pending",
    gpu: "zero-copy pending",
  },
  mock_workspace_surface: {
    canvas: "mock workspace",
    gpu: "mock surface",
  },
};

function formatSurfaceRect(
  rect: {
    x: number;
    y: number;
    width: number;
    height: number;
  } | null,
): string {
  if (rect === null) {
    return "none";
  }
  return `${rect.x.toFixed(1)},${rect.y.toFixed(1)} · ${rect.width.toFixed(1)}x${rect.height.toFixed(1)}`;
}

function workspaceSurfaceStatusLabel(
  canvasLayout: WorkspaceCanvasLayout | null,
): string {
  if (canvasLayout === null) {
    return "surface-pending";
  }
  return canvasLayout.overlayPrimitiveCount > 0
    ? "surface-ready · overlay-active"
    : "surface-ready";
}

function workspaceViewportDetailText(
  canvasLayout: WorkspaceCanvasLayout | null,
): string {
  if (canvasLayout === null) {
    return "The workspace canvas has not published viewport bounds yet.";
  }
  return `viewport ${formatSurfaceRect(canvasLayout.viewportBoundsCss)} css px`;
}

function workspaceFramePathVm(
  transport: BrowserHostTransport,
  snapshot: StateSnapshot,
  runtimeCapabilities: BrowserRuntimeCapabilities,
  canvasLayout: WorkspaceCanvasLayout | null,
): WorkspaceFramePathVm {
  if (transport.mode === "mock") {
    if (canvasLayout === null) {
      return {
        kind: "mock_workspace_pending",
        label: "mock workspace pending",
        detail:
          "The mock transport is active, but the workspace canvas has not produced its first bridge frame yet.",
        status: "pending",
        fallbackLabel: "Workspace canvas pending",
      };
    }
    return {
      kind: "mock_workspace_surface",
      label: "mock workspace surface",
      detail:
        "Mock transport is driving workspace state while the single WebGPU canvas draws the current workspace surface and overlays.",
      status: "ready",
      fallbackLabel: null,
    };
  }
  if (canvasLayout === null) {
    return {
      kind: "workspace_canvas_pending",
      label: "workspace canvas pending",
      detail:
        "The workspace canvas has not produced its first bridge frame yet.",
      status: "pending",
      fallbackLabel: "Workspace canvas pending",
    };
  }
  const bridgeViability =
    runtimeWorkspaceSurfaceBridgeViability(runtimeCapabilities);
  if (bridgeViability === "blocked") {
    return {
      kind: "workspace_surface_bridge_blocked",
      label: "workspace surface bridge blocked",
      detail:
        "The workspace canvas is measured, but the runtime has not kept the workspace surface bridge available.",
      status: "blocked",
      fallbackLabel: "Workspace surface bridge unavailable",
    };
  }
  if (bridgeViability === "pending") {
    return {
      kind: "workspace_surface_bridge_pending",
      label: "workspace surface bridge pending",
      detail:
        "The workspace canvas is measured while the runtime finishes enabling the workspace surface bridge.",
      status: "pending",
      fallbackLabel: "Workspace surface bridge pending",
    };
  }
  const zeroCopyStatus =
    runtimeCapabilities.workspace_surface_zero_copy ?? "unknown";
  if (zeroCopyStatus === "unavailable") {
    return {
      kind: "workspace_zero_copy_blocked",
      label: "workspace zero-copy blocked",
      detail:
        "The runtime cannot alias the current workspace surface as a zero-copy GPU texture.",
      status: "blocked",
      fallbackLabel: "Workspace zero-copy unavailable",
    };
  }
  if (zeroCopyStatus !== "available") {
    return {
      kind: "workspace_zero_copy_pending",
      label: "workspace zero-copy pending",
      detail:
        "The workspace surface bridge is up, but the runtime has not confirmed zero-copy GPU texture aliasing yet.",
      status: "pending",
      fallbackLabel: "Workspace zero-copy pending",
    };
  }
  if (snapshot.workspace_surface === null || snapshot.workspace_surface === undefined) {
    return {
      kind: "workspace_surface_pending",
      label: "workspace surface pending",
      detail:
        "The workspace canvas is ready, but the runtime has not published a current workspace surface revision yet.",
      status: "pending",
      fallbackLabel: "Workspace surface pending",
    };
  }
  return {
    kind: "workspace_surface_bridge_target",
    label: "workspace surface bridge target",
    detail:
      "The browser/runtime pair is importing the current workspace surface revision through the dedicated GPU bridge and drawing overlays in the same canvas.",
    status: "ready",
    fallbackLabel: null,
  };
}

function workspaceFramePathStatus(
  transport: BrowserHostTransport,
  snapshot: StateSnapshot,
  runtimeCapabilities: BrowserRuntimeCapabilities,
  canvasLayout: WorkspaceCanvasLayout | null,
): CapabilityStatusViewState {
  const framePath = workspaceFramePathVm(
    transport,
    snapshot,
    runtimeCapabilities,
    canvasLayout,
  );
  return {
    key: "frame_path",
    label: "Frame Path",
    status: framePath.status,
    summary: `frame path ${framePath.label}`,
    detail: framePath.detail,
  };
}

function workspaceOverlayPathStatus(
  canvasLayout: WorkspaceCanvasLayout | null,
  overlayArmed: boolean,
): CapabilityStatusViewState {
  if (canvasLayout === null) {
    return {
      key: "overlay_path",
      label: "Overlay Path",
      status: "pending",
      summary: "overlay surface pending",
      detail: "overlay pending",
    };
  }
  if (overlayArmed || canvasLayout.overlayPrimitiveCount > 0) {
    return {
      key: "overlay_path",
      label: "Overlay Path",
      status: "ready",
      summary: "overlay path webgpu canvas overlay",
      detail: "webgpu canvas overlay",
    };
  }
  return {
    key: "overlay_path",
    label: "Overlay Path",
    status: "ready",
    summary: "overlay path webgpu idle",
    detail: "webgpu idle",
  };
}

function workspaceFallbackLabel(
  framePath: WorkspaceFramePathVm,
): string | null {
  return framePath.fallbackLabel;
}

function workspaceCanvasStatusLabel(framePath: WorkspaceFramePathVm): string {
  return (
    kWorkspaceFramePathStatusLabels[framePath.kind]?.canvas ?? "canvas pending"
  );
}

function workspaceGpuStatusLabel(
  framePath: WorkspaceFramePathVm,
): string {
  return kWorkspaceFramePathStatusLabels[framePath.kind]?.gpu ?? "canvas pending";
}

function workspaceViewportNote(
  viewportEnabled: boolean,
  canvasLayout: WorkspaceCanvasLayout | null,
  framePath: WorkspaceFramePathVm,
): string | null {
  if (!viewportEnabled) {
    return "viewport intents activate in predict, annotate, and live";
  }
  if (canvasLayout === null) {
    return framePath.fallbackLabel ?? "workspace canvas pending";
  }
  const viewportLabel = `viewport ${formatSurfaceRect(canvasLayout.viewportBoundsCss)} css px`;
  if (framePath.status === "blocked" && framePath.fallbackLabel !== null) {
    return `${framePath.fallbackLabel} · ${viewportLabel}`;
  }
  if (framePath.status === "pending" && framePath.fallbackLabel !== null) {
    return `${framePath.fallbackLabel} · ${viewportLabel}`;
  }
  return viewportLabel;
}

function workspaceHardErrorVm(
  framePath: WorkspaceFramePathVm,
): WorkspaceHardErrorVm | null {
  switch (framePath.kind) {
    case "workspace_surface_bridge_blocked":
      return {
        code: "workspace_surface_bridge",
        title: "Workspace surface bridge blocked",
        detail: framePath.detail,
        recoveryHint:
          "Restore the native workspace surface bridge to resume the dedicated WebGPU workspace canvas.",
      };
    case "workspace_zero_copy_blocked":
      return {
        code: "workspace_surface_zero_copy",
        title: "Workspace zero-copy blocked",
        detail: framePath.detail,
        recoveryHint:
          "Restore zero-copy GPU texture aliasing for the workspace surface bridge to resume the dedicated WebGPU workspace canvas.",
      };
    default:
      return null;
  }
}

function supportsCropCommit(workflow: Workflow): boolean {
  return workflow === "predict" || workflow === "annotate" || workflow === "live";
}

@Injectable({ providedIn: "root" })
export class BrowserWorkspaceStateService {
  private readonly destroyRef = inject(DestroyRef);
  private readonly runtime = inject(BrowserHostRuntimeState);
  private readonly workflowRoute = inject(BrowserWorkflowRouteState);
  private viewportCommitTimer: ReturnType<typeof globalThis.setTimeout> | null =
    null;
  private readonly rendererStatusOverride = signal<string | null>(null);
  private renderer = new WorkspaceWebGpuRenderer();
  private workspaceCanvas: HTMLCanvasElement | null = null;
  private workspaceRenderInputFactory: WorkspaceRenderInputFactory | null = null;
  private workspaceCanvasResizeObserver: ResizeObserver | null = null;
  private workspaceCanvasListenerController: AbortController | null = null;
  private renderFrameId: number | null = null;
  private renderWork: Promise<void> | null = null;
  private renderQueued = false;
  private renderGeneration = 0;

  readonly workspaceState = signal<BrowserWorkspaceState>(
    makeWorkspaceState(this.runtime.snapshot()),
  );
  readonly workspaceCanvasLayout = signal<WorkspaceCanvasLayout | null>(
    null,
  );

  readonly transportMode = computed(() => this.runtime.transportMode());
  readonly viewportEnabled = computed(() =>
    supportsViewportIntent(this.workflowRoute.selectedWorkflow()),
  );
  readonly selectedRegion = computed(() =>
    selectedAnnotationRegion(this.runtime.snapshot()),
  );
  readonly workspaceGeometry = computed(() =>
    getWorkspaceGeometry(this.workspaceState()),
  );
  readonly workspaceClip = computed(() =>
    activeWorkspaceClip(this.workspaceState(), this.runtime.snapshot()),
  );
  readonly rendererStatus = computed(() => {
    const override = this.rendererStatusOverride();
    if (override !== null) {
      return override;
    }
    return workspaceSurfaceStatusLabel(this.workspaceCanvasLayout());
  });
  readonly workspaceViewportDetail = computed(() =>
    workspaceViewportDetailText(this.workspaceCanvasLayout()),
  );
  readonly workspaceHardError = computed<WorkspaceHardErrorVm | null>(() =>
    workspaceHardErrorVm(
      workspaceFramePathVm(
        this.runtime.transport(),
        this.runtime.snapshot(),
        this.runtime.runtimeCapabilities(),
        this.workspaceCanvasLayout(),
      ),
    ),
  );
  readonly workspaceInteractionBlocked = computed(
    () => this.workspaceHardError() !== null,
  );
  readonly canCommitCrop = computed(() => {
    const clip = this.commitCandidateClip();
    if (clip === null) {
      return false;
    }
    const snapshotClip = this.runtime.snapshot().source.has_crop
      ? this.runtime.snapshot().source.crop
      : null;
    return !captureRegionEquals(roundRegion(clip), snapshotClip);
  });
  readonly canFitWorkspace = computed(
    () => !this.workspaceInteractionBlocked() && this.workspaceState().initialized,
  );
  readonly canFocusSelection = computed(
    () => !this.workspaceInteractionBlocked() && this.selectedRegion() !== null,
  );
  readonly canFocusCrop = computed(
    () => !this.workspaceInteractionBlocked() && this.workspaceClip() !== null,
  );
  readonly workspaceRenderInput = computed<WorkspaceRenderInput>(() => {
    const snapshot = this.runtime.snapshot();
    const workspaceState = this.workspaceState();
    const geometry = this.workspaceGeometry();
    return {
      workflow: this.workflowRoute.selectedWorkflow(),
      canvasWidth: geometry.canvasWidth,
      canvasHeight: geometry.canvasHeight,
      captureWidth: workspaceState.captureWidth,
      captureHeight: workspaceState.captureHeight,
      frameX: geometry.frameX,
      frameY: geometry.frameY,
      frameWidth: geometry.frameWidth,
      frameHeight: geometry.frameHeight,
      clip: this.workspaceClip(),
      annotationCount: snapshot.annotation.instance_count,
      overlayPrimitives: [],
    };
  });
  readonly workspaceDiagnostics = computed<WorkspaceDiagnosticsVm>(() => {
    const canvasLayout = this.workspaceCanvasLayout();
    const runtimeCapabilities = this.runtime.runtimeCapabilities();
    const framePath = workspaceFramePathVm(
      this.runtime.transport(),
      this.runtime.snapshot(),
      runtimeCapabilities,
      canvasLayout,
    );
    const framePathStatus = workspaceFramePathStatus(
      this.runtime.transport(),
      this.runtime.snapshot(),
      runtimeCapabilities,
      canvasLayout,
    );
    const overlayPathStatus = workspaceOverlayPathStatus(
      canvasLayout,
      this.workflowRoute.selectedWorkflow() === "annotate" ||
        this.runtime.snapshot().annotation.instance_count > 0,
    );
    const statusItems = [
      framePathStatus,
      overlayPathStatus,
      ...this.runtime.runtimeCapabilityStatus(),
      ...this.runtime.transportStatus().statusItems,
    ];
    return {
      framePathLabel: framePath.label,
      overlayPathLabel: overlayPathStatus.detail,
      detail: capabilitySummaryText(statusItems, this.workspaceViewportDetail()),
      statusItems,
    };
  });
  readonly workspace = computed<WorkspaceVm>(() => {
    const framePath = workspaceFramePathVm(
      this.runtime.transport(),
      this.runtime.snapshot(),
      this.runtime.runtimeCapabilities(),
      this.workspaceCanvasLayout(),
    );
    return {
      title: `${this.workflowRoute.selectedWorkflow()} workspace surface`,
      gpuStatus: workspaceGpuStatusLabel(framePath),
    };
  });
  readonly workspaceCanvasChrome = computed<WorkspaceCanvasChromeVm>(() => {
    const snapshot = this.runtime.snapshot();
    const framePath = workspaceFramePathVm(
      this.runtime.transport(),
      this.runtime.snapshot(),
      this.runtime.runtimeCapabilities(),
      this.workspaceCanvasLayout(),
    );
    return {
      workflowLabel: snapshot.active_workflow.toUpperCase(),
      statusLabel: `${workspaceCanvasStatusLabel(framePath)} · ${snapshot.annotation.instance_count} annotations · ${this.rendererStatus()}`,
      fallbackLabel: workspaceFallbackLabel(framePath),
      viewportNote: workspaceViewportNote(
        this.viewportEnabled(),
        this.workspaceCanvasLayout(),
        framePath,
      ),
    };
  });

  constructor() {
    effect(() => {
      const snapshot = this.runtime.snapshot();
      untracked(() => {
        if (workspaceNeedsReset(this.workspaceState(), snapshot)) {
          this.cancelScheduledViewportCommit();
          this.workspaceCanvasLayout.set(null);
          this.rendererStatusOverride.set(null);
        }
        this.workspaceState.update((current: BrowserWorkspaceState) =>
          syncWorkspaceState(current, snapshot),
        );
      });
    });

    effect(() => {
      const hardError = this.workspaceHardError();
      if (hardError === null) {
        return;
      }
      untracked(() => {
        this.cancelScheduledViewportCommit();
        this.workspaceState.update((current: BrowserWorkspaceState) => {
          if (
            current.pointerId === null &&
            current.mode === "idle" &&
            current.clipDraft === null &&
            current.clipAnchor === null
          ) {
            return current;
          }
          return {
            ...current,
            pointerId: null,
            mode: "idle",
            clipDraft: null,
            clipAnchor: null,
          };
        });
      });
    });

    this.destroyRef.onDestroy(() => {
      this.cancelScheduledViewportCommit();
      this.detachWorkspaceCanvas(false);
    });
  }

  registerWorkspaceCanvas(
    canvas: HTMLCanvasElement,
    renderInputFactory: WorkspaceRenderInputFactory,
  ): void {
    if (this.workspaceCanvas !== canvas) {
      this.detachWorkspaceCanvas(true);
      this.workspaceCanvas = canvas;
      this.workspaceRenderInputFactory = renderInputFactory;
      this.workspaceCanvasListenerController = new AbortController();
      this.workspaceCanvasResizeObserver = new ResizeObserver(() => {
        this.syncWorkspaceCanvasResolution();
      });
      this.workspaceCanvasResizeObserver.observe(canvas);
      globalThis.addEventListener(
        "resize",
        () => {
          this.syncWorkspaceCanvasResolution();
        },
        { signal: this.workspaceCanvasListenerController.signal },
      );
      this.renderer.attachCanvas(canvas);
    } else {
      this.workspaceRenderInputFactory = renderInputFactory;
    }
    this.syncWorkspaceCanvasResolution();
  }

  unregisterWorkspaceCanvas(canvas: HTMLCanvasElement): void {
    if (this.workspaceCanvas !== canvas) {
      return;
    }
    this.detachWorkspaceCanvas(true);
  }

  updateWorkspaceSurfaceReport(report: WorkspaceCanvasLayout | null): void {
    this.updateWorkspaceCanvasLayout(report);
  }

  requestWorkspaceRender(): void {
    if (
      this.workspaceCanvas === null ||
      this.workspaceRenderInputFactory === null ||
      this.renderFrameId !== null
    ) {
      return;
    }
    if (this.renderWork !== null) {
      this.renderQueued = true;
      return;
    }
    this.renderFrameId = globalThis.requestAnimationFrame(() => {
      this.renderFrameId = null;
      void this.drawWorkspaceCanvas();
    });
  }

  setRendererStatus(status: string): void {
    const nextStatus = status.trim();
    this.rendererStatusOverride.set(
      nextStatus.length > 0 ? nextStatus : null,
    );
  }

  updateWorkspaceCanvasLayout(
    report: WorkspaceCanvasLayout | null,
  ): void {
    if (workspaceCanvasLayoutsEqual(this.workspaceCanvasLayout(), report)) {
      return;
    }
    this.workspaceCanvasLayout.set(report);
    this.rendererStatusOverride.set(null);
    if (report !== null && this.viewportEnabled()) {
      this.scheduleViewportCommit(kWorkspaceSurfaceCommitDelayMs);
    }
  }

  setWorkspaceCanvasSize(canvasWidth: number, canvasHeight: number): void {
    this.workspaceState.update((current: BrowserWorkspaceState) =>
      setWorkspaceCanvasSize(current, canvasWidth, canvasHeight),
    );
    this.requestWorkspaceRender();
  }

  fitWorkspaceToCapture(commitViewport = false): void {
    if (this.workspaceInteractionBlocked()) {
      return;
    }
    this.workspaceState.update((current: BrowserWorkspaceState) =>
      fitWorkspaceToCapture(current),
    );
    if (commitViewport) {
      this.commitViewport();
    }
  }

  commitWorkspaceCrop(): boolean {
    const workflow = this.workflowRoute.selectedWorkflow();
    const clip = this.commitCandidateClip();
    if (clip === null) {
      return false;
    }
    const roundedClip = roundRegion(clip);
    this.workspaceState.update((current: BrowserWorkspaceState) => ({
      ...current,
      clip: roundedClip,
      clipDraft: null,
      clipDirty: false,
    }));
    this.runtime.dispatch(workflow, "crop.commit", {
      has_crop: true,
      crop: roundedClip,
    });
    return true;
  }

  private commitCandidateClip(): CaptureRegion | null {
    if (this.workspaceInteractionBlocked()) {
      return null;
    }
    if (!supportsCropCommit(this.workflowRoute.selectedWorkflow())) {
      return null;
    }
    return this.workspaceClip();
  }

  focusSelectedAnnotation(scheduleCommit = false): boolean {
    if (this.workspaceInteractionBlocked()) {
      return false;
    }
    const region = this.selectedRegion();
    if (region === null) {
      return false;
    }
    this.workspaceState.update((current: BrowserWorkspaceState) =>
      fitWorkspaceToClip(current, region),
    );
    if (scheduleCommit) {
      this.scheduleViewportCommit();
    }
    return true;
  }

  focusActiveClip(scheduleCommit = false): boolean {
    if (this.workspaceInteractionBlocked()) {
      return false;
    }
    const clip = this.workspaceClip();
    if (clip === null) {
      return false;
    }
    this.workspaceState.update((current: BrowserWorkspaceState) =>
      fitWorkspaceToClip(current, clip),
    );
    if (scheduleCommit) {
      this.scheduleViewportCommit();
    }
    return true;
  }

  beginWorkspaceInteraction(
    pointerId: number,
    canvasX: number,
    canvasY: number,
    button: number,
    shiftKey: boolean,
  ): boolean {
    if (this.workspaceInteractionBlocked()) {
      return false;
    }
    const nextWorkspace = beginWorkspaceInteraction(
      this.workspaceState(),
      this.runtime.snapshot(),
      {
        pointerId,
        point: { x: canvasX, y: canvasY },
        button,
        shiftKey,
      },
    );
    if (nextWorkspace === this.workspaceState()) {
      return false;
    }
    this.cancelScheduledViewportCommit();
    this.workspaceState.set(nextWorkspace);
    return true;
  }

  moveWorkspaceInteraction(
    pointerId: number,
    canvasX: number,
    canvasY: number,
  ): boolean {
    if (this.workspaceInteractionBlocked()) {
      return false;
    }
    const nextWorkspace = advanceWorkspaceInteraction(
      this.workspaceState(),
      pointerId,
      {
        x: canvasX,
        y: canvasY,
      },
    );
    if (nextWorkspace === this.workspaceState()) {
      return false;
    }
    this.workspaceState.set(nextWorkspace);
    return true;
  }

  endWorkspaceInteraction(pointerId: number, shouldCommit: boolean): boolean {
    if (this.workspaceInteractionBlocked()) {
      return false;
    }
    const result = finishWorkspaceInteraction(
      this.workspaceState(),
      pointerId,
      shouldCommit,
    );
    if (result.workspace === this.workspaceState() && !result.commitViewport) {
      return false;
    }
    this.workspaceState.set(result.workspace);
    if (result.commitViewport) {
      this.commitViewport();
    }
    return true;
  }

  cancelWorkspaceInteraction(pointerId: number): boolean {
    return this.endWorkspaceInteraction(pointerId, false);
  }

  zoomWorkspaceAtCanvasPoint(
    canvasX: number,
    canvasY: number,
    deltaY: number,
  ): boolean {
    if (!this.viewportEnabled() || this.workspaceInteractionBlocked()) {
      return false;
    }

    const nextWorkspace = zoomWorkspaceAtCanvasPoint(
      this.workspaceState(),
      {
        x: canvasX,
        y: canvasY,
      },
      deltaY,
    );
    if (nextWorkspace === this.workspaceState()) {
      return false;
    }
    this.workspaceState.set(nextWorkspace);
    this.scheduleViewportCommit();
    return true;
  }

  commitViewport(): boolean {
    this.cancelScheduledViewportCommit();
    if (this.workspaceInteractionBlocked()) {
      return false;
    }
    const workflow = this.workflowRoute.selectedWorkflow();
    if (!supportsViewportIntent(workflow)) {
      return false;
    }

    const workspaceState = this.workspaceState();
    const payload: Record<string, unknown> = {
      center_x: Number(workspaceState.centerX.toFixed(3)),
      center_y: Number(workspaceState.centerY.toFixed(3)),
      zoom: Number(workspaceState.zoom.toFixed(4)),
    };
    const clip = workspaceState.clip;
    const includeClip =
      clip !== null &&
      (workflow === "predict" || workflow === "live" || workspaceState.clipDirty);
    if (includeClip) {
      payload.clip = roundRegion(clip);
    }

    this.workspaceState.update((current: BrowserWorkspaceState) =>
      current.clipDirty ? { ...current, clipDirty: false } : current,
    );
    this.runtime.dispatch(workflow, "viewport.commit", payload);
    return true;
  }

  scheduleViewportCommit(delayMs = kViewportCommitDelayMs): void {
    if (this.workspaceInteractionBlocked()) {
      this.cancelScheduledViewportCommit();
      return;
    }
    this.cancelScheduledViewportCommit();
    this.viewportCommitTimer = globalThis.setTimeout(() => {
      this.viewportCommitTimer = null;
      this.commitViewport();
    }, delayMs);
  }

  cancelScheduledViewportCommit(): void {
    if (this.viewportCommitTimer !== null) {
      globalThis.clearTimeout(this.viewportCommitTimer);
      this.viewportCommitTimer = null;
    }
  }

  private syncWorkspaceCanvasResolution(): void {
    const canvas = this.workspaceCanvas;
    if (canvas === null) {
      return;
    }
    const rect = canvas.getBoundingClientRect();
    const devicePixelRatio = globalThis.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(rect.width * devicePixelRatio));
    const height = Math.max(1, Math.round(rect.height * devicePixelRatio));
    if (canvas.width !== width) {
      canvas.width = width;
    }
    if (canvas.height !== height) {
      canvas.height = height;
    }
    this.setWorkspaceCanvasSize(width, height);
  }

  private async drawWorkspaceCanvas(): Promise<void> {
    if (this.renderWork !== null) {
      this.renderQueued = true;
      return;
    }
    const renderGeneration = this.renderGeneration;
    const renderer = this.renderer;
    const work = this.performWorkspaceDraw(renderer, renderGeneration).finally(() => {
      if (this.renderWork === work) {
        this.renderWork = null;
      }
      if (this.renderQueued) {
        this.renderQueued = false;
        this.requestWorkspaceRender();
      }
    });
    this.renderWork = work;
    await work;
  }

  private async performWorkspaceDraw(
    renderer: WorkspaceWebGpuRenderer,
    renderGeneration: number,
  ): Promise<void> {
    const canvas = this.workspaceCanvas;
    const renderInputFactory = this.workspaceRenderInputFactory;
    if (canvas === null || renderInputFactory === null) {
      return;
    }

    try {
      const renderInput = renderInputFactory();
      const surfaceBounds = canvas.getBoundingClientRect();
      const renderFrame = buildWorkspaceRenderFrame(renderInput, {
        devicePixelRatio: globalThis.devicePixelRatio || 1,
        surfaceBoundsCss: {
          x: surfaceBounds.left,
          y: surfaceBounds.top,
          width: surfaceBounds.width,
          height: surfaceBounds.height,
        },
      });
      if (!this.isCurrentWorkspaceRender(renderGeneration, canvas, renderer)) {
        return;
      }
      this.updateWorkspaceCanvasLayout(renderFrame.canvasLayout);
      const stats = await renderer.render(renderFrame);
      if (!this.isCurrentWorkspaceRender(renderGeneration, canvas, renderer)) {
        return;
      }
      this.setRendererStatus(stats.statusText);
    } catch (error) {
      if (!this.isCurrentWorkspaceRender(renderGeneration, canvas, renderer)) {
        return;
      }
      const message =
        error instanceof Error && error.message.trim().length > 0
          ? error.message
          : "workspace render failed";
      this.setRendererStatus(`webgpu error · ${message}`);
    }
  }

  private isCurrentWorkspaceRender(
    renderGeneration: number,
    canvas: HTMLCanvasElement,
    renderer: WorkspaceWebGpuRenderer,
  ): boolean {
    return (
      renderGeneration === this.renderGeneration &&
      canvas === this.workspaceCanvas &&
      renderer === this.renderer
    );
  }

  private detachWorkspaceCanvas(recreateRenderer: boolean): void {
    this.renderGeneration += 1;
    if (this.renderFrameId !== null) {
      globalThis.cancelAnimationFrame(this.renderFrameId);
      this.renderFrameId = null;
    }
    this.renderQueued = false;
    this.workspaceCanvasListenerController?.abort();
    this.workspaceCanvasListenerController = null;
    this.workspaceCanvasResizeObserver?.disconnect();
    this.workspaceCanvasResizeObserver = null;
    this.workspaceCanvas = null;
    this.workspaceRenderInputFactory = null;
    this.workspaceCanvasLayout.set(null);
    this.rendererStatusOverride.set(null);
    if (recreateRenderer) {
      this.renderer.dispose();
      this.renderer = new WorkspaceWebGpuRenderer();
      return;
    }
    this.renderer.dispose();
  }
}
