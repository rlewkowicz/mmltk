import {
  afterNextRender,
  ChangeDetectionStrategy,
  Component,
  DestroyRef,
  ElementRef,
  effect,
  inject,
  viewChild,
} from "@angular/core";

import type {
  CaptureRegion,
  StateSnapshot,
  WorkspaceGeometry,
} from "../../host_api";
import { captureRegionEquals } from "../../app_shared";
import {
  type AnnotationWorkspaceSceneState,
  type AnnotationWorkspaceBox,
  type AnnotateToolId,
} from "../../browser_shell_state";
import type { WorkspaceRenderInput } from "../../workspace_renderer";
import { BrowserAnnotateSidebarState } from "../state/browser-annotate-sidebar.service";
import { BrowserHostRuntimeState } from "../state/browser-host-runtime.service";
import { BrowserWorkflowRouteState } from "../state/browser-workflow-route.service";
import { BrowserWorkspaceStateService } from "../state/browser-workspace.service";
import {
  applyAnnotationWorkspaceBoxDrag,
  annotationWorkspaceCursorForDragKind,
  applyAnnotationWorkspaceCropDrag,
  buildAnnotationOverlayPrimitives,
  resolveAnnotationWorkspaceInteractionTarget,
  type AnnotationOverlayInteractionState,
  type AnnotationWorkspaceBoxDragKind,
  type AnnotationWorkspaceBoxDragPreview,
  type AnnotationWorkspaceCropDragState,
  type AnnotationWorkspaceHandleReference,
  type AnnotationWorkspaceInteractionTarget,
  type AnnotationWorkspaceRectDragKind,
} from "./annotation-scene-overlay";
import {
  type BrowserWorkspaceState,
  type WorkspaceCanvasPoint,
} from "./workspace-viewport";

type WorkspaceSelectionTarget = Exclude<
  AnnotationWorkspaceInteractionTarget,
  { kind: "crop" }
>;

type WorkspaceCanvasClickTool = Extract<
  AnnotateToolId,
  "point" | "spline" | "skeleton"
>;
type WorkspaceCanvasEditTool =
  | WorkspaceCanvasClickTool
  | "mask.fill"
  | "mask.color_sample";
type WorkspaceMaskTool = Extract<
  AnnotateToolId,
  "mask.paint" | "mask.erase" | "mask.fill"
>;
type WorkspaceMaskColorRangeKey = "sup" | "nosup";

function annotationBoxesEqual(
  left: AnnotationWorkspaceBox,
  right: AnnotationWorkspaceBox,
): boolean {
  return (
    left.x1 === right.x1 &&
    left.y1 === right.y1 &&
    left.x2 === right.x2 &&
    left.y2 === right.y2
  );
}

function handleReferenceFromTarget(
  target: AnnotationWorkspaceInteractionTarget | null,
): AnnotationWorkspaceHandleReference | null {
  if (target?.kind !== "editable_handle") {
    return null;
  }
  return {
    objectIndex: target.handle.objectIndex,
    elementIndex: target.handle.elementIndex,
    role: target.handle.role,
  };
}

function interactionTargetsEqual(
  left: AnnotationWorkspaceInteractionTarget | null,
  right: AnnotationWorkspaceInteractionTarget | null,
): boolean {
  if (left === right) {
    return true;
  }
  if (left === null || right === null || left.kind !== right.kind) {
    return false;
  }

  if (left.kind === "crop" && right.kind === "crop") {
    return left.dragKind === right.dragKind;
  }
  if (left.kind === "object" && right.kind === "object") {
    return left.object.index === right.object.index;
  }
  if (left.kind === "box" && right.kind === "box") {
    return (
      left.object.index === right.object.index &&
      left.dragKind === right.dragKind
    );
  }
  if (left.kind === "spline_segment" && right.kind === "spline_segment") {
    return (
      left.object.index === right.object.index &&
      left.segmentIndex === right.segmentIndex
    );
  }
  if (left.kind === "editable_handle" && right.kind === "editable_handle") {
    return (
      left.handle.objectIndex === right.handle.objectIndex &&
      left.handle.elementIndex === right.handle.elementIndex &&
      left.handle.role === right.handle.role
    );
  }
  return false;
}

