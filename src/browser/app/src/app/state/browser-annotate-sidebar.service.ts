import { computed, inject, Injectable } from "@angular/core";

import {
  annotateSelectedSummary,
  annotateSidebarState,
  capabilitySummaryText,
} from "../../app_shared";
import {
  annotateShellControlsState,
  annotateToolPaletteState,
  annotateWorkspaceSceneState,
  type AnnotateToolId,
  type AnnotationWorkspaceSceneState,
} from "../../browser_shell_state";
import type {
  AnnotateColorRangeState,
  AnnotateSidebarState,
  IntentMessage,
} from "../../host_api";
import { hostWorkspaceBoxDragKind } from "../host-annotation-intent";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import { BrowserWorkflowRouteState } from "./browser-workflow-route.service";

function annotateColorRangePayload(
  range: AnnotateColorRangeState,
): Record<string, unknown> {
  return {
    center: {
      hue_degrees: range.center.hueDegrees,
      saturation: range.center.saturation,
      value: range.center.value,
    },
    tolerance: {
      hue_minus_pct: range.tolerance.hueMinusPct,
      hue_plus_pct: range.tolerance.huePlusPct,
      saturation_minus_pct: range.tolerance.saturationMinusPct,
      saturation_plus_pct: range.tolerance.saturationPlusPct,
      value_minus_pct: range.tolerance.valueMinusPct,
      value_plus_pct: range.tolerance.valuePlusPct,
    },
    sampling: range.sampling,
  };
}

@Injectable({ providedIn: "root" })
export class BrowserAnnotateSidebarState {
  private readonly runtime = inject(BrowserHostRuntimeState);
  private readonly workflowRoute = inject(BrowserWorkflowRouteState);

  readonly selectedWorkflow = this.workflowRoute.selectedWorkflow;
  readonly sidebar = computed<AnnotateSidebarState | null>(() =>
    annotateSidebarState(this.runtime.snapshot()),
  );
  readonly controls = computed(() =>
    annotateShellControlsState(this.runtime.snapshot()),
  );
  readonly toolPalette = computed(() =>
    annotateToolPaletteState(this.runtime.snapshot()),
  );
  readonly toolPaletteSummary = computed(() =>
    capabilitySummaryText(
      this.toolPalette().statusItems,
      "annotate tool palette pending",
    ),
  );
  readonly workspaceScene = computed<AnnotationWorkspaceSceneState | null>(() =>
    annotateWorkspaceSceneState(this.runtime.snapshot()),
  );
  readonly brushRadius = computed(() => this.controls().brush.radius.value);
  readonly selectedSummary = computed(() => {
    return annotateSelectedSummary(this.sidebar());
  });
  readonly sidebarNote = computed(() => {
    const sidebar = this.sidebar();
    if (sidebar?.assistError) {
      return sidebar.assistError;
    }
    if (sidebar?.assistSummary) {
      return sidebar.assistSummary;
    }
    return this.toolPaletteSummary();
  });

  selectTool(tool: AnnotateToolId): IntentMessage | null {
    if (this.selectedWorkflow() !== "annotate") {
      return null;
    }
    return this.runtime.dispatch("annotate", "tool.select", { tool });
  }

  addCategory(categoryName: string): IntentMessage | null {
    const trimmed = categoryName.trim();
    if (trimmed.length === 0) {
      return null;
    }
    return this.dispatchSidebar("add_category", {
      category_name: trimmed,
    });
  }

  undo(): IntentMessage | null {
    return this.dispatchSidebar("undo");
  }

  redo(): IntentMessage | null {
    return this.dispatchSidebar("redo");
  }

  deleteSelectedObject(): IntentMessage | null {
    return this.dispatchSidebar("delete_selected");
  }

  selectObject(objectIndex: number | null): IntentMessage | null {
    if (this.workspaceScene()?.selectedObjectIndex === objectIndex) {
      return null;
    }
    return this.dispatchSidebar("select_object", {
      object_index: objectIndex,
    });
  }

