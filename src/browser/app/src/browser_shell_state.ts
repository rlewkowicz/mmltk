import type { LiveFrameId, StateSnapshot, Workflow } from "./host_api";
import {
  annotateSidebarState,
  booleanFromValue,
  integerFromValue,
  isRecord,
  numberFromValue,
  statusItemFromContract,
  stringFromValue,
  type CapabilityStatusViewState,
  workflowFromValue as parseWorkflowValue,
  workflowSectionRecord,
  workflowSourceRecord,
  workflowStateRoot,
} from "./app_shared";

type JsonRecord = Record<string, unknown>;

export type AnnotateToolId =
  | "select"
  | "direct"
  | "box"
  | "mask.paint"
  | "mask.erase"
  | "mask.fill"
  | "point"
  | "spline"
  | "skeleton";

export interface LocalGpuDeviceViewState {
  deviceId: number;
  name: string;
  totalMemoryBytes: number;
  selected: boolean;
  label: string;
}

export interface TrainLocalGpuViewState {
  hasSnapshotState: boolean;
  hasEnumeratedDevices: boolean;
  visibleDevices: LocalGpuDeviceViewState[];
  selectedDeviceIds: number[];
  draftSelectedDeviceIds: number[];
  summary: string;
  error: string;
  warning: string;
  statusItems: CapabilityStatusViewState[];
  refreshRunning: boolean;
  refreshSupported: boolean;
  refreshIntent: string | null;
  refreshWorkflow: Workflow;
}

export interface LiveStatusViewState {
  hasSnapshotState: boolean;
  showRunningSection: boolean;
  showStaticPreview: boolean;
  previewHasFrame: boolean;
  controllerLabel: string;
  previewLabel: string;
  analyzerLabel: string;
  workspacePathLabel: string;
  backendLabel: string;
  latencyLabel: string;
  errorText: string;
  statusItems: CapabilityStatusViewState[];
  runtime: LiveRuntimeViewState;
  preview: LivePreviewViewState;
  controller: LiveControllerViewState;
  analyzer: LiveAnalyzerViewState;
  workspacePath: LiveWorkspacePathViewState;
}

export type LiveActiveMode = "predict" | "annotate" | "none";
export type LiveStartupState = "idle" | "starting" | "drawable" | "failed";
export type LiveDisplayStartupState =
  | "idle"
  | "capture running"
  | "surface published"
  | "texture imported"
  | "draw submitted"
  | "native presented";

export interface LiveRuntimeViewState {
  activeMode: LiveActiveMode;
  startupState: LiveStartupState;
  starting: boolean;
  stopping: boolean;
  showIdleStartError: boolean;
  videoStreamSource: boolean;
}

export interface LivePreviewViewState {
  initialized: boolean;
  hasFrame: boolean;
  nativePresented: boolean;
  width: number;
  height: number;
  frameId: number;
  liveFrameId: LiveFrameId | null;
  surfaceRevision: string;
  owner: string;
  publishNs: number;
  displayStartupState: LiveDisplayStartupState;
  rendererAcquiredRevision: string;
  rendererImportedRevision: string;
  rendererSubmittedRevision: string;
  rendererDrawnRevision: string;
  rendererReleasePendingRevision: string;
  rendererLastDrawError: string;
  rendererLastResultCode: string;
  rendererLastResultDetail: string;
  fitToCapture: boolean;
  cropOverlayMode: boolean;
  staticSourceName: string;
  interopFailed: boolean;
  error: string;
  lastError: string;
  lastFailureReason: string;
  lastFailureDetail: string;
  lastFailureRevision: string;
  lastFailureStage: string;
  lastFailureLiveFrameId: LiveFrameId | null;
  lastFailureCudaDevice: number | null;
  lastFailureResultCode: string;
  lastFailureResultDetail: string;
  publicationCounters: LivePreviewPublicationCountersViewState;
}

export interface LivePreviewPublicationCountersViewState {
  attemptedWorkspaceAcquisitions: number;
  startupWorkspaceAcquisitionMisses: number;
  postStartupWorkspaceAcquisitionMisses: number;
  retainedSurfaces: number;
  rejectedStaleFrames: number;
  cefExportFailures: number;
  rendererReleases: number;
  nativeReleaseFailures: number;
  rendererImportFailures: number;
  rendererReleaseRejections: number;
}

export interface LiveControllerViewState {
  present: boolean;
  running: boolean;
  lastError: string;
}

export interface LiveAnalyzerViewState {
  attached: boolean;
  modelHot: boolean;
  running: boolean;
  framesAnalyzed: number;
  framesSkipped: number;
  lastLatencyMs: number;
  backendName: string;
  lastError: string;
}

export interface LiveWorkspacePathViewState {
  running: boolean;
  framesPresented: number;
  framesDropped: number;
  lastFrameId: LiveFrameId | null;
  overlayActive: boolean;
  lastError: string;
}

export interface BrowserIntentActionViewState {
  enabled: boolean;
  workflow: Workflow;
  intent: string | null;
  label: string;
  payloadKey: string | null;
}

export interface BrowserIntentToggleViewState
  extends BrowserIntentActionViewState {
  value: boolean;
}

export interface BrowserIntentNumericViewState
  extends BrowserIntentActionViewState {
  value: number;
  min: number;
  max: number;
}

export interface AnnotateSetupFrameNavigationViewState {
  visible: boolean;
  currentIndex: number;
  inputCount: number;
  summary: string;
  reload: BrowserIntentActionViewState;
  previous: BrowserIntentActionViewState;
  next: BrowserIntentActionViewState;
}

export interface AnnotateLiveAnnotateControlsViewState {
  visible: boolean;
  running: boolean;
  summary: string;
  start: BrowserIntentActionViewState;
  stop: BrowserIntentActionViewState;
}

export interface AnnotateSaveControlsViewState {
  visible: boolean;
  summary: string;
  saveNow: BrowserIntentActionViewState;
  holdSave: BrowserIntentToggleViewState;
  holdSaveBlocked: boolean;
}

export interface AnnotateBrushControlsViewState {
  visible: boolean;
  summary: string;
  radius: BrowserIntentNumericViewState;
}

export interface AnnotateShellControlsViewState {
  statusItems: CapabilityStatusViewState[];
  setupFrame: AnnotateSetupFrameNavigationViewState;
  liveAnnotate: AnnotateLiveAnnotateControlsViewState;
  save: AnnotateSaveControlsViewState;
  brush: AnnotateBrushControlsViewState;
}

export interface LivePreviewControlsViewState {
  statusItems: CapabilityStatusViewState[];
  fitToCapture: BrowserIntentToggleViewState;
  fullFrameDisplay: BrowserIntentToggleViewState;
}

export interface AnnotateToolOption {
  id: AnnotateToolId;
  label: string;
  description: string;
  active: boolean;
  disabled: boolean;
}

export interface AnnotateToolPaletteState {
  activeTool: AnnotateToolId;
  options: AnnotateToolOption[];
  statusItems: CapabilityStatusViewState[];
}

export type AnnotationWorkspaceShapeType =
  | "box"
  | "mask"
  | "spline"
  | "point"
  | "skeleton";

export type AnnotationWorkspaceHandleRole =
  | "point"
  | "spline_knot"
  | "spline_in_handle"
  | "spline_out_handle"
  | "skeleton_node";

export interface AnnotationWorkspacePoint {
  x: number;
  y: number;
}

