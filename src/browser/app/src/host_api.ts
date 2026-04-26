export type Workflow =
  | "train"
  | "validate"
  | "predict"
  | "annotate"
  | "export"
  | "live";

export type SourceKind =
  | "compiled_dataset"
  | "single_image"
  | "image_folder"
  | "video_stream";

export type BrowserHostBackend = "unknown" | "cef";

export type BrowserRuntimeCapabilityStatus =
  | "unknown"
  | "available"
  | "unavailable";

export type FileDialogMode = "open_file" | "open_folder" | "save_file";

export interface CaptureRegion {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface WorkspaceGeometry {
  canvasWidth: number;
  canvasHeight: number;
  fitScale: number;
  scale: number;
  frameX: number;
  frameY: number;
  frameWidth: number;
  frameHeight: number;
}

export interface AnnotateSidebarCategoryState {
  id: number;
  name: string;
}

export interface AnnotateSidebarObjectState {
  index: number;
  label: string;
  selected: boolean;
}

export interface AnnotateColorToleranceState {
  hueMinusPct: number;
  huePlusPct: number;
  saturationMinusPct: number;
  saturationPlusPct: number;
  valueMinusPct: number;
  valuePlusPct: number;
}

export interface AnnotateColorRangeState {
  center: {
    hueDegrees: number;
    saturation: number;
    value: number;
  };
  tolerance: AnnotateColorToleranceState;
  sampling: boolean;
}

export interface AnnotateSplineState {
  knotCount: number;
  segmentCount: number;
  closed: boolean;
  activeKnotIndex: number | null;
  activeSegmentIndex: number | null;
  activeHandleMode: "corner" | "smooth" | "mirrored" | null;
  closeIntent: boolean;
  reopenRequested: boolean;
  canClose: boolean;
  canReopen: boolean;
  canInsertActiveSegmentKnot: boolean;
  canDeleteActiveKnot: boolean;
  canEditActiveHandleMode: boolean;
}

export interface AnnotateSkeletonJointState {
  index: number;
  label: string;
  visible: boolean;
  active: boolean;
}

export interface AnnotateSkeletonState {
  visibleJointCount: number;
  totalJointCount: number;
  activeJointIndex: number | null;
  activeJointKey: string | null;
  nextJointIndex: number | null;
  nextJointKey: string | null;
  activeJointVisible: boolean;
  reseedRequested: boolean;
  canSkipJoint: boolean;
  canHideActiveJoint: boolean;
  canReactivateActiveJoint: boolean;
  canReseedActiveJoint: boolean;
  joints: AnnotateSkeletonJointState[];
}

export interface AnnotateSidebarSelectedObjectState {
  enabled: boolean;
  categoryIndex: number;
  shapeLabel: string;
  displayBox: { x1: number; y1: number; x2: number; y2: number } | null;
  supportsMaskEditing: boolean;
  sup: AnnotateColorRangeState;
  nosup: AnnotateColorRangeState;
  point: { x: number; y: number } | null;
  spline: AnnotateSplineState | null;
  skeleton: AnnotateSkeletonState | null;
}

export interface AnnotateSidebarState {
  hasAnnotationFrame: boolean;
  cleanupRadius: number;
  assistAvailable: boolean;
  assistRunning: boolean;
  assistSummary: string;
  assistError: string;
  preferredNewObjectCategoryIndex: number;
  hasClasses: boolean;
  canUndo: boolean;
  canRedo: boolean;
  canCreateObjects: boolean;
  canDeleteSelected: boolean;
  canRedrawSelectedBox: boolean;
  canEditSelectedMask: boolean;
  hasSelectedObject: boolean;
  selectedObjectIndex: number | null;
  categories: AnnotateSidebarCategoryState[];
  objects: AnnotateSidebarObjectState[];
  selectedObject: AnnotateSidebarSelectedObjectState | null;
}

export interface JobLogEntry {
  sequence: number;
  level: string;
  message: string;
}

export interface JobState {
  running: boolean;
  label: string;
  summary: string;
  error: string;
  output_tail: string;
  recent_logs: JobLogEntry[];
}

export interface SourceMetadata {
  kind: SourceKind;
  locator: string;
  recursive: boolean;
  device_index: number;
  capture_width: number;
  capture_height: number;
  capture_fps: number;
  v4l2_buffer_count: number;
  has_crop: boolean;
  crop: CaptureRegion;
}

export interface AnnotationDocumentState {
  document_generation: number;
  session_revision: number;
  capture_width: number;
  capture_height: number;
  instance_count: number;
  selected_instance: number | null;
}

export interface WorkspaceSurfaceInfo {
  surfaceId: "workspace";
  revision: string;
  width: number;
  height: number;
  textureFormat: "rgba8unorm";
  opaque: true;
  upright: true;
}

export interface LiveFrameId {
  session_nonce: number;
  sequence: number;
}

export interface BrowserRuntimeCapabilities {
  host_backend: BrowserHostBackend;
  navigator_gpu: BrowserRuntimeCapabilityStatus;
  workspace_surface_bridge: BrowserRuntimeCapabilityStatus;
  workspace_surface_zero_copy: BrowserRuntimeCapabilityStatus;
}

export interface StateSnapshot {
  type: "state.snapshot";
  protocol_version: number;
  state_revision: number;
  active_workflow: Workflow;
  workflow_state: Record<string, unknown>;
  settings_state: Record<string, unknown>;
  job: JobState;
  source: SourceMetadata;
  annotation: AnnotationDocumentState;
  runtime_capabilities: BrowserRuntimeCapabilities;
  workspace_surface?: WorkspaceSurfaceInfo | null;
}

export interface IntentMessage {
  type: "intent";
  protocol_version: number;
  request_id: number;
  workflow: Workflow;
  intent: string;
  payload: Record<string, unknown>;
}

export type BrowserHostCapabilityStatus =
  | "active"
  | "ready"
  | "fallback"
  | "blocked"
  | "pending";

export interface BrowserHostCapabilityContract {
  status?:
    | BrowserHostCapabilityStatus
    | "available"
    | "degraded"
    | "warning"
    | "error"
    | "waiting"
    | "loading"
    | "unavailable"
    | "disabled";
  label?: string;
  summary?: string;
  detail?: string;
  active?: boolean;
  ready?: boolean;
  pending?: boolean;
  blocked?: boolean;
  available?: boolean;
  enabled?: boolean;
  supported?: boolean;
  fallback?: boolean;
  degraded?: boolean;
}

export type BrowserHostCapabilityContractMap = Record<
  string,
  BrowserHostCapabilityContract | unknown
>;

export interface BrowserHostBridgeState {
  phase: "idle" | "polling" | "dispatch";
  connected: boolean;
  lastError: string;
  lastSuccessRevision: number | null;
  runtimeCapabilities?: BrowserRuntimeCapabilities;
  capabilities?: BrowserHostCapabilityContractMap;
}

export interface BrowserHostTransport {
  readonly mode: "native" | "websocket" | "mock";
  getSnapshot(): StateSnapshot;
  dispatch(intent: IntentMessage): void;
  subscribe(listener: (snapshot: StateSnapshot) => void): () => void;
  getBridgeState?(): BrowserHostBridgeState;
  subscribeBridgeState?(
    listener: (state: BrowserHostBridgeState) => void,
  ): () => void;
}

export interface BrowserHostWorkspaceGpuBridge {
  acquireCurrentSurface(surfaceId: string): WorkspaceSurfaceInfo | null;
  importTexture(
    device: GPUDevice,
    surfaceId: string,
    revision: string,
  ): GPUTexture;
  releaseSurface(surfaceId: string, revision: string): void;
}

export interface BrowserHostControlBridge {
  postMessage(messageText: string): void;
  deliverSnapshot?(jsonText: string): void;
  setBridgeState?(jsonText: string): void;
  reportError?(messageText: string): void;
}

export type BrowserHostNativeBridge = BrowserHostControlBridge;

export interface BrowserHostBridgeErrorMessage {
  mode?: "native" | "websocket";
  type: "bridge.error";
  request_id: number | null;
  message: string;
}

export interface BrowserHostSurfaceReadyMessage {
  type: "surface.ready";
  surface: WorkspaceSurfaceInfo;
}

declare global {
  var __MMLTK_NATIVE_BRIDGE__: BrowserHostNativeBridge | undefined;
  var __MMLTK_BROWSER_WS_URL__: string | undefined;
  var __MMLTK_WORKSPACE_GPU_BRIDGE__:
    | BrowserHostWorkspaceGpuBridge
    | undefined;

  interface Window {
    __MMLTK_NATIVE_BRIDGE__?: BrowserHostNativeBridge;
    __MMLTK_BROWSER_WS_URL__?: string;
    __MMLTK_WORKSPACE_GPU_BRIDGE__?: BrowserHostWorkspaceGpuBridge;
  }
}