  isCategorySelected(categoryIndex: number): boolean {
    const sidebar = this.sidebar();
    if (sidebar === null) {
      return false;
    }
    if (sidebar.selectedObject !== null) {
      return sidebar.selectedObject.categoryIndex === categoryIndex;
    }
    return sidebar.preferredNewObjectCategoryIndex === categoryIndex;
  }

  updateSelectedEnabled(enabled: boolean): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject === null || selectedObject === undefined) {
      return null;
    }
    return this.dispatchSidebar("update_selected", {
      enabled,
      category_index: selectedObject.categoryIndex,
    });
  }

  selectCategory(categoryIndex: number): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject === null || selectedObject === undefined) {
      return null;
    }
    return this.dispatchSidebar("update_selected", {
      enabled: selectedObject.enabled,
      category_index: categoryIndex,
    });
  }

  requestAssist(): IntentMessage | null {
    const sidebar = this.sidebar();
    if (sidebar === null || !sidebar.assistAvailable || sidebar.assistRunning) {
      return null;
    }
    return this.dispatchSidebar("assist");
  }

  redrawSelectedBox(): IntentMessage | null {
    const sidebar = this.sidebar();
    if (sidebar === null || sidebar.canRedrawSelectedBox !== true) {
      return null;
    }
    return this.dispatchSidebar("redraw_box");
  }

  runMaskCleanup(cleanupOp: string, cleanupRadius: number): IntentMessage | null {
    if (this.sidebar()?.canEditSelectedMask !== true) {
      return null;
    }
    return this.dispatchSidebar("mask.cleanup", {
      cleanup_op: cleanupOp,
      cleanup_radius: cleanupRadius,
    });
  }

  updateMaskColorRanges(
    sup: AnnotateColorRangeState,
    nosup: AnnotateColorRangeState,
  ): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject === null || selectedObject === undefined) {
      return null;
    }
    return this.dispatchSidebar("mask.update_color_ranges", {
      sup: annotateColorRangePayload(sup),
      nosup: annotateColorRangePayload(nosup),
    });
  }

  updateSplineActiveSegment(segmentIndex: number): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject?.spline === null || selectedObject?.spline === undefined) {
      return null;
    }
    return this.dispatchSidebar("spline.update_active_segment", {
      spline_segment_index: Math.max(0, Math.round(segmentIndex)),
    });
  }

  insertSplineKnot(): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject?.spline?.canInsertActiveSegmentKnot !== true) {
      return null;
    }
    return this.dispatchSidebar("spline.insert_active_knot");
  }

  setSplineHandleMode(handleMode: string): IntentMessage | null {
    if (handleMode !== "corner" && handleMode !== "smooth" && handleMode !== "mirrored") {
      return null;
    }
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject?.spline?.canEditActiveHandleMode !== true) {
      return null;
    }
    return this.dispatchSidebar("spline.set_handle_mode", {
      spline_handle_mode: handleMode,
    });
  }

  closeSpline(): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject?.spline?.canClose !== true) {
      return null;
    }
    return this.dispatchSidebar("spline.close");
  }

  reopenSpline(): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject?.spline?.canReopen !== true) {
      return null;
    }
    return this.dispatchSidebar("spline.reopen");
  }

  deleteSplineActiveKnot(): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject?.spline?.canDeleteActiveKnot !== true) {
      return null;
    }
    return this.dispatchSidebar("spline.delete_active_knot");
  }

  updateSkeletonActiveJoint(jointIndex: number): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject?.skeleton === null || selectedObject?.skeleton === undefined) {
      return null;
    }
    return this.dispatchSidebar("skeleton.update_active_joint", {
      skeleton_joint_index: Math.max(0, Math.round(jointIndex)),
    });
  }

  skipSkeletonJoint(): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject?.skeleton?.canSkipJoint !== true) {
      return null;
    }
    return this.dispatchSidebar("skeleton.skip_joint");
  }

  hideSkeletonJoint(): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject?.skeleton?.canHideActiveJoint !== true) {
      return null;
    }
    return this.dispatchSidebar("skeleton.hide_joint");
  }

  showSkeletonJoint(): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject?.skeleton?.canReactivateActiveJoint !== true) {
      return null;
    }
    return this.dispatchSidebar("skeleton.show_joint");
  }

  reseedSkeletonJoint(): IntentMessage | null {
    const selectedObject = this.sidebar()?.selectedObject;
    if (selectedObject?.skeleton?.canReseedActiveJoint !== true) {
      return null;
    }
    return this.dispatchSidebar("skeleton.reseed_joint");
  }

  dispatchWorkspaceCanvasClick(payload: Record<string, unknown>): IntentMessage | null {
    return this.dispatchWorkspaceIntent("annotate.workspace.click", {
      capture_x: payload.capture_x,
      capture_y: payload.capture_y,
      ...(payload.double_click === undefined ? {} : { double_click: payload.double_click }),
    });
  }

  dispatchWorkspaceHandleDrag(payload: Record<string, unknown>): IntentMessage | null {
    return this.dispatchWorkspaceIntent("annotate.workspace.handle_drag", {
      phase: payload.phase,
      capture_x: payload.capture_x,
      capture_y: payload.capture_y,
      handle: {
        object_index: payload.object_index,
        element_index: payload.element_index,
        role: payload.role,
      },
    });
  }

  dispatchWorkspaceBoxDrag(payload: Record<string, unknown>): IntentMessage | null {
    return this.dispatchWorkspaceIntent("annotate.workspace.box_drag", {
      phase: payload.phase,
      capture_x: payload.capture_x,
      capture_y: payload.capture_y,
      drag_kind: hostWorkspaceBoxDragKind(payload.drag_kind),
      object_index: payload.object_index,
    });
  }

  dispatchWorkspaceBrush(payload: Record<string, unknown>): IntentMessage | null {
    return this.dispatchWorkspaceIntent("annotate.workspace.brush", {
      phase: payload.phase,
      capture_x: payload.capture_x,
      capture_y: payload.capture_y,
      radius: payload.radius,
    });
  }

  dispatchWorkspacePointer(payload: Record<string, unknown>): IntentMessage | null {
    return this.dispatchWorkspaceIntent("annotate.workspace.pointer", payload);
  }

  dispatchWorkspaceFill(payload: Record<string, unknown>): IntentMessage | null {
    return this.dispatchWorkspaceIntent("annotate.workspace.fill", {
      capture_x: payload.capture_x,
      capture_y: payload.capture_y,
    });
  }

  dispatchWorkspaceColorSample(
    payload: Record<string, unknown>,
  ): IntentMessage | null {
    return this.dispatchWorkspaceIntent("annotate.workspace.color_sample", {
      capture_x: payload.capture_x,
      capture_y: payload.capture_y,
    });
  }

  private dispatchSidebar(
    action: string,
    payload: Record<string, unknown> = {},
  ): IntentMessage | null {
    if (this.selectedWorkflow() !== "annotate") {
      return null;
    }
    return this.runtime.dispatch("annotate", "annotate.sidebar", {
      action,
      ...payload,
    });
  }

  private dispatchWorkspaceIntent(
    intent: string,
    payload: Record<string, unknown>,
  ): IntentMessage | null {
    if (this.selectedWorkflow() !== "annotate") {
      return null;
    }
    const nextPayload =
      payload.object_index === null || payload.object_index === undefined
        ? (() => {
            const filteredPayload = { ...payload };
            Reflect.deleteProperty(filteredPayload, "object_index");
            return filteredPayload;
          })()
        : payload;
    return this.runtime.dispatch("annotate", intent, nextPayload);
  }
}
