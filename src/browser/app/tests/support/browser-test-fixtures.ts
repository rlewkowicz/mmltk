import type { StaticProvider } from "@angular/core";

import { BrowserAnnotateSidebarState } from "../../src/app/state/browser-annotate-sidebar.service";
import { BrowserAnnotateWorkflowState } from "../../src/app/state/browser-annotate-workflow.service";
import { BrowserExportWorkflowState } from "../../src/app/state/browser-export-workflow.service";
import { BrowserHostRuntimeState } from "../../src/app/state/browser-host-runtime.service";
import { BrowserLiveWorkflowState } from "../../src/app/state/browser-live-workflow.service";
import { BrowserModelConfigState } from "../../src/app/state/browser-model-config.service";
import { BrowserPredictWorkflowState } from "../../src/app/state/browser-predict-workflow.service";
import { BrowserShellChromeState } from "../../src/app/state/browser-shell-chrome.service";
import { BrowserShellSettingsState } from "../../src/app/state/browser-shell-settings.service";
import { BrowserSourceState } from "../../src/app/state/browser-source-state.service";
import { BrowserTrainWorkflowState } from "../../src/app/state/browser-train-workflow.service";
import { BrowserValidateWorkflowState } from "../../src/app/state/browser-validate-workflow.service";
import { BrowserWorkflowDraftSharedState } from "../../src/app/state/browser-workflow-draft-shared.service";
import { BrowserWorkflowDraftsState } from "../../src/app/state/browser-workflow-drafts.service";
import { BrowserWorkflowPanelsState } from "../../src/app/state/browser-workflow-panels.service";
import { BrowserWorkflowPrimaryActionState } from "../../src/app/state/browser-workflow-primary-action.service";
import { BrowserWorkflowRouteState } from "../../src/app/state/browser-workflow-route.service";
import { BrowserWorkspaceStateService } from "../../src/app/state/browser-workspace.service";
import type { StateSnapshot, WorkspaceSurfaceInfo } from "../../src/host_api";
import { HOST_API_CONTRACT_HASH } from "../../src/host_api.generated";
import { makeIdleBrowserHostJob } from "../../src/transport";

export type BrowserTestWindow = Window & typeof globalThis;

type JobOverrides = Partial<StateSnapshot["job"]>;
type SourceOverrides = Partial<StateSnapshot["source"]>;
type AnnotationOverrides = Partial<StateSnapshot["annotation"]>;
type RuntimeCapabilityOverrides = Partial<StateSnapshot["runtime_capabilities"]>;
type LivePreviewControlsOverrides = Partial<{
  fit_to_capture: Record<string, unknown>;
  full_frame_display: Record<string, unknown>;
}>;
type SnapshotOverrides = Omit<
  Partial<StateSnapshot>,
  "annotation" | "job" | "runtime_capabilities" | "source"
> & {
  annotation?: AnnotationOverrides;
  job?: JobOverrides;
  runtime_capabilities?: RuntimeCapabilityOverrides;
  source?: SourceOverrides;
};

export function makeBrowserTestSource(
  overrides: SourceOverrides = {},
): StateSnapshot["source"] {
  const source: StateSnapshot["source"] = {
    kind: "single_image",
    locator: "",
    recursive: false,
    device_index: 0,
    capture_width: 1280,
    capture_height: 720,
    capture_fps: 60,
    v4l2_buffer_count: 4,
    has_crop: false,
    crop: {
      x: 0,
      y: 0,
      width: 0,
      height: 0,
    },
  };

  return {
    ...source,
    ...overrides,
    crop: {
      ...source.crop,
      ...overrides.crop,
    },
  };
}

export function makeBrowserTestAnnotation(
  overrides: AnnotationOverrides = {},
): StateSnapshot["annotation"] {
  const annotation: StateSnapshot["annotation"] = {
    document_generation: 0,
    session_revision: 0,
    capture_width: 1280,
    capture_height: 720,
    instance_count: 0,
    selected_instance: null,
  };

  return {
    ...annotation,
    ...overrides,
  };
}

export function makeBrowserTestWorkspaceSurface(
  overrides: Partial<WorkspaceSurfaceInfo> = {},
): WorkspaceSurfaceInfo {
  return {
    surfaceId: "workspace",
    revision: "1",
    width: 1280,
    height: 720,
    textureFormat: "rgba8unorm",
    opaque: true,
    upright: true,
    ...overrides,
  };
}

export function makeAvailableBrowserTestRuntimeCapabilities(
  overrides: RuntimeCapabilityOverrides = {},
): StateSnapshot["runtime_capabilities"] {
  return {
    host_backend: "cef",
    navigator_gpu: "available",
    workspace_surface_bridge: "available",
    workspace_surface_zero_copy: "available",
    ...overrides,
  };
}