@Component({
  selector: "mmltk-workspace-canvas",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <div class="workspace-shell">
      <canvas
        #workspaceCanvas
        class="workspace-canvas"
        aria-label="Workspace canvas"
      ></canvas>
    </div>
  `,
  styles: [
    `
      :host {
        display: block;
        width: 100%;
        height: 100%;
        min-height: 240px;
      }

      .workspace-shell {
        position: relative;
        width: 100%;
        height: 100%;
        overflow: hidden;
        background: transparent;
      }

      .workspace-canvas {
        position: absolute;
        inset: 0;
        display: block;
        width: 100%;
        height: 100%;
        touch-action: none;
      }
    `,
  ],
})
export class WorkspaceCanvasComponent {
  readonly runtime = inject(BrowserHostRuntimeState);
  readonly workflowRoute = inject(BrowserWorkflowRouteState);
  readonly workspace = inject(BrowserWorkspaceStateService);
  readonly annotate = inject(BrowserAnnotateSidebarState);

  private readonly destroyRef = inject(DestroyRef);
  private readonly workspaceCanvasRef =
    viewChild.required<ElementRef<HTMLCanvasElement>>("workspaceCanvas");

  private readonly listenerController = new AbortController();
  private ready = false;
  private disposed = false;
  private hoveredTarget: AnnotationWorkspaceInteractionTarget | null = null;
  private lastPointerPoint: WorkspaceCanvasPoint | null = null;
  private pendingPick: {
    pointerId: number;
    target: WorkspaceSelectionTarget;
    startX: number;
    startY: number;
    moved: boolean;
  } | null = null;
  private cropDrag:
    | (AnnotationWorkspaceCropDragState & {
        pointerId: number;
        draftClip: CaptureRegion;
      })
    | null = null;
  private pendingWorkspaceEditClick: {
    pointerId: number;
    tool: WorkspaceCanvasEditTool;
    target: WorkspaceSelectionTarget | null;
    captureX: number;
    captureY: number;
    startX: number;
    startY: number;
    moved: boolean;
  } | null = null;
  private boxDrag: {
    pointerId: number;
    objectIndex: number | null;
    dragKind: AnnotationWorkspaceBoxDragKind;
    startCaptureX: number;
    startCaptureY: number;
    lastCaptureX: number;
    lastCaptureY: number;
    startBox: AnnotationWorkspaceBox;
    previewBox: AnnotationWorkspaceBox;
  } | null = null;
  private handleDrag: {
    pointerId: number;
    handle: AnnotationWorkspaceHandleReference;
    lastCaptureX: number;
    lastCaptureY: number;
  } | null = null;
  private maskBrushStroke: {
    pointerId: number;
    tool: Extract<WorkspaceMaskTool, "mask.paint" | "mask.erase">;
    lastCaptureX: number;
    lastCaptureY: number;
  } | null = null;

  constructor() {
    effect(() => {
      this.workspace.workspaceRenderInput();
      this.annotate.workspaceScene();
      this.annotate.sidebar();
      this.annotate.toolPalette();
      this.workflowRoute.selectedWorkflow();
      this.runtime.snapshot();
      if (this.ready) {
        this.workspace.requestWorkspaceRender();
      }
    });

    afterNextRender(() => {
      this.initializeCanvas();
    });

    this.destroyRef.onDestroy(() => {
      this.dispose();
    });
  }

  private initializeCanvas(): void {
    if (this.disposed) {
      return;
    }
    const workspaceCanvas = this.workspaceCanvasRef().nativeElement;
    this.workspace.registerWorkspaceCanvas(
      workspaceCanvas,
      () => this.workspaceCanvasRenderInput(),
    );
    this.attachPointerHandlers(workspaceCanvas);
    this.ready = true;
    this.applyCanvasCursor();
    this.workspace.requestWorkspaceRender();
  }

  private attachPointerHandlers(canvas: HTMLCanvasElement): void {
    canvas.addEventListener(
      "contextmenu",
      (event) => {
        if (this.workspace.viewportEnabled()) {
          event.preventDefault();
        }
      },
      { signal: this.listenerController.signal },
    );

    canvas.addEventListener(
      "pointerdown",
      (event) => {
        const point = this.canvasPointFromEvent(canvas, event);
        this.lastPointerPoint = point;
        if (this.beginWorkspaceOwnedInteraction(canvas, event, point)) {
          this.workspace.requestWorkspaceRender();
          return;
        }
        const began = this.workspace.beginWorkspaceInteraction(
          event.pointerId,
          point.x,
          point.y,
          event.button,
          event.shiftKey,
        );
        if (!began) {
          return;
        }
        canvas.setPointerCapture(event.pointerId);
        event.preventDefault();
        this.workspace.requestWorkspaceRender();
      },
      { signal: this.listenerController.signal },
    );

    canvas.addEventListener(
      "pointermove",
      (event) => {
        const point = this.canvasPointFromEvent(canvas, event);
        let needsRender = false;
        if (this.cropDrag?.pointerId === event.pointerId) {
          this.lastPointerPoint = point;
          const nextClip = applyAnnotationWorkspaceCropDrag(
            this.cropDrag,
            this.workspace.workspaceGeometry(),
            point,
            this.workspace.workspaceState().captureWidth,
            this.workspace.workspaceState().captureHeight,
          );
          if (!captureRegionEquals(nextClip, this.cropDrag.draftClip)) {
            this.cropDrag = {
              ...this.cropDrag,
              draftClip: nextClip,
            };
            needsRender = true;
          }
          needsRender = this.syncHoveredTarget(point) || needsRender;
        } else if (this.boxDrag?.pointerId === event.pointerId) {
          this.lastPointerPoint = point;
          const capture = this.capturePointFromCanvasPoint(point, true);
          if (
            capture !== null &&
            (capture.captureX !== this.boxDrag.lastCaptureX ||
              capture.captureY !== this.boxDrag.lastCaptureY)
          ) {
            const nextPreview = applyAnnotationWorkspaceBoxDrag(
              {
                kind: this.boxDrag.dragKind,
                startCapturePoint: {
                  x: this.boxDrag.startCaptureX,
                  y: this.boxDrag.startCaptureY,
                },
                startBox: this.boxDrag.startBox,
              },
              {
                x: capture.captureX,
                y: capture.captureY,
              },
              this.workspace.workspaceState().captureWidth,
              this.workspace.workspaceState().captureHeight,
            );
            if (!annotationBoxesEqual(nextPreview, this.boxDrag.previewBox)) {
              this.boxDrag = {
                ...this.boxDrag,
                lastCaptureX: capture.captureX,
                lastCaptureY: capture.captureY,
                previewBox: nextPreview,
              };
              this.dispatchAnnotateWorkspaceEdit("box.drag", {
                phase: "update",
                drag_kind: this.boxDrag.dragKind,
                object_index: this.boxDrag.objectIndex,
                capture_x: capture.captureX,
                capture_y: capture.captureY,
              });
              needsRender = true;
            }
          }
        } else if (this.handleDrag?.pointerId === event.pointerId) {
          this.lastPointerPoint = point;
          const capture = this.capturePointFromCanvasPoint(point, true);
          if (
            capture !== null &&
            (capture.captureX !== this.handleDrag.lastCaptureX ||
              capture.captureY !== this.handleDrag.lastCaptureY)
          ) {
            this.handleDrag = {
              ...this.handleDrag,
              lastCaptureX: capture.captureX,
              lastCaptureY: capture.captureY,
            };
            this.dispatchAnnotateWorkspaceEdit("editable_handle.drag", {
              phase: "update",
              tool: "direct",
              object_index: this.handleDrag.handle.objectIndex,
              element_index: this.handleDrag.handle.elementIndex,
              role: this.handleDrag.handle.role,
              capture_x: capture.captureX,
              capture_y: capture.captureY,
            });
            needsRender = true;
          }
          needsRender = this.syncHoveredTarget(point) || needsRender;
        } else if (this.maskBrushStroke?.pointerId === event.pointerId) {
          this.lastPointerPoint = point;
          const capture = this.capturePointFromCanvasPoint(point, true);
          if (
            capture !== null &&
            (capture.captureX !== this.maskBrushStroke.lastCaptureX ||
              capture.captureY !== this.maskBrushStroke.lastCaptureY)
          ) {
            this.maskBrushStroke = {
              ...this.maskBrushStroke,
              lastCaptureX: capture.captureX,
              lastCaptureY: capture.captureY,
            };
            this.dispatchAnnotateWorkspaceEdit("mask.brush", {
              phase: "update",
              tool: this.maskBrushStroke.tool,
              capture_x: capture.captureX,
              capture_y: capture.captureY,
              radius: this.annotateBrushRadius(),
              erase: this.maskBrushStroke.tool === "mask.erase",
            });
            needsRender = true;
          }
          needsRender = this.syncHoveredTarget(point) || needsRender;
        } else if (
          this.pendingWorkspaceEditClick?.pointerId === event.pointerId
        ) {
          const moved =
            Math.hypot(
              point.x - this.pendingWorkspaceEditClick.startX,
              point.y - this.pendingWorkspaceEditClick.startY,
            ) > 8;
          if (moved !== this.pendingWorkspaceEditClick.moved) {
            this.pendingWorkspaceEditClick = {
              ...this.pendingWorkspaceEditClick,
              moved,
            };
            needsRender = true;
          }
          needsRender = this.syncHoveredTarget(point) || needsRender;
        } else if (this.pendingPick?.pointerId === event.pointerId) {
          const moved =
            Math.hypot(
              point.x - this.pendingPick.startX,
              point.y - this.pendingPick.startY,
            ) > 8;
          if (moved !== this.pendingPick.moved) {
            this.pendingPick = {
              ...this.pendingPick,
              moved,
            };
            needsRender = true;
          }
          needsRender = this.syncHoveredTarget(point) || needsRender;
        } else if (
          this.workspace.moveWorkspaceInteraction(event.pointerId, point.x, point.y)
        ) {
          needsRender = true;
          needsRender = this.syncHoveredTarget(point) || needsRender;
        } else {
          needsRender = this.syncHoveredTarget(point);
        }
        if (needsRender) {
          this.workspace.requestWorkspaceRender();
        }
      },
      { signal: this.listenerController.signal },
    );

    canvas.addEventListener(
      "pointerup",
      (event) => {
        if (canvas.hasPointerCapture(event.pointerId)) {
          canvas.releasePointerCapture(event.pointerId);
        }
        const point = this.canvasPointFromEvent(canvas, event);
        if (this.cropDrag?.pointerId === event.pointerId) {
          const drag = this.cropDrag;
          this.cropDrag = null;
          let needsRender = this.syncHoveredTarget(point);
          if (!captureRegionEquals(drag.startClip, drag.draftClip)) {
            this.workspace.workspaceState.update(
              (workspace: BrowserWorkspaceState) => ({
                ...workspace,
                clip: drag.draftClip,
                clipDraft: null,
                clipDirty: true,
              }),
            );
            needsRender = this.workspace.commitWorkspaceCrop() || needsRender;
          }
          if (needsRender) {
            this.workspace.requestWorkspaceRender();
          }
          return;
        }
        if (this.boxDrag?.pointerId === event.pointerId) {
          const drag = this.boxDrag;
          this.boxDrag = null;
          const capture = this.capturePointFromCanvasPoint(point, true) ?? {
            captureX: drag.lastCaptureX,
            captureY: drag.lastCaptureY,
          };
          this.dispatchAnnotateWorkspaceEdit("box.drag", {
            phase: "end",
            drag_kind: drag.dragKind,
            object_index: drag.objectIndex,
            capture_x: capture.captureX,
            capture_y: capture.captureY,
            cancelled: false,
          });
          this.syncHoveredTarget(point);
          this.workspace.requestWorkspaceRender();
          return;
        }
        if (this.handleDrag?.pointerId === event.pointerId) {
          const drag = this.handleDrag;
          this.handleDrag = null;
          const capture = this.capturePointFromCanvasPoint(point, true) ?? {
            captureX: drag.lastCaptureX,
            captureY: drag.lastCaptureY,
          };
          this.dispatchAnnotateWorkspaceEdit("editable_handle.drag", {
            phase: "end",
            tool: "direct",
            object_index: drag.handle.objectIndex,
            element_index: drag.handle.elementIndex,
            role: drag.handle.role,
            capture_x: capture.captureX,
            capture_y: capture.captureY,
            cancelled: false,
          });
          if (this.syncHoveredTarget(point)) {
            this.workspace.requestWorkspaceRender();
          }
          return;
        }
        if (this.maskBrushStroke?.pointerId === event.pointerId) {
          const stroke = this.maskBrushStroke;
          this.maskBrushStroke = null;
          const capture = this.capturePointFromCanvasPoint(point, true) ?? {
            captureX: stroke.lastCaptureX,
            captureY: stroke.lastCaptureY,
          };
          this.dispatchAnnotateWorkspaceEdit("mask.brush", {
            phase: "end",
            tool: stroke.tool,
            capture_x: capture.captureX,
            capture_y: capture.captureY,
            radius: this.annotateBrushRadius(),
            erase: stroke.tool === "mask.erase",
            cancelled: false,
          });
          if (this.syncHoveredTarget(point)) {
            this.workspace.requestWorkspaceRender();
          }
          return;
        }
        if (this.pendingWorkspaceEditClick?.pointerId === event.pointerId) {
          const pendingClick = this.pendingWorkspaceEditClick;
          this.pendingWorkspaceEditClick = null;
          const hoverChanged = this.syncHoveredTarget(point);
          if (!pendingClick.moved) {
            if (pendingClick.tool === "mask.color_sample") {
              this.dispatchAnnotateWorkspaceEdit("mask.color_sample", {
                capture_x: pendingClick.captureX,
                capture_y: pendingClick.captureY,
              });
            } else if (pendingClick.tool === "mask.fill") {
              this.commitWorkspaceSelectionIfPresent(pendingClick.target);
              this.dispatchAnnotateWorkspaceEdit("mask.fill", {
                tool: pendingClick.tool,
                capture_x: pendingClick.captureX,
                capture_y: pendingClick.captureY,
              });
            } else {
              this.commitWorkspaceSelectionIfPresent(pendingClick.target);
              this.dispatchAnnotateWorkspaceEdit("canvas.click", {
                tool: pendingClick.tool,
                capture_x: pendingClick.captureX,
                capture_y: pendingClick.captureY,
                default_category_index: this.defaultAnnotateCategoryIndex(),
              });
            }
          }
          if (hoverChanged || !pendingClick.moved) {
            this.workspace.requestWorkspaceRender();
          }
          return;
        }
        if (this.pendingPick?.pointerId === event.pointerId) {
          const pick = this.pendingPick;
          this.pendingPick = null;
          const hoverChanged = this.syncHoveredTarget(point);
          if (!pick.moved) {
            this.commitWorkspaceSelection(pick.target);
          }
          if (hoverChanged || !pick.moved) {
            this.workspace.requestWorkspaceRender();
          }
          return;
        }
        if (this.workspace.endWorkspaceInteraction(event.pointerId, true)) {
          this.workspace.requestWorkspaceRender();
        }
      },
      { signal: this.listenerController.signal },
    );

    canvas.addEventListener(
      "pointercancel",
      (event) => {
        if (canvas.hasPointerCapture(event.pointerId)) {
          canvas.releasePointerCapture(event.pointerId);
        }
        if (this.cropDrag?.pointerId === event.pointerId) {
          this.cropDrag = null;
          if (this.syncHoveredTarget(null)) {
            this.workspace.requestWorkspaceRender();
          }
          return;
        }
        if (this.boxDrag?.pointerId === event.pointerId) {
          const drag = this.boxDrag;
          this.boxDrag = null;
          this.dispatchAnnotateWorkspaceEdit("box.drag", {
            phase: "end",
            drag_kind: drag.dragKind,
            object_index: drag.objectIndex,
            capture_x: drag.lastCaptureX,
            capture_y: drag.lastCaptureY,
            cancelled: true,
          });
          this.syncHoveredTarget(null);
          this.workspace.requestWorkspaceRender();
          return;
        }
        if (this.handleDrag?.pointerId === event.pointerId) {
          const drag = this.handleDrag;
          this.handleDrag = null;
          this.dispatchAnnotateWorkspaceEdit("editable_handle.drag", {
            phase: "end",
            tool: "direct",
            object_index: drag.handle.objectIndex,
            element_index: drag.handle.elementIndex,
            role: drag.handle.role,
            capture_x: drag.lastCaptureX,
            capture_y: drag.lastCaptureY,
            cancelled: true,
          });
          if (this.syncHoveredTarget(null)) {
            this.workspace.requestWorkspaceRender();
          }
          return;
        }
        if (this.maskBrushStroke?.pointerId === event.pointerId) {
          const stroke = this.maskBrushStroke;
          this.maskBrushStroke = null;
          this.dispatchAnnotateWorkspaceEdit("mask.brush", {
            phase: "end",
            tool: stroke.tool,
            capture_x: stroke.lastCaptureX,
            capture_y: stroke.lastCaptureY,
            radius: this.annotateBrushRadius(),
            erase: stroke.tool === "mask.erase",
            cancelled: true,
          });
          if (this.syncHoveredTarget(null)) {
            this.workspace.requestWorkspaceRender();
          }
          return;
        }
        if (this.pendingWorkspaceEditClick?.pointerId === event.pointerId) {
          this.pendingWorkspaceEditClick = null;
          if (this.syncHoveredTarget(null)) {
            this.workspace.requestWorkspaceRender();
          }
          return;
        }
        if (this.pendingPick?.pointerId === event.pointerId) {
          this.pendingPick = null;
          if (this.syncHoveredTarget(null)) {
            this.workspace.requestWorkspaceRender();
          }
          return;
        }
        if (this.workspace.cancelWorkspaceInteraction(event.pointerId)) {
          this.workspace.requestWorkspaceRender();
        }
      },
      { signal: this.listenerController.signal },
    );

    canvas.addEventListener(
      "pointerleave",
      (event) => {
        if (canvas.hasPointerCapture(event.pointerId)) {
          return;
        }
        if (this.syncHoveredTarget(null)) {
          this.workspace.requestWorkspaceRender();
        }
      },
      { signal: this.listenerController.signal },
    );

    canvas.addEventListener(
      "wheel",
      (event) => {
        if (!this.workspace.viewportEnabled()) {
          return;
        }
        event.preventDefault();
        const point = this.canvasPointFromEvent(canvas, event);
        if (
          this.workspace.zoomWorkspaceAtCanvasPoint(point.x, point.y, event.deltaY)
        ) {
          this.workspace.requestWorkspaceRender();
        }
      },
      { passive: false, signal: this.listenerController.signal },
    );

    canvas.addEventListener(
      "dblclick",
      () => {
        if (!this.workspace.viewportEnabled()) {
          return;
        }
        this.workspace.fitWorkspaceToCapture(true);
        this.workspace.requestWorkspaceRender();
      },
      { signal: this.listenerController.signal },
    );
  }

  private workspaceCanvasRenderInput(): WorkspaceRenderInput {
    const geometry = this.workspace.workspaceGeometry();
    const snapshot = this.runtime.snapshot();
    const scene = this.annotationWorkspaceScene();
    this.syncHoveredTarget(this.lastPointerPoint, scene, geometry);
    const overlayPrimitives = buildAnnotationOverlayPrimitives(
      scene,
      geometry,
      this.overlayInteractionState(snapshot, scene),
      this.annotationBoxDragPreview(),
    );
    return {
      ...this.workspace.workspaceRenderInput(),
      overlayPrimitives,
    };
  }

  private canvasPointFromEvent(
    canvas: HTMLCanvasElement,
    event: PointerEvent | WheelEvent,
  ): WorkspaceCanvasPoint {
    const rect = canvas.getBoundingClientRect();
    const scaleX =
      rect.width > 0 ? this.workspace.workspaceState().canvasWidth / rect.width : 1;
    const scaleY =
      rect.height > 0
        ? this.workspace.workspaceState().canvasHeight / rect.height
        : 1;
    return {
      x: (event.clientX - rect.left) * scaleX,
      y: (event.clientY - rect.top) * scaleY,
    };
  }

  private annotationWorkspaceScene(): AnnotationWorkspaceSceneState | null {
    return this.annotate.workspaceScene();
  }

  private activeWorkspaceClip(): CaptureRegion | null {
    return this.cropDrag?.draftClip ?? this.workspace.workspaceClip();
  }

  private annotationBoxDragPreview(): AnnotationWorkspaceBoxDragPreview | null {
    if (this.boxDrag === null) {
      return null;
    }
    return {
      objectIndex: this.boxDrag.objectIndex,
      captureBox: this.boxDrag.previewBox,
      dragKind: this.boxDrag.dragKind,
    };
  }

  private activeMaskSamplingRange(): WorkspaceMaskColorRangeKey | null {
    const selectedObject = this.annotate.sidebar()?.selectedObject;
    if (selectedObject?.supportsMaskEditing !== true) {
      return null;
    }
    if (selectedObject.sup.sampling) {
      return "sup";
    }
    if (selectedObject.nosup.sampling) {
      return "nosup";
    }
    return null;
  }

  private activeBoxHoverTarget(
    scene: AnnotationWorkspaceSceneState | null,
  ): Extract<AnnotationWorkspaceInteractionTarget, { kind: "box" }> | null {
    if (
      this.boxDrag === null ||
      this.boxDrag.objectIndex === null ||
      this.boxDrag.dragKind === "create" ||
      scene === null
    ) {
      return null;
    }
    const object =
      scene.visibleObjects.find(
        (candidate) => candidate.index === this.boxDrag?.objectIndex,
      ) ?? null;
    if (object === null) {
      return null;
    }
    return {
      kind: "box",
      object,
      dragKind: this.boxDrag.dragKind,
    };
  }

  private resolveWorkspaceTarget(
    scene: AnnotationWorkspaceSceneState | null,
    geometry: WorkspaceGeometry,
    point: WorkspaceCanvasPoint | null,
  ): AnnotationWorkspaceInteractionTarget | null {
    if (point === null) {
      return null;
    }
    return resolveAnnotationWorkspaceInteractionTarget(
      scene,
      geometry,
      point,
      this.activeWorkspaceClip(),
    );
  }

  private syncHoveredTarget(
    point: WorkspaceCanvasPoint | null,
    scene: AnnotationWorkspaceSceneState | null = this.annotationWorkspaceScene(),
    geometry: WorkspaceGeometry = this.workspace.workspaceGeometry(),
  ): boolean {
    this.lastPointerPoint = point;
    const nextHovered =
      point === null
        ? null
        : this.cropDrag !== null
          ? {
              kind: "crop" as const,
              dragKind: this.cropDrag.kind,
            }
          : this.boxDrag !== null
            ? this.activeBoxHoverTarget(scene)
            : this.resolveWorkspaceTarget(scene, geometry, point);
    if (interactionTargetsEqual(nextHovered, this.hoveredTarget)) {
      this.applyCanvasCursor();
      return false;
    }
    this.hoveredTarget = nextHovered;
    this.applyCanvasCursor();
    return true;
  }

  private overlayInteractionState(
    snapshot: StateSnapshot,
    scene: AnnotationWorkspaceSceneState | null,
  ): AnnotationOverlayInteractionState {
    const hoveredTarget = this.hoveredTarget;
    const hoveredObjectIndex =
      hoveredTarget?.kind === "editable_handle"
        ? hoveredTarget.handle.objectIndex
        : hoveredTarget?.kind === "spline_segment"
          ? hoveredTarget.object.index
          : hoveredTarget?.kind === "box"
            ? hoveredTarget.object.index
            : hoveredTarget?.kind === "object"
              ? hoveredTarget.object.index
              : null;

    return {
      hoveredObjectIndex,
      hoveredHandle: handleReferenceFromTarget(hoveredTarget),
      focusedHandle: this.focusedHandleReference(scene),
      hoveredSplineSegment:
        hoveredTarget?.kind === "spline_segment"
          ? {
              objectIndex: hoveredTarget.object.index,
              segmentIndex: hoveredTarget.segmentIndex,
            }
          : null,
      activeSplineSegment: this.activeSplineSegmentReference(scene),
    };
  }

  private focusedHandleReference(
    scene: AnnotationWorkspaceSceneState | null,
  ): AnnotationWorkspaceHandleReference | null {
    if (scene?.selectedObjectIndex === null || scene === null) {
      return null;
    }
    const selected = this.annotate.sidebar()?.selectedObject;
    const activeKnotIndex = selected?.spline?.activeKnotIndex;
    if (activeKnotIndex !== null && activeKnotIndex !== undefined) {
      return {
        objectIndex: scene.selectedObjectIndex,
        elementIndex: activeKnotIndex,
        role: "spline_knot",
      };
    }
    const activeJointIndex = selected?.skeleton?.activeJointIndex;
    if (activeJointIndex !== null && activeJointIndex !== undefined) {
      return {
        objectIndex: scene.selectedObjectIndex,
        elementIndex: activeJointIndex,
        role: "skeleton_node",
      };
    }
    if (selected?.point !== null && selected?.point !== undefined) {
      return {
        objectIndex: scene.selectedObjectIndex,
        elementIndex: 0,
        role: "point",
      };
    }
    return null;
  }

  private activeSplineSegmentReference(
    scene: AnnotationWorkspaceSceneState | null,
  ): {
    objectIndex: number;
    segmentIndex: number;
  } | null {
    if (scene?.selectedObjectIndex === null || scene === null) {
      return null;
    }
    const activeSegmentIndex =
      this.annotate.sidebar()?.selectedObject?.spline
        ?.activeSegmentIndex ?? null;
    return activeSegmentIndex === null
      ? null
      : {
          objectIndex: scene.selectedObjectIndex,
          segmentIndex: activeSegmentIndex,
        };
  }

  private beginWorkspaceOwnedInteraction(
    canvas: HTMLCanvasElement,
    event: PointerEvent,
    point: WorkspaceCanvasPoint,
  ): boolean {
    if (event.button !== 0 || event.shiftKey) {
      return false;
    }

    const scene = this.annotationWorkspaceScene();
    const target = this.resolveWorkspaceTarget(
      scene,
      this.workspace.workspaceGeometry(),
      point,
    );

    if (target?.kind === "crop") {
      return this.beginCropDrag(canvas, event, point, target.dragKind);
    }
    if (this.workflowRoute.selectedWorkflow() !== "annotate") {
      return false;
    }

    const annotateTool = this.annotate.toolPalette().activeTool;
    if (this.activeMaskSamplingRange() !== null) {
      const selectionTarget = target;
      return this.beginWorkspaceEditClick(
        canvas,
        event,
        point,
        "mask.color_sample",
        selectionTarget,
      );
    }

    if (annotateTool === "direct" && target?.kind === "editable_handle") {
      return this.beginEditableHandleDrag(canvas, event, point, target);
    }
    if (annotateTool === "direct" && target?.kind === "box") {
      return this.beginBoxDrag(canvas, event, point, target);
    }
    if (annotateTool === "box") {
      return this.beginBoxCreateDrag(canvas, event, point);
    }

    if (
      this.isWorkspaceCanvasClickTool(annotateTool) ||
      annotateTool === "mask.fill"
    ) {
      return this.beginWorkspaceEditClick(
        canvas,
        event,
        point,
        annotateTool,
        target,
      );
    }

    if (this.isMaskBrushTool(annotateTool)) {
      return this.beginMaskBrushStroke(
        canvas,
        event,
        point,
        annotateTool,
        target,
      );
    }

    if (target === null) {
      return false;
    }

    this.pendingPick = {
      pointerId: event.pointerId,
      target,
      startX: point.x,
      startY: point.y,
      moved: false,
    };
    return this.claimWorkspacePointer(canvas, event, target);
  }

  private claimWorkspacePointer(
    canvas: HTMLCanvasElement,
    event: PointerEvent,
    hoveredTarget: WorkspaceSelectionTarget | null,
  ): boolean {
    this.hoveredTarget = hoveredTarget;
    this.applyCanvasCursor();
    canvas.setPointerCapture(event.pointerId);
    event.preventDefault();
    return true;
  }

  private beginWorkspaceEditClick(
    canvas: HTMLCanvasElement,
    event: PointerEvent,
    point: WorkspaceCanvasPoint,
    tool: WorkspaceCanvasEditTool,
    target: WorkspaceSelectionTarget | null,
  ): boolean {
    const capture = this.capturePointFromCanvasPoint(point);
    if (capture === null) {
      return false;
    }

    this.pendingWorkspaceEditClick = {
      pointerId: event.pointerId,
      tool,
      target,
      captureX: capture.captureX,
      captureY: capture.captureY,
      startX: point.x,
      startY: point.y,
      moved: false,
    };
    return this.claimWorkspacePointer(canvas, event, target);
  }

  private beginBoxCreateDrag(
    canvas: HTMLCanvasElement,
    event: PointerEvent,
    point: WorkspaceCanvasPoint,
  ): boolean {
    const capture = this.capturePointFromCanvasPoint(point);
    if (capture === null) {
      return false;
    }

    const previewBox: AnnotationWorkspaceBox = {
      x1: capture.captureX,
      y1: capture.captureY,
      x2: capture.captureX + 1,
      y2: capture.captureY + 1,
    };
    this.boxDrag = {
      pointerId: event.pointerId,
      objectIndex: null,
      dragKind: "create",
      startCaptureX: capture.captureX,
      startCaptureY: capture.captureY,
      lastCaptureX: capture.captureX,
      lastCaptureY: capture.captureY,
      startBox: previewBox,
      previewBox,
    };
    this.hoveredTarget = null;
    this.applyCanvasCursor();
    this.dispatchAnnotateWorkspaceEdit("box.drag", {
      phase: "begin",
      drag_kind: "create",
      capture_x: capture.captureX,
      capture_y: capture.captureY,
    });
    return this.claimWorkspacePointer(canvas, event, null);
  }

  private beginBoxDrag(
    canvas: HTMLCanvasElement,
    event: PointerEvent,
    point: WorkspaceCanvasPoint,
    target: Extract<WorkspaceSelectionTarget, { kind: "box" }>,
  ): boolean {
    const capture = this.capturePointFromCanvasPoint(point);
    if (capture === null) {
      return false;
    }

    this.commitWorkspaceSelectionIfPresent(target);
    this.boxDrag = {
      pointerId: event.pointerId,
      objectIndex: target.object.index,
      dragKind: target.dragKind,
      startCaptureX: capture.captureX,
      startCaptureY: capture.captureY,
      lastCaptureX: capture.captureX,
      lastCaptureY: capture.captureY,
      startBox: target.object.captureBox,
      previewBox: target.object.captureBox,
    };
    this.hoveredTarget = target;
    this.applyCanvasCursor();
    this.dispatchAnnotateWorkspaceEdit("box.drag", {
      phase: "begin",
      drag_kind: target.dragKind,
      object_index: target.object.index,
      capture_x: capture.captureX,
      capture_y: capture.captureY,
    });
    return this.claimWorkspacePointer(canvas, event, target);
  }

  private beginEditableHandleDrag(
    canvas: HTMLCanvasElement,
    event: PointerEvent,
    point: WorkspaceCanvasPoint,
    target: Extract<WorkspaceSelectionTarget, { kind: "editable_handle" }>,
  ): boolean {
    const capture = this.capturePointFromCanvasPoint(point);
    if (capture === null) {
      return false;
    }

    this.commitWorkspaceSelectionIfPresent(target);
    this.handleDrag = {
      pointerId: event.pointerId,
      handle: {
        objectIndex: target.handle.objectIndex,
        elementIndex: target.handle.elementIndex,
        role: target.handle.role,
      },
      lastCaptureX: capture.captureX,
      lastCaptureY: capture.captureY,
    };
    this.hoveredTarget = target;
    this.applyCanvasCursor();
    this.dispatchAnnotateWorkspaceEdit("editable_handle.drag", {
      phase: "begin",
      tool: "direct",
      object_index: target.handle.objectIndex,
      element_index: target.handle.elementIndex,
      role: target.handle.role,
      capture_x: capture.captureX,
      capture_y: capture.captureY,
    });
    canvas.setPointerCapture(event.pointerId);
    event.preventDefault();
    return true;
  }

  private beginMaskBrushStroke(
    canvas: HTMLCanvasElement,
    event: PointerEvent,
    point: WorkspaceCanvasPoint,
    tool: Extract<WorkspaceMaskTool, "mask.paint" | "mask.erase">,
    target: WorkspaceSelectionTarget | null,
  ): boolean {
    const capture = this.capturePointFromCanvasPoint(point);
    if (capture === null) {
      return false;
    }

    this.commitWorkspaceSelectionIfPresent(target);
    this.maskBrushStroke = {
      pointerId: event.pointerId,
      tool,
      lastCaptureX: capture.captureX,
      lastCaptureY: capture.captureY,
    };
    this.hoveredTarget = target;
    this.applyCanvasCursor();
    this.dispatchAnnotateWorkspaceEdit("mask.brush", {
      phase: "begin",
      tool,
      capture_x: capture.captureX,
      capture_y: capture.captureY,
      radius: this.annotateBrushRadius(),
      erase: tool === "mask.erase",
    });
    canvas.setPointerCapture(event.pointerId);
    event.preventDefault();
    return true;
  }

  private beginCropDrag(
    canvas: HTMLCanvasElement,
    event: PointerEvent,
    point: WorkspaceCanvasPoint,
    dragKind: AnnotationWorkspaceRectDragKind,
  ): boolean {
    const clip = this.activeWorkspaceClip();
    if (dragKind === "none" || clip === null || !this.workspace.viewportEnabled()) {
      return false;
    }

    this.cropDrag = {
      kind: dragKind,
      startPoint: point,
      startClip: clip,
      draftClip: clip,
      pointerId: event.pointerId,
    };
    this.hoveredTarget = {
      kind: "crop",
      dragKind,
    };
    this.applyCanvasCursor();
    canvas.setPointerCapture(event.pointerId);
    event.preventDefault();
    return true;
  }

  private commitWorkspaceSelection(target: WorkspaceSelectionTarget): void {
    if (target.kind === "object" || target.kind === "box") {
      this.selectAnnotationObject(target.object.index);
      return;
    }

    this.selectAnnotationObject(
      target.kind === "editable_handle"
        ? target.handle.objectIndex
        : target.object.index,
    );

    if (
      target.kind === "editable_handle" &&
      target.handle.role === "skeleton_node"
    ) {
      const activeJointIndex =
        this.annotate.sidebar()?.selectedObject?.skeleton
          ?.activeJointIndex ?? null;
      if (activeJointIndex !== target.handle.elementIndex) {
        this.annotate.updateSkeletonActiveJoint(target.handle.elementIndex);
      }
      return;
    }

    if (target.kind === "spline_segment") {
      const activeSegmentIndex =
        this.annotate.sidebar()?.selectedObject?.spline
          ?.activeSegmentIndex ?? null;
      if (activeSegmentIndex !== target.segmentIndex) {
        this.annotate.updateSplineActiveSegment(target.segmentIndex);
      }
    }
  }

  private commitWorkspaceSelectionIfPresent(
    target: WorkspaceSelectionTarget | null,
  ): void {
    if (target !== null) {
      this.commitWorkspaceSelection(target);
    }
  }

  private selectAnnotationObject(objectIndex: number): void {
    this.annotate.selectObject(objectIndex);
  }

  private capturePointFromCanvasPoint(
    point: WorkspaceCanvasPoint,
    clampToFrame = false,
  ): {
    captureX: number;
    captureY: number;
  } | null {
    const geometry = this.workspace.workspaceGeometry();
    const workspace = this.workspace.workspaceState();
    const frameRight = geometry.frameX + geometry.frameWidth;
    const frameBottom = geometry.frameY + geometry.frameHeight;
    const insideFrame =
      point.x >= geometry.frameX &&
      point.x <= frameRight &&
      point.y >= geometry.frameY &&
      point.y <= frameBottom;
    if (!clampToFrame && !insideFrame) {
      return null;
    }

    const clampedX = Math.min(frameRight, Math.max(geometry.frameX, point.x));
    const clampedY = Math.min(frameBottom, Math.max(geometry.frameY, point.y));
    const scale = Math.max(geometry.scale, 0.0001);
    return {
      captureX: Math.max(
        0,
        Math.min(
          workspace.captureWidth - 1,
          Math.round((clampedX - geometry.frameX) / scale),
        ),
      ),
      captureY: Math.max(
        0,
        Math.min(
          workspace.captureHeight - 1,
          Math.round((clampedY - geometry.frameY) / scale),
        ),
      ),
    };
  }

  private defaultAnnotateCategoryIndex(): number {
    return this.annotate.sidebar()?.preferredNewObjectCategoryIndex ?? 0;
  }

  private annotateBrushRadius(): number {
    return this.annotate.brushRadius();
  }

  private dispatchAnnotateWorkspaceEdit(
    action: string,
    payload: Record<string, unknown>,
  ): void {
    const nextPayload =
      payload.object_index === null || payload.object_index === undefined
        ? (() => {
            const filteredPayload = { ...payload };
            Reflect.deleteProperty(filteredPayload, "object_index");
            return filteredPayload;
          })()
        : payload;

    if (action === "canvas.click") {
      this.annotate.dispatchWorkspaceCanvasClick(nextPayload);
      return;
    }

    if (action === "editable_handle.drag") {
      this.annotate.dispatchWorkspaceHandleDrag(nextPayload);
      return;
    }

    if (action === "box.drag") {
      this.annotate.dispatchWorkspaceBoxDrag(nextPayload);
      return;
    }

    if (action === "mask.brush") {
      this.annotate.dispatchWorkspaceBrush(nextPayload);
      return;
    }

    if (action === "mask.fill") {
      this.annotate.dispatchWorkspaceFill(nextPayload);
      return;
    }

    if (action === "mask.color_sample") {
      this.annotate.dispatchWorkspaceColorSample(nextPayload);
    }
  }

  private isWorkspaceCanvasClickTool(
    tool: AnnotateToolId,
  ): tool is WorkspaceCanvasClickTool {
    return tool === "point" || tool === "spline" || tool === "skeleton";
  }

  private isMaskBrushTool(
    tool: AnnotateToolId,
  ): tool is Extract<WorkspaceMaskTool, "mask.paint" | "mask.erase"> {
    return tool === "mask.paint" || tool === "mask.erase";
  }

  private canvasCursor(): string {
    if (this.cropDrag !== null) {
      return annotationWorkspaceCursorForDragKind(this.cropDrag.kind, true);
    }
    if (this.boxDrag !== null) {
      return this.boxDrag.dragKind === "create"
        ? "crosshair"
        : annotationWorkspaceCursorForDragKind(this.boxDrag.dragKind, true);
    }
    if (this.handleDrag !== null) {
      return "grabbing";
    }
    if (
      this.maskBrushStroke !== null ||
      this.pendingWorkspaceEditClick !== null
    ) {
      return "crosshair";
    }

    const workspace = this.workspace.workspaceState();
    if (workspace.pointerId !== null) {
      return workspace.mode === "clip" ? "crosshair" : "grabbing";
    }

    if (this.activeMaskSamplingRange() !== null) {
      return "crosshair";
    }
    if (this.hoveredTarget?.kind === "editable_handle") {
      return "grab";
    }
    if (this.hoveredTarget?.kind === "box") {
      return this.annotate.toolPalette().activeTool === "direct"
        ? annotationWorkspaceCursorForDragKind(this.hoveredTarget.dragKind)
        : "pointer";
    }
    if (this.hoveredTarget?.kind === "crop") {
      return annotationWorkspaceCursorForDragKind(this.hoveredTarget.dragKind);
    }
    if (
      this.hoveredTarget?.kind === "spline_segment" ||
      this.hoveredTarget?.kind === "object"
    ) {
      return "pointer";
    }
    if (
      this.workflowRoute.selectedWorkflow() === "annotate" &&
      (this.annotate.toolPalette().activeTool === "box" ||
        this.isWorkspaceCanvasClickTool(
          this.annotate.toolPalette().activeTool,
        ) ||
        this.isMaskBrushTool(this.annotate.toolPalette().activeTool) ||
        this.annotate.toolPalette().activeTool === "mask.fill")
    ) {
      return "crosshair";
    }
    return this.workspace.viewportEnabled() ? "grab" : "default";
  }

  private applyCanvasCursor(): void {
    if (this.disposed) {
      return;
    }
    this.workspaceCanvasRef().nativeElement.style.cursor = this.canvasCursor();
  }

  private dispose(): void {
    this.disposed = true;
    this.listenerController.abort();
    if (this.ready) {
      this.workspace.unregisterWorkspaceCanvas(
        this.workspaceCanvasRef().nativeElement,
      );
    }
    this.cropDrag = null;
    this.pendingPick = null;
    this.pendingWorkspaceEditClick = null;
    this.boxDrag = null;
    this.handleDrag = null;
    this.maskBrushStroke = null;
    this.hoveredTarget = null;
    this.lastPointerPoint = null;
  }
}