export interface AnnotationWorkspaceBox {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

export interface AnnotationWorkspaceEdge {
  sourceIndex: number;
  targetIndex: number;
}

export interface AnnotationWorkspaceObjectGeometryState {
  capturePoints: AnnotationWorkspacePoint[];
  edges: AnnotationWorkspaceEdge[];
  closed: boolean;
}

export interface AnnotationWorkspaceObjectState {
  index: number;
  categoryIndex: number;
  shapeType: AnnotationWorkspaceShapeType;
  captureBox: AnnotationWorkspaceBox;
  fullyVisible: boolean;
  geometry: AnnotationWorkspaceObjectGeometryState;
}

export interface AnnotationWorkspaceEditableHandleState {
  objectIndex: number;
  elementIndex: number;
  role: AnnotationWorkspaceHandleRole;
  categoryIndex: number;
  capturePoint: AnnotationWorkspacePoint;
  tetherCapturePoint: AnnotationWorkspacePoint | null;
  materialized: boolean;
}

export interface AnnotationWorkspaceBrushPreviewState {
  visible: boolean;
  capturePoint: AnnotationWorkspacePoint;
  radius: number;
  erase: boolean;
}

export interface AnnotationWorkspaceDenseMaskState {
  objectIndex: number;
  categoryIndex: number;
  captureBox: AnnotationWorkspaceBox;
  maskRle: string;
}

export type AnnotationWorkspaceSelectedDenseMaskState =
  AnnotationWorkspaceDenseMaskState;

export interface AnnotationWorkspaceSceneState {
  documentGeneration: number;
  selectedObjectIndex: number | null;
  captureWidth: number;
  captureHeight: number;
  visibleObjects: AnnotationWorkspaceObjectState[];
  editableHandles: AnnotationWorkspaceEditableHandleState[];
  brushPreview: AnnotationWorkspaceBrushPreviewState | null;
  visibleDenseMasks: AnnotationWorkspaceDenseMaskState[];
  selectedDenseMask: AnnotationWorkspaceSelectedDenseMaskState | null;
}

const kAnnotateToolSpecs: ReadonlyArray<{
  id: AnnotateToolId;
  label: string;
  description: string;
}> = [
  { id: "select", label: "Select", description: "Inspect and change the active object." },
  { id: "direct", label: "Direct", description: "Move existing handles and joints." },
  { id: "box", label: "Box", description: "Draw or redraw object boxes." },
  { id: "mask.paint", label: "Paint", description: "Paint into the selected mask." },
  { id: "mask.erase", label: "Erase", description: "Erase from the selected mask." },
  { id: "mask.fill", label: "Fill", description: "Flood fill the selected mask." },
  { id: "point", label: "Point", description: "Place point annotations." },
  { id: "spline", label: "Spline", description: "Create or adjust splines." },
  { id: "skeleton", label: "Skeleton", description: "Seed and edit skeleton joints." },
];

const kDefaultAnnotateBrushRadius = 12;
const kDefaultAnnotateBrushRadiusMin = 1;
const kDefaultAnnotateBrushRadiusMax = 128;

function firstRecord(candidates: ReadonlyArray<unknown>): JsonRecord | null {
  for (const candidate of candidates) {
    if (isRecord(candidate)) {
      return candidate;
    }
  }
  return null;
}

function liveActiveModeFromValue(value: unknown): LiveActiveMode {
  return value === "predict" || value === "annotate" ? value : "none";
}

function liveStartupStateFromValue(value: unknown): LiveStartupState {
  return value === "starting" || value === "drawable" || value === "failed"
    ? value
    : "idle";
}

function liveDisplayStartupStateFromValue(
  value: unknown,
  fallback: LiveDisplayStartupState,
): LiveDisplayStartupState {
  return value === "capture running" ||
    value === "surface published" ||
    value === "texture imported" ||
    value === "draw submitted" ||
    value === "native presented"
    ? value
    : fallback;
}

function liveFrameIdFromValue(value: unknown): LiveFrameId | null {
  if (!isRecord(value)) {
    return null;
  }
  const sessionNonce = integerFromValue(value.session_nonce, -1);
  const sequence = integerFromValue(value.sequence, -1);
  if (sessionNonce < 0 || sequence < 0) {
    return null;
  }
  return {
    session_nonce: sessionNonce,
    sequence,
  };
}

function joinStatusSegments(segments: ReadonlyArray<string>): string {
  return segments.filter((segment) => segment.length > 0).join(" · ");
}

function integerListFromValue(value: unknown): number[] {
  if (!Array.isArray(value)) {
    return [];
  }
  const parsed = value
    .map((entry) => Number(entry))
    .filter((entry) => Number.isFinite(entry) && entry >= 0)
    .map((entry) => Math.round(entry));
  return Array.from(new Set(parsed));
}

function workflowFromValue(value: unknown, fallback: Workflow): Workflow {
  return parseWorkflowValue(value, fallback);
}

function sourceKindFromValue(
  value: unknown,
): "compiled_dataset" | "single_image" | "image_folder" | "video_stream" {
  if (value === "compiled_dataset" || value === 0 || value === "0") {
    return "compiled_dataset";
  }
  if (value === "single_image" || value === 1 || value === "1") {
    return "single_image";
  }
  if (value === "image_folder" || value === 2 || value === "2") {
    return "image_folder";
  }
  return "video_stream";
}

function actionRecord(parent: JsonRecord | null, key: string): JsonRecord | null {
  if (parent === null) {
    return null;
  }
  if (isRecord(parent[key])) {
    return parent[key];
  }

  const flat: JsonRecord = {};
  const enabledKey = `${key}_enabled`;
  if (enabledKey in parent) {
    flat.enabled = parent[enabledKey];
  }
  const workflowKey = `${key}_workflow`;
  if (workflowKey in parent) {
    flat.workflow = parent[workflowKey];
  }
  const intentKey = `${key}_intent`;
  if (intentKey in parent) {
    flat.intent = parent[intentKey];
  }
  const payloadKey = `${key}_payload_key`;
  if (payloadKey in parent) {
    flat.payload_key = parent[payloadKey];
  }
  const labelKey = `${key}_label`;
  if (labelKey in parent) {
    flat.label = parent[labelKey];
  }
  const valueKey = `${key}_value`;
  if (valueKey in parent) {
    flat.value = parent[valueKey];
  }
  const activeKey = `${key}_active`;
  if (activeKey in parent) {
    flat.active = parent[activeKey];
    if (!("value" in flat)) {
      flat.value = parent[activeKey];
    }
  }
  const blockedKey = `${key}_blocked`;
  if (blockedKey in parent) {
    flat.blocked = parent[blockedKey];
  }
  const minKey = `${key}_min`;
  if (minKey in parent) {
    flat.min = parent[minKey];
  }
  const maxKey = `${key}_max`;
  if (maxKey in parent) {
    flat.max = parent[maxKey];
  }

  return Object.keys(flat).length > 0 ? flat : null;
}

function intentActionState(
  value: unknown,
  fallback: {
    enabled: boolean;
    workflow: Workflow;
    intent: string | null;
    label: string;
    payloadKey?: string | null;
  },
): BrowserIntentActionViewState {
  const record = isRecord(value) ? value : null;
  const intent = stringFromValue(
    record?.intent ?? record?.intent_name,
    fallback.intent ?? "",
  ).trim();
  return {
    enabled: booleanFromValue(record?.enabled, fallback.enabled),
    workflow: workflowFromValue(record?.workflow, fallback.workflow),
    intent: intent.length > 0 ? intent : null,
    label: stringFromValue(record?.label, fallback.label),
    payloadKey: stringFromValue(
      record?.payload_key,
      fallback.payloadKey ?? "",
    ).trim() || null,
  };
}

function intentToggleState(
  value: unknown,
  fallback: {
    value: boolean;
    enabled: boolean;
    workflow: Workflow;
    intent: string | null;
    label: string;
    payloadKey?: string | null;
  },
): BrowserIntentToggleViewState {
  const record = isRecord(value) ? value : null;
  return {
    ...intentActionState(record, fallback),
    value: booleanFromValue(
      record?.value ?? record?.active,
      booleanFromValue(value, fallback.value),
    ),
  };
}

function intentNumericState(
  value: unknown,
  fallback: {
    value: number;
    min: number;
    max: number;
    enabled: boolean;
    workflow: Workflow;
    intent: string | null;
    label: string;
    payloadKey?: string | null;
  },
): BrowserIntentNumericViewState {
  const record = isRecord(value) ? value : null;
  const min = Math.max(
    0,
    integerFromValue(record?.min, fallback.min),
  );
  const max = Math.max(
    min,
    integerFromValue(record?.max, fallback.max),
  );
  return {
    ...intentActionState(record, fallback),
    value: Math.min(
      max,
      Math.max(
        min,
        integerFromValue(record?.value ?? value, fallback.value),
      ),
    ),
    min,
    max,
  };
}

function formatMemoryGiB(totalMemoryBytes: number): string {
  if (!Number.isFinite(totalMemoryBytes) || totalMemoryBytes <= 0) {
    return "";
  }
  return `${(totalMemoryBytes / (1024 ** 3)).toFixed(1)} GiB`;
}

function summarizeSelectedGpuLabels(devices: ReadonlyArray<LocalGpuDeviceViewState>): string {
  const selected = devices.filter((device) => device.selected);
  if (selected.length === 0) {
    return "none selected";
  }
  return selected.map((device) => device.name || `GPU ${device.deviceId}`).join(", ");
}

function trainLocalGpuRecord(snapshot: StateSnapshot): JsonRecord | null {
  const root = workflowStateRoot(snapshot);
  return firstRecord([
    root?.train_local_gpu,
    root?.train_local_gpus,
    root?.local_gpu,
    root?.local_gpus,
    workflowSectionRecord(snapshot, "train", "local_gpu"),
    workflowSectionRecord(snapshot, "train", "local_gpus"),
    workflowSectionRecord(snapshot, "train", "train_local_gpu"),
  ]);
}

function trainingActivityRecord(snapshot: StateSnapshot): JsonRecord | null {
  const root = workflowStateRoot(snapshot);
  return firstRecord([
    root?.training_activity,
    root?.train_activity,
  ]);
}

function liveStatusRecord(snapshot: StateSnapshot): JsonRecord | null {
  const root = workflowStateRoot(snapshot);
  return firstRecord([
    root?.live_runtime,
    root?.live_status,
    root?.live_predict_activity,
    root?.live_predict,
    workflowSectionRecord(snapshot, "predict", "live_runtime"),
    workflowSectionRecord(snapshot, "predict", "live_status"),
    workflowSectionRecord(snapshot, "predict", "live_predict_activity"),
    workflowSectionRecord(snapshot, "predict", "live_predict"),
  ]);
}

function annotateControlsRecord(snapshot: StateSnapshot): JsonRecord | null {
  const root = workflowStateRoot(snapshot);
  return firstRecord([
    root?.annotate_controls,
    root?.annotate_shell_controls,
    root?.annotate_runtime_controls,
    workflowSectionRecord(snapshot, "annotate", "controls"),
    workflowSectionRecord(snapshot, "annotate", "shell_controls"),
    workflowSectionRecord(snapshot, "annotate", "runtime_controls"),
  ]);
}

function livePreviewControlsRecord(snapshot: StateSnapshot): JsonRecord | null {
  const root = workflowStateRoot(snapshot);
  const liveRuntime = liveStatusRecord(snapshot);
  return firstRecord([
    root?.live_preview_controls,
    liveRuntime?.preview_controls,
    workflowSectionRecord(snapshot, "live", "preview_controls"),
    workflowSectionRecord(snapshot, "predict", "preview_controls"),
  ]);
}

function localGpuDevices(
  record: JsonRecord | null,
  selectedDeviceIds: ReadonlyArray<number>,
): LocalGpuDeviceViewState[] {
  if (record === null) {
    return [];
  }
  const selectionFlags = Array.isArray(record.gpu_selection)
    ? record.gpu_selection.map((entry) => entry === true)
    : [];
  const selectedSet = new Set(selectedDeviceIds);
  const source = Array.isArray(record.visible_devices)
    ? record.visible_devices
    : Array.isArray(record.gpus)
      ? record.gpus
      : Array.isArray(record.devices)
        ? record.devices
        : [];

  return source
    .map((entry, index) => {
      if (!isRecord(entry)) {
        return null;
      }
      const deviceId = integerFromValue(
        entry.device_id ?? entry.id ?? entry.device_index,
        index,
      );
      const name = stringFromValue(entry.name, `GPU ${deviceId}`);
      const totalMemoryBytes = Math.max(
        0,
        integerFromValue(entry.total_memory_bytes ?? entry.memory_bytes, 0),
      );
      const selected =
        booleanFromValue(entry.selected, false) ||
        (index < selectionFlags.length ? selectionFlags[index] : false) ||
        selectedSet.has(deviceId);
      const memoryLabel = formatMemoryGiB(totalMemoryBytes);
      return {
        deviceId,
        name,
        totalMemoryBytes,
        selected,
        label: memoryLabel.length > 0
          ? `${name} · device ${deviceId} · ${memoryLabel}`
          : `${name} · device ${deviceId}`,
      };
    })
    .filter((entry): entry is LocalGpuDeviceViewState => entry !== null);
}

export function trainLocalGpuState(
  snapshot: StateSnapshot,
  draftSelectedDeviceIds?: ReadonlyArray<number>,
): TrainLocalGpuViewState {
  const record = trainLocalGpuRecord(snapshot);
  const activity = trainingActivityRecord(snapshot);
  const training = workflowSectionRecord(snapshot, "train", "training");
  const selectedDeviceIds = integerListFromValue(
    record?.selected_device_ids ??
      record?.configured_device_ids ??
      record?.local_device_ids ??
      training?.local_device_ids,
  );
  const draftIds =
    draftSelectedDeviceIds !== undefined
      ? Array.from(new Set(draftSelectedDeviceIds))
      : selectedDeviceIds;
  const devices = localGpuDevices(record, draftIds);
  const summary =
    stringFromValue(record?.selection_summary) ||
    stringFromValue(record?.summary) ||
    stringFromValue(record?.local_gpu_summary) ||
    stringFromValue(activity?.local_gpu_summary) ||
    (devices.length > 0
      ? `${draftIds.length}/${devices.length} selected · ${summarizeSelectedGpuLabels(devices)}`
      : draftIds.length > 0
        ? `Manual device IDs: ${draftIds.join(", ")}`
        : "No local GPUs selected.");
  const error =
    stringFromValue(record?.error) ||
    stringFromValue(record?.gpu_error) ||
    stringFromValue(activity?.local_gpu_error);
  const banner = isRecord(record?.banner) ? record.banner : null;
  const warning =
    error.length > 0
      ? ""
      : stringFromValue(banner?.message) ||
        (devices.length === 0 && record !== null
          ? "No visible CUDA GPUs were found for local training."
          : draftIds.length === 0
            ? "Select at least one local GPU before running training."
            : "");
  const refreshRunning = booleanFromValue(
    record?.refresh_running ?? record?.gpu_refresh_running,
    false,
  );
  const refreshIntent = "train.local_gpu.refresh";
  const refreshWorkflowRaw = stringFromValue(record?.refresh_workflow, "train");
  const refreshWorkflow: Workflow =
    refreshWorkflowRaw === "train" ||
    refreshWorkflowRaw === "validate" ||
    refreshWorkflowRaw === "predict" ||
    refreshWorkflowRaw === "annotate" ||
    refreshWorkflowRaw === "export" ||
    refreshWorkflowRaw === "live"
      ? refreshWorkflowRaw
      : "train";
  const refreshSupported =
    booleanFromValue(record?.refresh_supported, refreshIntent.length > 0) ||
    refreshIntent.length > 0;
  const statusItems: CapabilityStatusViewState[] = [
    statusItemFromContract(record, "inventory", {
      key: "inventory",
      label: "Inventory",
      status:
        devices.length > 0
          ? "ready"
          : record !== null
            ? "blocked"
            : "pending",
      summary:
        devices.length > 0
          ? `${devices.length} local GPU${devices.length === 1 ? "" : "s"} published`
          : record !== null
            ? "local GPU inventory published without visible devices"
            : "local GPU inventory pending",
      detail:
        devices.length > 0
          ? summary
          : "The native snapshot has not published visible local GPU inventory yet.",
    }),
    statusItemFromContract(record, "selection", {
      key: "selection",
      label: "Selection",
      status: draftIds.length > 0 ? "active" : "blocked",
      summary:
        draftIds.length > 0
          ? `draft device IDs ${draftIds.join(", ")}`
          : "no draft GPU selection",
      detail:
        draftIds.length > 0
          ? `Training will use local device IDs ${draftIds.join(", ")}.`
          : "Select at least one local GPU before running training.",
    }),
    statusItemFromContract(record, "refresh", {
      key: "refresh",
      label: "Refresh",
      status: refreshRunning ? "active" : refreshSupported ? "ready" : "blocked",
      summary: refreshRunning
        ? "GPU inventory refresh running"
        : refreshSupported
          ? "GPU inventory refresh ready"
          : "GPU inventory refresh unavailable",
      detail: refreshSupported
        ? "Inventory refresh dispatches train.local_gpu.refresh."
        : "Inventory refresh is not currently exposed by the native snapshot.",
    }),
  ];

  return {
    hasSnapshotState: record !== null || activity !== null,
    hasEnumeratedDevices: devices.length > 0,
    visibleDevices: devices,
    selectedDeviceIds,
    draftSelectedDeviceIds: draftIds,
    summary,
    error,
    warning,
    statusItems,
    refreshRunning,
    refreshSupported,
    refreshIntent: refreshIntent.length > 0 ? refreshIntent : null,
    refreshWorkflow,
  };
}

export function liveStatusState(snapshot: StateSnapshot): LiveStatusViewState {
  const record = liveStatusRecord(snapshot);
  const preview = isRecord(record?.preview) ? record.preview : null;
  const controller = isRecord(record?.controller) ? record.controller : null;
  const analyzer = isRecord(record?.analyzer) ? record.analyzer : null;
  const manualOverlay = isRecord(record?.manual_overlay) ? record.manual_overlay : null;
  const compositor = isRecord(record?.compositor) ? record.compositor : null;
  const showRunningSection = booleanFromValue(record?.show_running_section, false);
  const showStaticPreview = booleanFromValue(record?.show_static_preview, false);
  const previewRegion =
    preview?.displayed_region && isRecord(preview.displayed_region)
      ? preview.displayed_region
      : null;
  const previewPublishedLiveFrameId = liveFrameIdFromValue(preview?.live_frame_id);
  const compositorPublishedLastFrameId = liveFrameIdFromValue(compositor?.last_frame_id);
  const previewPublishedFrameId = Math.max(
    0,
    integerFromValue(preview?.frame_id ?? record?.preview_frame_id, 0),
  );
  const previewPublishedWidth = Math.max(
    0,
    integerFromValue(previewRegion?.width ?? record?.preview_width, 0),
  );
  const previewPublishedHeight = Math.max(
    0,
    integerFromValue(previewRegion?.height ?? record?.preview_height, 0),
  );
  const staticPreviewSource = stringFromValue(
    preview?.static_source_name ?? record?.static_preview_source_name,
  );
  const runtime: LiveRuntimeViewState = {
    activeMode: liveActiveModeFromValue(record?.active_mode),
    startupState: liveStartupStateFromValue(record?.startup_state),
    starting: booleanFromValue(record?.starting, false),
    stopping: booleanFromValue(record?.stopping, false),
    showIdleStartError: booleanFromValue(record?.show_idle_start_error, false),
    videoStreamSource: booleanFromValue(
      record?.video_stream_source,
      snapshot.source.kind === "video_stream",
    ),
  };
  const controllerPresentPublished =
    controller !== null &&
    Object.prototype.hasOwnProperty.call(controller, "present");
  const controllerRunning = booleanFromValue(
    controller?.running ?? record?.controller_running,
    false,
  );
  const controllerState: LiveControllerViewState = {
    present: booleanFromValue(controller?.present, controllerRunning || showRunningSection),
    running: controllerRunning,
    lastError: stringFromValue(controller?.last_error),
  };
  const analyzerRunning = booleanFromValue(
    analyzer?.running ?? record?.analyzer_running,
    false,
  );
  const analyzerModelHot = booleanFromValue(
    analyzer?.model_hot ?? record?.analyzer_model_hot,
    false,
  );
  const framesAnalyzed = integerFromValue(
    analyzer?.frames_analyzed ?? record?.frames_analyzed,
    0,
  );
  const framesSkipped = integerFromValue(
    analyzer?.frames_skipped ?? record?.frames_skipped,
    0,
  );
  const framesComposited = integerFromValue(
    compositor?.frames_composited ?? record?.frames_composited,
    0,
  );
  const latencyMs = numberFromValue(
    analyzer?.last_latency_ms ?? record?.last_latency_ms,
    0,
  );
  const backendName = stringFromValue(
    analyzer?.backend_name ?? record?.analyzer_backend_name,
  );
  const analyzerAttachedPublished =
    analyzer !== null &&
    Object.prototype.hasOwnProperty.call(analyzer, "attached");
  const analyzerState: LiveAnalyzerViewState = {
    attached: booleanFromValue(
      analyzer?.attached,
      analyzerRunning || analyzerModelHot || framesAnalyzed > 0 || backendName.length > 0,
    ),
    modelHot: analyzerModelHot,
    running: analyzerRunning,
    framesAnalyzed,
    framesSkipped,
    lastLatencyMs: latencyMs,
    backendName,
    lastError: stringFromValue(analyzer?.last_error),
  };
  const previewHasExplicitFrameMetadata =
    previewPublishedLiveFrameId !== null ||
    compositorPublishedLastFrameId !== null ||
    previewPublishedFrameId > 0 ||
    (previewPublishedWidth > 0 && previewPublishedHeight > 0) ||
    (showStaticPreview && staticPreviewSource.length > 0);
  const previewHasFrame = booleanFromValue(
    preview?.has_frame ?? record?.preview_has_frame,
    showStaticPreview && previewHasExplicitFrameMetadata,
  );
  const previewLiveFrameId =
    previewPublishedLiveFrameId ??
    compositorPublishedLastFrameId;
  const previewWidth =
    previewPublishedWidth > 0
      ? previewPublishedWidth
      : previewHasFrame
        ? snapshot.source.capture_width
        : 0;
  const previewHeight =
    previewPublishedHeight > 0
      ? previewPublishedHeight
      : previewHasFrame
        ? snapshot.source.capture_height
        : 0;
  const previewFrameId =
    previewPublishedFrameId > 0
      ? previewPublishedFrameId
      : previewLiveFrameId?.sequence ?? 0;
  const previewInteropFailed = booleanFromValue(preview?.interop_failed, false);
  const previewPublicationCounters = isRecord(preview?.publication_counters)
    ? preview.publication_counters
    : null;
  const previewStatus: CapabilityStatusViewState["status"] =
    previewInteropFailed
      ? "fallback"
      : previewHasFrame
        ? showStaticPreview
          ? "ready"
          : "active"
        : "pending";
  const displayStartupFallback: LiveDisplayStartupState = previewHasFrame
    ? "surface published"
    : showRunningSection || runtime.starting
      ? "capture running"
      : "idle";
  const displayStartupState = liveDisplayStartupStateFromValue(
    preview?.display_startup_state,
    displayStartupFallback,
  );
  const previewSummary =
    previewHasFrame
      ? showStaticPreview && staticPreviewSource.length > 0
        ? `static preview ${previewWidth}x${previewHeight}`
        : `preview frame ${previewFrameId > 0 ? previewFrameId : previewLiveFrameId?.sequence ?? 0}`
      : "preview awaiting frame";
  const lastFailureCudaDeviceValue = preview?.last_failure_cuda_device;
  const previewState: LivePreviewViewState = {
    initialized: booleanFromValue(preview?.initialized, previewHasFrame),
    hasFrame: previewHasFrame,
    width: previewWidth,
    height: previewHeight,
    frameId: previewFrameId,
    liveFrameId: previewLiveFrameId,
    surfaceRevision: stringFromValue(preview?.surface_revision),
    owner: stringFromValue(preview?.owner, "none"),
    publishNs: integerFromValue(preview?.publish_ns, 0),
    displayStartupState,
    nativePresented: booleanFromValue(preview?.native_presented, false),
    rendererAcquiredRevision: stringFromValue(preview?.renderer_acquired_revision),
    rendererImportedRevision: stringFromValue(preview?.renderer_imported_revision),
    rendererSubmittedRevision: stringFromValue(preview?.renderer_submitted_revision),
    rendererDrawnRevision: stringFromValue(preview?.renderer_drawn_revision),
    rendererReleasePendingRevision: stringFromValue(
      preview?.renderer_release_pending_revision,
    ),
    rendererLastDrawError: stringFromValue(preview?.renderer_last_draw_error),
    rendererLastResultCode: stringFromValue(preview?.renderer_last_result_code),
    rendererLastResultDetail: stringFromValue(
      preview?.renderer_last_result_detail,
    ),
    fitToCapture: booleanFromValue(preview?.fit_to_capture, false),
    cropOverlayMode: booleanFromValue(preview?.crop_overlay_mode, false),
    staticSourceName: staticPreviewSource,
    interopFailed: previewInteropFailed,
    error: stringFromValue(preview?.error ?? record?.preview_error),
    lastError: stringFromValue(preview?.last_error),
    lastFailureReason: stringFromValue(preview?.last_failure_reason),
    lastFailureDetail: stringFromValue(preview?.last_failure_detail),
    lastFailureRevision: stringFromValue(preview?.last_failure_revision),
    lastFailureStage: stringFromValue(preview?.last_failure_stage),
    lastFailureLiveFrameId: liveFrameIdFromValue(
      preview?.last_failure_live_frame_id,
    ),
    lastFailureCudaDevice:
      lastFailureCudaDeviceValue === null ||
      lastFailureCudaDeviceValue === undefined
        ? null
        : integerFromValue(lastFailureCudaDeviceValue, -1),
    lastFailureResultCode: stringFromValue(preview?.last_failure_result_code),
    lastFailureResultDetail: stringFromValue(
      preview?.last_failure_result_detail,
    ),
    publicationCounters: {
      attemptedWorkspaceAcquisitions: integerFromValue(
        previewPublicationCounters?.attempted_workspace_acquisitions,
        0,
      ),
      startupWorkspaceAcquisitionMisses: integerFromValue(
        previewPublicationCounters?.startup_workspace_acquisition_misses,
        0,
      ),
      postStartupWorkspaceAcquisitionMisses: integerFromValue(
        previewPublicationCounters?.post_startup_workspace_acquisition_misses,
        0,
      ),
      retainedSurfaces: integerFromValue(
        previewPublicationCounters?.retained_surfaces,
        0,
      ),
      rejectedStaleFrames: integerFromValue(
        previewPublicationCounters?.rejected_stale_frames,
        0,
      ),
      cefExportFailures: integerFromValue(
        previewPublicationCounters?.cef_export_failures,
        0,
      ),
      rendererReleases: integerFromValue(
        previewPublicationCounters?.renderer_releases,
        0,
      ),
      nativeReleaseFailures: integerFromValue(
        previewPublicationCounters?.native_release_failures,
        0,
      ),
      rendererImportFailures: integerFromValue(
        previewPublicationCounters?.renderer_import_failures,
        0,
      ),
      rendererReleaseRejections: integerFromValue(
        previewPublicationCounters?.renderer_release_rejections,
        0,
      ),
    },
  };
  const manualOverlayRunning = booleanFromValue(manualOverlay?.running, false);
  const manualOverlayError = stringFromValue(manualOverlay?.last_error);
  const workspacePathLastFrameId =
    compositorPublishedLastFrameId ??
    previewLiveFrameId;
  const workspacePathState: LiveWorkspacePathViewState = {
    running: booleanFromValue(compositor?.running, false),
    framesPresented: framesComposited,
    framesDropped: integerFromValue(compositor?.frames_dropped, 0),
    lastFrameId: workspacePathLastFrameId,
    overlayActive: booleanFromValue(
      compositor?.analysis_overlay_active,
      false,
    ) || booleanFromValue(
      compositor?.manual_overlay_active,
      manualOverlayRunning,
    ),
    lastError:
      stringFromValue(compositor?.last_error) ||
      (manualOverlayError.length > 0
        ? "workspace overlay updates reported an error"
        : ""),
  };
  const errors = [
    stringFromValue(record?.start_error),
    stringFromValue(record?.action_error),
    previewState.error,
    previewState.rendererLastDrawError,
    controllerState.lastError,
    analyzerState.lastError,
    workspacePathState.lastError,
    stringFromValue(record?.last_error),
  ].filter((message) => message.length > 0);
  const previewLabelBase =
    previewHasFrame
      ? showStaticPreview && staticPreviewSource.length > 0
        ? `Static · ${staticPreviewSource} · ${previewWidth}x${previewHeight}`
        : previewFrameId > 0
          ? `Frame ${previewFrameId} · ${previewWidth}x${previewHeight}`
          : `${previewWidth}x${previewHeight}`
      : "No preview frame";
  const previewLabel = joinStatusSegments([
    previewLabelBase,
    displayStartupState !== "idle" ? displayStartupState : "",
    previewState.cropOverlayMode ? "full-frame" : "",
    !previewState.cropOverlayMode && previewState.fitToCapture ? "fit" : "",
    previewState.interopFailed ? "interop fallback" : "",
  ]);
  const controllerLabel =
    runtime.starting
      ? "starting"
      : runtime.stopping
        ? "stopping"
        : showRunningSection
          ? controllerRunning
            ? "running"
            : controllerPresentPublished && !controllerState.present
              ? "unavailable"
              : "stopped"
          : runtime.showIdleStartError
            ? "idle start failed"
            : previewHasFrame
              ? "idle with preview"
              : "idle";
  const analyzerLabel =
    showRunningSection
        ? joinStatusSegments([
          analyzerRunning ? "running" : "idle",
          `${framesAnalyzed} analyzed / ${framesSkipped} skipped`,
          analyzerModelHot ? "model hot" : "",
          analyzerAttachedPublished && !analyzerState.attached ? "detached" : "",
        ])
      : previewHasFrame
        ? "preview staged"
        : "waiting for live session";
  const workspacePathLabel =
    showRunningSection
      ? joinStatusSegments([
          `${framesComposited} presented`,
          workspacePathState.framesDropped > 0
            ? `${workspacePathState.framesDropped} dropped`
            : "",
          workspacePathState.overlayActive ? "overlay active" : "",
        ])
      : previewHasFrame || workspacePathState.lastFrameId !== null
        ? "workspace path staged"
        : "workspace path pending";
  const workspacePathStatus: CapabilityStatusViewState["status"] =
    workspacePathState.running
      ? "active"
      : framesComposited > 0 ||
          workspacePathState.lastFrameId !== null ||
          previewHasFrame
        ? "ready"
        : "pending";
  const workspacePathSummary =
    showRunningSection
      ? workspacePathState.framesDropped > 0
        ? `workspace path ${framesComposited} / ${workspacePathState.framesDropped} dropped`
        : `workspace path ${framesComposited}`
      : previewHasFrame || workspacePathState.lastFrameId !== null
        ? "workspace path staged"
        : "workspace path pending";
  const statusItems: CapabilityStatusViewState[] = [
    statusItemFromContract(record, "session", {
      key: "session",
      label: "Session",
      status:
        runtime.starting || runtime.stopping
          ? "pending"
          : showRunningSection
            ? controllerRunning
              ? "active"
              : controllerPresentPublished && !controllerState.present
                ? "blocked"
                : "ready"
            : runtime.showIdleStartError
              ? "blocked"
              : previewHasFrame
                ? "ready"
                : "pending",
      summary:
        runtime.starting
          ? "live session starting"
          : runtime.stopping
            ? "live session stopping"
            : showRunningSection
              ? controllerRunning
                ? "live session running"
                : controllerPresentPublished && !controllerState.present
                  ? "live controller unavailable"
                  : "live session stopped"
              : runtime.showIdleStartError
                ? "idle start failed"
                : previewHasFrame
                  ? "preview staged"
                  : "live session pending",
      detail:
        runtime.activeMode === "none"
          ? "No live runtime is currently active."
          : `Active live mode: ${runtime.activeMode}.`,
    }),
    statusItemFromContract(record, "preview", {
      key: "preview",
      label: "Preview",
      status: previewStatus,
      summary: previewSummary,
      detail: previewLabel,
    }),
    statusItemFromContract(record, "analyzer", {
      key: "analyzer",
      label: "Analyzer",
      status: analyzerState.running
        ? "active"
        : analyzerState.attached || analyzerModelHot || framesAnalyzed > 0
          ? "ready"
          : showRunningSection
            ? "pending"
            : "pending",
      summary:
        showRunningSection || framesAnalyzed > 0
          ? `analyzer ${framesAnalyzed} / ${framesSkipped}`
          : previewHasFrame
            ? "analyzer staged"
            : "analyzer waiting",
      detail: analyzerLabel,
    }),
    statusItemFromContract(record, "workspace_path", {
      key: "workspace_path",
      label: "Workspace Path",
      status: workspacePathStatus,
      summary: workspacePathSummary,
      detail: workspacePathLabel,
    }),
  ];
  return {
    hasSnapshotState: record !== null,
    showRunningSection,
    showStaticPreview,
    previewHasFrame,
    controllerLabel,
    previewLabel,
    analyzerLabel,
    workspacePathLabel,
    backendLabel: backendName.length > 0 ? backendName : "pending",
    latencyLabel: latencyMs > 0 ? `${latencyMs.toFixed(2)} ms` : "-",
    errorText: errors.join(" | "),
    statusItems,
    runtime,
    preview: previewState,
    controller: controllerState,
    analyzer: analyzerState,
    workspacePath: workspacePathState,
  };
}

export function isLivePredictPrimaryActionActive(
  status: LiveStatusViewState,
): boolean {
  return (
    status.runtime.activeMode === "predict" &&
    (status.runtime.starting ||
      status.runtime.stopping ||
      status.controller.running ||
      status.showRunningSection)
  );
}

export function annotateShellControlsState(
  snapshot: StateSnapshot,
): AnnotateShellControlsViewState {
  const controls = annotateControlsRecord(snapshot);
  const setupFrame = firstRecord([
    controls?.setup_frame_navigation,
    controls?.frame_navigation,
    controls?.setup_frame,
  ]);
  const liveAnnotate = firstRecord([
    controls?.live_annotate,
    controls?.live_controls,
  ]);
  const save = firstRecord([
    controls?.save,
    controls?.save_controls,
  ]);
  const brush = firstRecord([
    controls?.brush,
    controls?.brush_radius,
  ]);
  const sidebar = annotateSidebarState(snapshot);
  const annotateSource = workflowSourceRecord(snapshot, "annotate");
  const sourceKind = sourceKindFromValue(
    annotateSource?.kind ?? snapshot.source.kind,
  );
  const liveVideo = sourceKind === "video_stream";
  const liveStatus = liveStatusState(snapshot);
  const activeTool = normalizeAnnotateToolId(
    workflowSectionRecord(snapshot, "annotate", "annotate")?.active_tool,
    "select",
  );
  const brushVisibleFallback =
    (activeTool === "mask.paint" || activeTool === "mask.erase") &&
    (sidebar?.canEditSelectedMask ?? false);

  const frameInputCount = Math.max(
    0,
    integerFromValue(
      setupFrame?.input_count ?? setupFrame?.frame_count,
      sourceKind === "image_folder" || sourceKind === "single_image" ? 1 : 0,
    ),
  );
  const frameCurrentIndex = Math.max(
    0,
    integerFromValue(
      setupFrame?.current_index ?? setupFrame?.frame_index,
      0,
    ),
  );
  const frameVisible = booleanFromValue(setupFrame?.visible, !liveVideo);
  const frameSummary =
    stringFromValue(setupFrame?.summary) ||
    (liveVideo
      ? "Setup-frame navigation is unavailable while video capture is selected."
      : frameInputCount > 0
        ? `Frame ${Math.min(frameCurrentIndex + 1, frameInputCount)} of ${frameInputCount}`
        : "No setup-frame metadata has been published yet.");

  const liveRunningFallback =
    liveStatus.runtime.activeMode === "annotate" &&
    (liveStatus.controller.running || liveStatus.showRunningSection);
  const liveVisible = booleanFromValue(liveAnnotate?.visible, liveVideo);
  const liveRunning = booleanFromValue(liveAnnotate?.running, liveRunningFallback);
  const liveSummary =
    stringFromValue(liveAnnotate?.summary) ||
    (liveVisible
      ? liveRunning
        ? "Live annotate is streaming from the current capture device."
        : "Start live annotate to keep the workspace bound to incoming frames."
      : "Live annotate is only available for video-stream sources.");

  const holdSaveRecord = actionRecord(save, "hold_save");
  const saveNowEnabled = booleanFromValue(
    actionRecord(save, "save_now")?.enabled ?? save?.save_now_enabled,
    (sidebar?.hasAnnotationFrame ?? false) && !liveStatus.runtime.starting,
  );
  const holdSaveBlocked = booleanFromValue(
    holdSaveRecord?.blocked ?? save?.hold_save_blocked,
    false,
  );
  const holdSaveActive = booleanFromValue(
    holdSaveRecord?.value ?? holdSaveRecord?.active ?? save?.hold_save_active,
    false,
  );
  const holdSaveEnabled = booleanFromValue(
    holdSaveRecord?.enabled ?? save?.hold_save_enabled,
    liveRunning && (sidebar?.hasAnnotationFrame ?? false),
  );
  const saveSummary =
    stringFromValue(save?.summary) ||
    (holdSaveBlocked
      ? "Hold Save is blocked until the current queued frame drains."
      : holdSaveActive
        ? "Hold Save is armed while the control stays pressed."
        : holdSaveEnabled
          ? "Hold Save captures frames continuously while live annotate is running."
          : saveNowEnabled
            ? "Save Now persists the current annotation snapshot immediately."
            : "Save controls enable after a writable frame is available.");

  const brushSummary =
    stringFromValue(brush?.summary) ||
    (brushVisibleFallback
      ? "Brush radius stays in capture pixels and feeds the workspace brush intent."
      : "Brush radius appears when Paint or Erase is active on a mask-capable selection.");

  const brushRadius = intentNumericState(
    actionRecord(brush, "radius") ?? brush?.radius ?? brush,
    {
      value: kDefaultAnnotateBrushRadius,
      min: kDefaultAnnotateBrushRadiusMin,
      max: kDefaultAnnotateBrushRadiusMax,
      enabled: booleanFromValue(brush?.enabled, brushVisibleFallback),
      workflow: workflowFromValue(brush?.workflow, "annotate"),
      intent: stringFromValue(brush?.intent ?? brush?.intent_name, "annotate.brush_radius"),
      label: "Brush Radius",
      payloadKey: stringFromValue(brush?.payload_key, "radius"),
    },
  );
  const brushVisible = booleanFromValue(brush?.visible, brushVisibleFallback);
  const statusItems: CapabilityStatusViewState[] = [
    statusItemFromContract(controls, "setup_frame", {
      key: "setup_frame",
      label: "Setup Frame",
      status:
        frameVisible && frameInputCount > 0
          ? "ready"
          : liveVideo
            ? "blocked"
            : "pending",
      summary:
        frameVisible && frameInputCount > 0
          ? `setup frame ${Math.min(frameCurrentIndex + 1, frameInputCount)}/${frameInputCount}`
          : liveVideo
            ? "setup frame navigation unavailable"
            : "setup frame metadata pending",
      detail: frameSummary,
    }),
    statusItemFromContract(controls, "live_annotate", {
      key: "live_annotate",
      label: "Live Annotate",
      status: liveRunning ? "active" : liveVisible ? "ready" : "blocked",
      summary: liveRunning
        ? "live annotate running"
        : liveVisible
          ? "live annotate ready"
          : "live annotate unavailable",
      detail: liveSummary,
    }),
    statusItemFromContract(controls, "save", {
      key: "save",
      label: "Save",
      status: holdSaveBlocked
        ? "blocked"
        : holdSaveActive
          ? "active"
          : holdSaveEnabled || saveNowEnabled
            ? "ready"
            : "pending",
      summary: holdSaveBlocked
        ? "hold save blocked"
        : holdSaveActive
          ? "hold save armed"
          : holdSaveEnabled
            ? "hold save ready"
            : saveNowEnabled
              ? "save now ready"
              : "save controls waiting",
      detail: saveSummary,
    }),
    statusItemFromContract(controls, "brush", {
      key: "brush",
      label: "Brush",
      status: brushVisible && brushRadius.enabled
        ? activeTool === "mask.paint" || activeTool === "mask.erase"
          ? "active"
          : "ready"
        : "blocked",
      summary:
        brushVisible && brushRadius.enabled
          ? `brush radius ${brushRadius.value}px`
          : "brush controls unavailable",
      detail: brushSummary,
    }),
  ];

  return {
    statusItems,
    setupFrame: {
      visible: frameVisible,
      currentIndex: frameCurrentIndex,
      inputCount: frameInputCount,
      summary: frameSummary,
      reload: intentActionState(actionRecord(setupFrame, "reload"), {
        enabled: booleanFromValue(
          setupFrame?.reload_enabled,
          frameVisible && frameInputCount > 0,
        ),
        workflow: workflowFromValue(setupFrame?.reload_workflow, "annotate"),
        intent: stringFromValue(setupFrame?.reload_intent, "annotate.setup_frame.reload"),
        label: "Reload",
      }),
      previous: intentActionState(actionRecord(setupFrame, "prev"), {
        enabled: booleanFromValue(
          setupFrame?.prev_enabled,
          frameVisible && frameCurrentIndex > 0,
        ),
        workflow: workflowFromValue(setupFrame?.prev_workflow, "annotate"),
        intent: stringFromValue(setupFrame?.prev_intent, "annotate.setup_frame.prev"),
        label: "Prev",
      }),
      next: intentActionState(actionRecord(setupFrame, "next"), {
        enabled: booleanFromValue(
          setupFrame?.next_enabled,
          frameVisible &&
            frameInputCount > 0 &&
            frameCurrentIndex + 1 < frameInputCount,
        ),
        workflow: workflowFromValue(setupFrame?.next_workflow, "annotate"),
        intent: stringFromValue(setupFrame?.next_intent, "annotate.setup_frame.next"),
        label: "Next",
      }),
    },
    liveAnnotate: {
      visible: liveVisible,
      running: liveRunning,
      summary: liveSummary,
      start: intentActionState(actionRecord(liveAnnotate, "start"), {
        enabled: booleanFromValue(
          liveAnnotate?.start_enabled,
          liveVisible && !liveRunning,
        ),
        workflow: workflowFromValue(liveAnnotate?.start_workflow, "annotate"),
        intent: stringFromValue(liveAnnotate?.start_intent, "annotate.live.start"),
        label: "Start Live Annotate",
      }),
      stop: intentActionState(actionRecord(liveAnnotate, "stop"), {
        enabled: booleanFromValue(
          liveAnnotate?.stop_enabled,
          liveVisible && liveRunning,
        ),
        workflow: workflowFromValue(liveAnnotate?.stop_workflow, "annotate"),
        intent: stringFromValue(liveAnnotate?.stop_intent, "annotate.live.stop"),
        label: "Stop Live Annotate",
      }),
    },
    save: {
      visible: booleanFromValue(save?.visible, true),
      summary: saveSummary,
      saveNow: intentActionState(actionRecord(save, "save_now"), {
        enabled: saveNowEnabled,
        workflow: workflowFromValue(save?.save_now_workflow, "annotate"),
        intent: stringFromValue(save?.save_now_intent, "annotate.save_now"),
        label: "Save Now",
      }),
      holdSave: intentToggleState(holdSaveRecord ?? save?.hold_save, {
        value: holdSaveActive,
        enabled: holdSaveEnabled && !holdSaveBlocked,
        workflow: workflowFromValue(save?.hold_save_workflow, "annotate"),
        intent: stringFromValue(save?.hold_save_intent, "annotate.hold_save"),
        label: "Hold Save",
        payloadKey: stringFromValue(save?.hold_save_payload_key, "enabled"),
      }),
      holdSaveBlocked,
    },
    brush: {
      visible: brushVisible,
      summary: brushSummary,
      radius: brushRadius,
    },
  };
}

export function livePreviewControlsState(
  snapshot: StateSnapshot,
): LivePreviewControlsViewState {
  const controls = livePreviewControlsRecord(snapshot);
  const liveStatus = liveStatusState(snapshot);
  const fitValue = actionRecord(controls, "fit_to_capture") ?? controls?.fit_to_capture;
  const fullFrameValue =
    actionRecord(controls, "full_frame_display") ??
    actionRecord(controls, "full_frame") ??
    actionRecord(controls, "crop_overlay_mode") ??
    controls?.full_frame_display ??
    controls?.full_frame ??
    controls?.crop_overlay_mode;
  const fitToCapture = intentToggleState(fitValue, {
    value: liveStatus.preview.fitToCapture,
    enabled: booleanFromValue(
      controls?.fit_to_capture_enabled,
      liveStatus.preview.initialized ||
        liveStatus.preview.hasFrame ||
        liveStatus.showStaticPreview,
    ),
    workflow: workflowFromValue(controls?.fit_to_capture_workflow, "live"),
    intent: stringFromValue(
      controls?.fit_to_capture_intent,
      "live.preview.fit_to_capture",
    ),
    label: "Fit To Capture",
    payloadKey: stringFromValue(controls?.fit_to_capture_payload_key, "enabled"),
  });
  const fullFrameDisplay = intentToggleState(fullFrameValue, {
    value: liveStatus.preview.cropOverlayMode,
    enabled: booleanFromValue(
      controls?.full_frame_display_enabled ??
        controls?.full_frame_enabled,
      liveStatus.preview.initialized || liveStatus.preview.hasFrame,
    ),
    workflow: workflowFromValue(
      controls?.full_frame_display_workflow ?? controls?.full_frame_workflow,
      "live",
    ),
    intent: stringFromValue(
      controls?.full_frame_display_intent ?? controls?.full_frame_intent,
      "live.preview.full_frame_display",
    ),
    label: "Full Frame",
    payloadKey: stringFromValue(
      controls?.full_frame_display_payload_key ??
        controls?.full_frame_payload_key,
      "enabled",
    ),
  });
  const statusItems: CapabilityStatusViewState[] = [
    statusItemFromContract(controls, "display", {
      key: "display",
      label: "Display",
      status:
        fitToCapture.enabled || fullFrameDisplay.enabled
          ? "ready"
          : liveStatus.preview.hasFrame || liveStatus.showStaticPreview
            ? "blocked"
            : "pending",
      summary:
        fitToCapture.enabled || fullFrameDisplay.enabled
          ? "preview display controls ready"
          : liveStatus.preview.hasFrame || liveStatus.showStaticPreview
            ? "preview display controls unavailable"
            : "preview display controls pending",
      detail: liveStatus.previewLabel,
    }),
    statusItemFromContract(controls, "fit_to_capture", {
      key: "fit_to_capture",
      label: "Fit To Capture",
      status: fitToCapture.value
        ? "active"
        : fitToCapture.enabled
          ? "ready"
          : "blocked",
      summary: fitToCapture.value
        ? "fit to capture enabled"
        : fitToCapture.enabled
          ? "fit to capture ready"
          : "fit to capture unavailable",
      detail: fitToCapture.label,
    }),
    statusItemFromContract(controls, "full_frame_display", {
      key: "full_frame_display",
      label: "Full Frame",
      status: fullFrameDisplay.value
        ? "active"
        : fullFrameDisplay.enabled
          ? "ready"
          : "blocked",
      summary: fullFrameDisplay.value
        ? "full-frame preview enabled"
        : fullFrameDisplay.enabled
          ? "full-frame preview ready"
          : "full-frame preview unavailable",
      detail: fullFrameDisplay.label,
    }),
  ];

  return {
    statusItems,
    fitToCapture,
    fullFrameDisplay,
  };
}

function annotationWorkspacePointFromValue(
  value: unknown,
): AnnotationWorkspacePoint | null {
  if (!isRecord(value)) {
    return null;
  }
  return {
    x: numberFromValue(value.x, 0),
    y: numberFromValue(value.y, 0),
  };
}

function annotationWorkspaceBoxFromValue(
  value: unknown,
): AnnotationWorkspaceBox | null {
  if (!isRecord(value)) {
    return null;
  }
  return {
    x1: numberFromValue(value.x1, 0),
    y1: numberFromValue(value.y1, 0),
    x2: numberFromValue(value.x2, 0),
    y2: numberFromValue(value.y2, 0),
  };
}

function annotationWorkspaceEdgeFromValue(
  value: unknown,
): AnnotationWorkspaceEdge | null {
  if (!isRecord(value)) {
    return null;
  }
  return {
    sourceIndex: integerFromValue(value.source_index ?? value.sourceIndex, 0),
    targetIndex: integerFromValue(value.target_index ?? value.targetIndex, 0),
  };
}

function normalizeAnnotationWorkspaceShapeType(
  value: unknown,
): AnnotationWorkspaceShapeType {
  switch (stringFromValue(value, "box")) {
    case "mask":
      return "mask";
    case "spline":
      return "spline";
    case "point":
      return "point";
    case "skeleton":
      return "skeleton";
    default:
      return "box";
  }
}

function normalizeAnnotationWorkspaceHandleRole(
  value: unknown,
): AnnotationWorkspaceHandleRole {
  switch (stringFromValue(value, "point")) {
    case "spline_knot":
      return "spline_knot";
    case "spline_in_handle":
      return "spline_in_handle";
    case "spline_out_handle":
      return "spline_out_handle";
    case "skeleton_node":
      return "skeleton_node";
    default:
      return "point";
  }
}

function annotationWorkspaceObjectGeometryStateFromValue(
  value: unknown,
): AnnotationWorkspaceObjectGeometryState {
  if (!isRecord(value)) {
    return {
      capturePoints: [],
      edges: [],
      closed: false,
    };
  }
  return {
    capturePoints: Array.isArray(value.capture_points)
      ? value.capture_points
          .map((entry) => annotationWorkspacePointFromValue(entry))
          .filter((entry): entry is AnnotationWorkspacePoint => entry !== null)
      : [],
    edges: Array.isArray(value.edges)
      ? value.edges
          .map((entry) => annotationWorkspaceEdgeFromValue(entry))
          .filter((entry): entry is AnnotationWorkspaceEdge => entry !== null)
      : [],
    closed: booleanFromValue(value.closed, false),
  };
}

function annotationWorkspaceObjectStateFromValue(
  value: unknown,
): AnnotationWorkspaceObjectState | null {
  if (!isRecord(value)) {
    return null;
  }
  const captureBox = annotationWorkspaceBoxFromValue(value.capture_box);
  if (captureBox === null) {
    return null;
  }
  return {
    index: integerFromValue(value.index, 0),
    categoryIndex: integerFromValue(value.category_index, 0),
    shapeType: normalizeAnnotationWorkspaceShapeType(value.shape_type),
    captureBox,
    fullyVisible: booleanFromValue(value.fully_visible, false),
    geometry: annotationWorkspaceObjectGeometryStateFromValue(value.geometry),
  };
}

function annotationWorkspaceEditableHandleStateFromValue(
  value: unknown,
): AnnotationWorkspaceEditableHandleState | null {
  if (!isRecord(value)) {
    return null;
  }
  const capturePoint = annotationWorkspacePointFromValue(value.capture_point);
  if (capturePoint === null) {
    return null;
  }
  return {
    objectIndex: integerFromValue(value.object_index, 0),
    elementIndex: integerFromValue(value.element_index, 0),
    role: normalizeAnnotationWorkspaceHandleRole(value.role),
    categoryIndex: integerFromValue(value.category_index, 0),
    capturePoint,
    tetherCapturePoint: annotationWorkspacePointFromValue(
      value.tether_capture_point,
    ),
    materialized: booleanFromValue(value.materialized, true),
  };
}

function annotationWorkspaceBrushPreviewRecord(
  scene: JsonRecord,
): JsonRecord | null {
  const nested = firstRecord([
    scene.brush_preview,
    scene.brushPreview,
    scene.annotation_brush_preview,
    scene.annotate_brush_preview,
  ]);
  if (nested !== null) {
    return nested;
  }

  const flat: JsonRecord = {};
  if ("brush_preview_visible" in scene) {
    flat.visible = scene.brush_preview_visible;
  }
  if ("brush_preview_capture_x" in scene) {
    flat.capture_x = scene.brush_preview_capture_x;
  }
  if ("brush_preview_capture_y" in scene) {
    flat.capture_y = scene.brush_preview_capture_y;
  }
  if ("brush_preview_radius" in scene) {
    flat.radius = scene.brush_preview_radius;
  }
  if ("brush_preview_erase" in scene) {
    flat.erase = scene.brush_preview_erase;
  }
  if ("brush_preview_mode" in scene) {
    flat.mode = scene.brush_preview_mode;
  }
  return Object.keys(flat).length > 0 ? flat : null;
}

function annotationWorkspaceBrushPreviewStateFromValue(
  value: unknown,
): AnnotationWorkspaceBrushPreviewState | null {
  if (!isRecord(value)) {
    return null;
  }
  const mode = stringFromValue(value.mode).trim().toLowerCase();
  return {
    visible: booleanFromValue(value.visible, false),
    capturePoint:
      annotationWorkspacePointFromValue(value.capture_point) ?? {
        x: integerFromValue(value.capture_x, integerFromValue(value.x, 0)),
        y: integerFromValue(value.capture_y, integerFromValue(value.y, 0)),
      },
    radius: Math.max(
      1,
      integerFromValue(value.radius, kDefaultAnnotateBrushRadius),
    ),
    erase: booleanFromValue(value.erase, mode === "erase"),
  };
}

function annotationWorkspaceDenseMaskStateFromValue(
  value: unknown,
): AnnotationWorkspaceDenseMaskState | null {
  if (!isRecord(value)) {
    return null;
  }
  const captureBox = annotationWorkspaceBoxFromValue(value.capture_box);
  const maskRle = stringFromValue(value.mask_rle).trim();
  if (captureBox === null || maskRle.length === 0) {
    return null;
  }
  return {
    objectIndex: integerFromValue(value.object_index, 0),
    categoryIndex: integerFromValue(value.category_index, 0),
    captureBox,
    maskRle,
  };
}

function annotateWorkspaceSceneRecord(snapshot: StateSnapshot): JsonRecord | null {
  const root = workflowStateRoot(snapshot);
  return firstRecord([
    root?.annotate_workspace_scene,
    root?.annotation_workspace_scene,
    workflowSectionRecord(snapshot, "annotate", "workspace_scene"),
    workflowSectionRecord(snapshot, "annotate", "annotate_workspace_scene"),
  ]);
}

export function annotateWorkspaceSceneState(
  snapshot: StateSnapshot,
): AnnotationWorkspaceSceneState | null {
  const record = annotateWorkspaceSceneRecord(snapshot);
  if (record === null) {
    return null;
  }
  const visibleDenseMasksValue =
    record.visible_dense_masks ?? record.visibleDenseMasks;
  const visibleDenseMasksSource: unknown[] = Array.isArray(visibleDenseMasksValue)
    ? visibleDenseMasksValue
    : [];

  return {
    documentGeneration: integerFromValue(record.document_generation, 0),
    selectedObjectIndex:
      record.selected_object_index === null || record.selected_object_index === undefined
        ? null
        : integerFromValue(record.selected_object_index, 0),
    captureWidth: Math.max(
      1,
      integerFromValue(
        record.capture_width,
        Math.max(snapshot.annotation.capture_width, snapshot.source.capture_width),
      ),
    ),
    captureHeight: Math.max(
      1,
      integerFromValue(
        record.capture_height,
        Math.max(snapshot.annotation.capture_height, snapshot.source.capture_height),
      ),
    ),
    visibleObjects: Array.isArray(record.visible_objects)
      ? record.visible_objects
          .map((entry) => annotationWorkspaceObjectStateFromValue(entry))
          .filter((entry): entry is AnnotationWorkspaceObjectState => entry !== null)
      : [],
    editableHandles: Array.isArray(record.editable_handles)
      ? record.editable_handles
          .map((entry) => annotationWorkspaceEditableHandleStateFromValue(entry))
          .filter(
            (entry): entry is AnnotationWorkspaceEditableHandleState =>
              entry !== null,
          )
      : [],
    brushPreview: annotationWorkspaceBrushPreviewStateFromValue(
      annotationWorkspaceBrushPreviewRecord(record),
    ),
    visibleDenseMasks: visibleDenseMasksSource
      .map((entry) => annotationWorkspaceDenseMaskStateFromValue(entry))
      .filter(
        (entry): entry is AnnotationWorkspaceDenseMaskState => entry !== null,
      ),
    selectedDenseMask: annotationWorkspaceDenseMaskStateFromValue(
      record.selected_dense_mask ?? record.selectedDenseMask,
    ),
  };
}

export function normalizeAnnotateToolId(
  value: unknown,
  fallback: AnnotateToolId = "select",
): AnnotateToolId {
  switch (stringFromValue(value, fallback)) {
    case "select":
      return "select";
    case "direct":
      return "direct";
    case "box":
      return "box";
    case "paint":
    case "mask_paint":
    case "mask.paint":
      return "mask.paint";
    case "erase":
    case "mask_erase":
    case "mask.erase":
      return "mask.erase";
    case "fill":
    case "mask_fill":
    case "mask.fill":
      return "mask.fill";
    case "point":
      return "point";
    case "spline":
      return "spline";
    case "skeleton":
      return "skeleton";
    default:
      return fallback;
  }
}

export function annotateToolPaletteState(
  snapshot: StateSnapshot,
  fallbackTool?: AnnotateToolId | null,
): AnnotateToolPaletteState {
  const annotateLeaf = workflowSectionRecord(snapshot, "annotate", "annotate");
  const sidebar = annotateSidebarState(snapshot);
  const activeTool = normalizeAnnotateToolId(
    annotateLeaf?.active_tool,
    fallbackTool ?? "select",
  );
  const canCreateObjects = sidebar?.canCreateObjects ?? true;
  const hasSelectedObject = sidebar?.hasSelectedObject ?? false;
  const canEditMask = sidebar?.canEditSelectedMask ?? false;
  const canRedrawBox = sidebar?.canRedrawSelectedBox ?? false;
  const options = kAnnotateToolSpecs.map((tool) => {
    const disabled =
      tool.id === "direct"
        ? !hasSelectedObject
        : tool.id === "box"
          ? !(canCreateObjects || canRedrawBox)
          : tool.id === "mask.paint" ||
              tool.id === "mask.erase" ||
              tool.id === "mask.fill"
            ? !canEditMask
            : tool.id === "point" ||
                tool.id === "spline" ||
                tool.id === "skeleton"
              ? !canCreateObjects
              : false;
    return {
      ...tool,
      active: tool.id === activeTool,
      disabled,
    };
  });
  const statusItems: CapabilityStatusViewState[] = [
    statusItemFromContract(annotateLeaf, "mask_tools", {
      key: "mask_tools",
      label: "Mask Tools",
      status: canEditMask
        ? activeTool === "mask.paint" ||
          activeTool === "mask.erase" ||
          activeTool === "mask.fill"
          ? "active"
          : "ready"
        : "blocked",
      summary: canEditMask
        ? activeTool === "mask.paint" ||
          activeTool === "mask.erase" ||
          activeTool === "mask.fill"
          ? "mask tools active"
          : "mask tools ready"
        : "mask tools blocked until selection",
      detail: canEditMask
        ? "The selected object supports mask editing."
        : "Select a mask-capable object to enable Paint, Erase, and Fill.",
    }),
    statusItemFromContract(annotateLeaf, "create_tools", {
      key: "create_tools",
      label: "Create Tools",
      status: canCreateObjects ? "ready" : "blocked",
      summary: canCreateObjects
        ? "create tools ready"
        : "create tools unavailable",
      detail: canCreateObjects
        ? "Point, spline, skeleton, and box creation remain available."
        : "Create-object tools are disabled until categories and source state allow new annotations.",
    }),
  ];

  return {
    activeTool,
    options,
    statusItems,
  };
}