export function makeNativeBridgeContractCapabilities(): Record<string, unknown> {
  return {
    transport: {
      status: "ready",
      summary: "native bridge contract ready",
    },
    workspace_surface_zero_copy: {
      status: "ready",
      summary: "workspace zero-copy ready",
    },
  };
}

export function installBrowserTestWindow(windowValue: BrowserTestWindow): () => void {
  const priorDescriptor = Object.getOwnPropertyDescriptor(globalThis, "window");
  Object.defineProperty(globalThis, "window", {
    value: windowValue,
    configurable: true,
    writable: true,
  });
  return () => {
    if (priorDescriptor) {
      Object.defineProperty(globalThis, "window", priorDescriptor);
      return;
    }
    Reflect.deleteProperty(globalThis, "window");
  };
}

export function makeRuntimeWorkspaceProviders(): StaticProvider[] {
  return [
    { provide: BrowserHostRuntimeState, useFactory: () => new BrowserHostRuntimeState() },
    { provide: BrowserWorkflowRouteState, useFactory: () => new BrowserWorkflowRouteState() },
    {
      provide: BrowserWorkspaceStateService,
      useFactory: () => new BrowserWorkspaceStateService(),
    },
  ];
}

export function makeBrowserWorkflowServiceProviders(
  options: {
    annotateSidebar?: boolean;
    shellChrome?: boolean;
    shellSettings?: boolean;
    workflowDrafts?: boolean;
    workspace?: boolean;
  } = {},
): StaticProvider[] {
  const providers: StaticProvider[] = [
    { provide: BrowserHostRuntimeState, useFactory: () => new BrowserHostRuntimeState() },
    { provide: BrowserWorkflowRouteState, useFactory: () => new BrowserWorkflowRouteState() },
    { provide: BrowserExportWorkflowState, useFactory: () => new BrowserExportWorkflowState() },
    {
      provide: BrowserValidateWorkflowState,
      useFactory: () => new BrowserValidateWorkflowState(),
    },
    { provide: BrowserLiveWorkflowState, useFactory: () => new BrowserLiveWorkflowState() },
    { provide: BrowserTrainWorkflowState, useFactory: () => new BrowserTrainWorkflowState() },
    {
      provide: BrowserPredictWorkflowState,
      useFactory: () => new BrowserPredictWorkflowState(),
    },
    {
      provide: BrowserAnnotateWorkflowState,
      useFactory: () => new BrowserAnnotateWorkflowState(),
    },
    {
      provide: BrowserWorkflowDraftSharedState,
      useFactory: () => new BrowserWorkflowDraftSharedState(),
    },
    { provide: BrowserSourceState, useFactory: () => new BrowserSourceState() },
  ];

  if (options.shellChrome ?? true) {
    providers.push({
      provide: BrowserShellChromeState,
      useFactory: () => new BrowserShellChromeState(),
    });
  }
  if (options.workflowDrafts) {
    providers.push({
      provide: BrowserWorkflowDraftsState,
      useFactory: () => new BrowserWorkflowDraftsState(),
    });
  }
  if (options.shellSettings) {
    providers.push({
      provide: BrowserShellSettingsState,
      useFactory: () => new BrowserShellSettingsState(),
    });
  }
  if (options.workspace) {
    providers.push({
      provide: BrowserWorkspaceStateService,
      useFactory: () => new BrowserWorkspaceStateService(),
    });
  }
  providers.push(
    { provide: BrowserWorkflowPanelsState, useFactory: () => new BrowserWorkflowPanelsState() },
    {
      provide: BrowserWorkflowPrimaryActionState,
      useFactory: () => new BrowserWorkflowPrimaryActionState(),
    },
    {
      provide: BrowserModelConfigState,
      useFactory: () => new BrowserModelConfigState(),
    },
  );
  if (options.annotateSidebar) {
    providers.push({
      provide: BrowserAnnotateSidebarState,
      useFactory: () => new BrowserAnnotateSidebarState(),
    });
  }
  return providers;
}

export function makeBrowserShellStoreProviders(): StaticProvider[] {
  return makeBrowserWorkflowServiceProviders({
    shellSettings: true,
    workflowDrafts: true,
    workspace: true,
  });
}

export function makeLivePreviewControlsFixture(
  overrides: LivePreviewControlsOverrides = {},
): Record<string, unknown> {
  return {
    fit_to_capture: {
      value: false,
      enabled: true,
      intent: "live.preview.fit_to_capture",
      payload_key: "enabled",
      ...asRecord(overrides.fit_to_capture),
    },
    full_frame_display: {
      value: false,
      enabled: true,
      intent: "live.preview.full_frame_display",
      payload_key: "enabled",
      ...asRecord(overrides.full_frame_display),
    },
  };
}

export function makeBrowserTestWorkspaceBridgeSnapshot(
  overrides: SnapshotOverrides = {},
): StateSnapshot {
  const stateRevision = overrides.state_revision ?? 1;
  return makeBrowserTestSnapshot({
    active_workflow: "live",
    ...overrides,
    runtime_capabilities: makeAvailableBrowserTestRuntimeCapabilities(
      overrides.runtime_capabilities,
    ),
    workspace_surface:
      overrides.workspace_surface === undefined
        ? makeBrowserTestWorkspaceSurface({
            revision: String(stateRevision),
          })
        : overrides.workspace_surface,
  });
}

function asRecord(value: unknown): Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
}

export function makeLivePredictRuntimeFixture(
  overrides: Record<string, unknown> = {},
): Record<string, unknown> {
  const { preview, controller, ...rest } = overrides;
  return {
    active_mode: "predict",
    starting: false,
    stopping: false,
    show_running_section: true,
    ...rest,
    preview: {
      initialized: true,
      has_frame: true,
      displayed_region: {
        x: 0,
        y: 0,
        width: 1280,
        height: 720,
      },
      ...asRecord(preview),
    },
    controller: {
      present: true,
      running: true,
      last_error: "",
      ...asRecord(controller),
    },
  };
}

export function makeBrowserTestSnapshot(
  overrides: SnapshotOverrides = {},
): StateSnapshot {
  const snapshot: StateSnapshot = {
    type: "state.snapshot",
    protocol_version: 2,
    contract_hash: HOST_API_CONTRACT_HASH,
    state_revision: 1,
    active_workflow: "predict",
    workflow_state: {},
    settings_state: {},
    job: makeIdleBrowserHostJob(),
    source: makeBrowserTestSource(),
    annotation: makeBrowserTestAnnotation(),
    runtime_capabilities: {
      host_backend: "unknown",
      navigator_gpu: "unknown",
      workspace_surface_bridge: "unknown",
      workspace_surface_zero_copy: "unknown",
    },
    workspace_surface: null,
  };

  return {
    ...snapshot,
    ...overrides,
    job: {
      ...snapshot.job,
      ...overrides.job,
      recent_logs: overrides.job?.recent_logs ?? snapshot.job.recent_logs,
    },
    source: makeBrowserTestSource(overrides.source),
    annotation: makeBrowserTestAnnotation(overrides.annotation),
    runtime_capabilities: {
      ...snapshot.runtime_capabilities,
      ...overrides.runtime_capabilities,
    },
    workspace_surface:
      overrides.workspace_surface === undefined
        ? snapshot.workspace_surface
        : overrides.workspace_surface,
  };
}

export function setAnnotateWorkflowState(
  snapshot: StateSnapshot,
  activeTool: string,
  imageDirectory = "/datasets/annotate",
): void {
  const workflows =
    snapshot.workflow_state.workflows as Record<string, unknown> | undefined;
  snapshot.workflow_state.workflows = {
    ...(workflows ?? {}),
    annotate: {
      source: {
        kind: 2,
        image_directory: imageDirectory,
      },
      annotate: {
        active_tool: activeTool,
      },
    },
  };
}

export function makeAnnotateSidebarFixture(
  overrides: Record<string, unknown> = {},
): Record<string, unknown> {
  const selectedObject = overrides.selected_object;
  const sidebar = {
    has_annotation_frame: true,
    cleanup_radius: 1,
    assist_available: true,
    assist_running: false,
    assist_summary: "",
    assist_error: "",
    preferred_new_object_category_index: 0,
    has_classes: true,
    can_undo: true,
    can_redo: false,
    can_create_objects: true,
    can_delete_selected: true,
    can_redraw_selected_box: true,
    can_edit_selected_mask: true,
    has_selected_object: true,
    selected_object_index: 0,
    categories: [{ id: 1, name: "car" }],
    objects: [],
    selected_object: {
      enabled: true,
      category_index: 0,
      shape_label: "Mask",
      display_box: null,
      supports_mask_editing: true,
      sup: {},
      nosup: {},
      point: null,
      spline: null,
      skeleton: null,
    },
  };
  return {
    ...sidebar,
    ...overrides,
    selected_object:
      selectedObject === undefined ? sidebar.selected_object : selectedObject,
  };
}
