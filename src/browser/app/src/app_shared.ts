import type {
  CaptureRegion,
  AnnotateColorRangeState,
  AnnotateColorToleranceState,
  AnnotateSidebarCategoryState,
  AnnotateSidebarObjectState,
  AnnotateSidebarSelectedObjectState,
  AnnotateSidebarState,
  AnnotateSkeletonJointState,
  AnnotateSkeletonState,
  AnnotateSplineState,
  BrowserHostCapabilityStatus,
  JobState,
  SourceKind,
  StateSnapshot,
  Workflow,
  WorkspaceGeometry,
  WorkspaceSurfaceInfo,
} from "./host_api";

type JsonRecord = Record<string, unknown>;

export function isRecord(value: unknown): value is JsonRecord {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

export function ownWindowPropertyValue<T>(
  hostWindow: Window | undefined,
  propertyName: string,
): T | undefined {
  if (hostWindow === undefined) {
    return undefined;
  }
  const descriptor = Object.getOwnPropertyDescriptor(hostWindow, propertyName);
  if (descriptor === undefined) {
    return undefined;
  }
  if ("value" in descriptor) {
    return descriptor.value as T | undefined;
  }
  if (typeof descriptor.get === "function") {
    try {
      return descriptor.get.call(hostWindow) as T | undefined;
    } catch {
      return undefined;
    }
  }
  return undefined;
}

function parseRecord<T>(value: unknown, mapper: (record: JsonRecord) => T): T | null {
  if (!isRecord(value)) {
    return null;
  }
  return mapper(value);
}

export function booleanFromValue(value: unknown, fallback = false): boolean {
  return typeof value === "boolean" ? value : fallback;
}

export function integerFromValue(value: unknown, fallback: number): number {
  return Math.round(numberFromValue(value, fallback));
}

export function numberFromValue(value: unknown, fallback: number): number {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

export function stringFromValue(value: unknown, fallback = ""): string {
  return typeof value === "string" ? value : fallback;
}

export interface CapabilityStatusViewState {
  key: string;
  label: string;
  status: BrowserHostCapabilityStatus;
  summary: string;
  detail: string;
}

export interface LabeledValueField {
  label: string;
  value: string;
  wide?: boolean;
}

export interface RectLike {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface WorkflowNavItem {
  id: Workflow;
  label: string;
  eyebrow: string;
  description: string;
}

export const workflowNavItems: readonly WorkflowNavItem[] = [
  {
    id: "train",
    label: "Train",
    eyebrow: "Dataset + runtime",
    description: "Training presets, device selection, and remote launch surfaces.",
  },
  {
    id: "validate",
    label: "Validate",
    eyebrow: "Evaluation",
    description: "Validation inputs, execution knobs, and report surfaces.",
  },
  {
    id: "predict",
    label: "Predict",
    eyebrow: "Inference",
    description: "Shared inference shell for predict and live startup settings.",
  },
  {
    id: "annotate",
    label: "Annotate",
    eyebrow: "Dataset editing",
    description: "Annotation sidebar, object tooling, and save workflow shell.",
  },
  {
    id: "export",
    label: "Export",
    eyebrow: "Artifacts",
    description: "ONNX and TensorRT export orchestration surfaces.",
  },
  {
    id: "live",
    label: "Live",
    eyebrow: "Realtime preview",
    description: "Live controller, preview, and analyzer surfaces.",
  },
];

export function primaryActionLabelText(
  workflow: Workflow,
  trainExecutionTarget: "local" | "remote",
  sourceKind: SourceKind,
  livePredictActive: boolean,
): string {
  switch (workflow) {
    case "train":
      return trainExecutionTarget === "remote"
        ? "Start / Stop Remote"
        : "Start / Stop Training";
    case "validate":
      return "Run Validation";
    case "annotate":
      return "Save Annotations";
    case "export":
      return "Run Export";
    case "live":
      return livePredictActive ? "Stop Live Predict" : "Run Live Predict";
    case "predict":
      if (livePredictActive) {
        return "Stop Live Predict";
      }
      return sourceKind === "video_stream" ? "Run Live Predict" : "Run Predict";
    default:
      return "Run Predict";
  }
}

export function capabilityStatusFromValue(
  value: unknown,
  fallback: BrowserHostCapabilityStatus,
): BrowserHostCapabilityStatus {
  switch (stringFromValue(value).trim()) {
    case "active":
      return "active";
    case "ready":
    case "available":
      return "ready";
    case "fallback":
    case "degraded":
    case "warning":
      return "fallback";
    case "blocked":
    case "error":
    case "unavailable":
    case "disabled":
      return "blocked";
    case "pending":
    case "waiting":
    case "loading":
      return "pending";
    default:
      return fallback;
  }
}

export function capabilityContractRecord(
  parent: unknown,
  key: string,
): JsonRecord | null {
  if (!isRecord(parent)) {
    return null;
  }
  const containers = [
    parent.capabilities,
    parent.capability_status,
    parent.capability_statuses,
    parent.runtime_capabilities,
    parent.status,
    parent.statuses,
  ];
  for (const container of containers) {
    if (isRecord(container) && isRecord(container[key])) {
      return container[key];
    }
  }
  return null;
}

export function capabilityStatusViewStateFromValue(
  value: unknown,
  fallback: CapabilityStatusViewState,
): CapabilityStatusViewState {
  const record = isRecord(value) ? value : null;
  let status = capabilityStatusFromValue(
    record?.status ?? record?.state ?? record?.tone,
    fallback.status,
  );
  if (record !== null) {
    const hasAvailability =
      Object.prototype.hasOwnProperty.call(record, "available") ||
      Object.prototype.hasOwnProperty.call(record, "enabled") ||
      Object.prototype.hasOwnProperty.call(record, "supported");
    const availability = hasAvailability
      ? booleanFromValue(
          record.available ?? record.enabled ?? record.supported,
          false,
        )
      : null;
    if (booleanFromValue(record.active, false)) {
      status = "active";
    } else if (
      booleanFromValue(record.fallback, false) ||
      booleanFromValue(record.degraded, false)
    ) {
      status = "fallback";
    } else if (
      booleanFromValue(record.pending, false) ||
      booleanFromValue(record.loading, false) ||
      booleanFromValue(record.waiting, false)
    ) {
      status = "pending";
    } else if (booleanFromValue(record.blocked, false) || availability === false) {
      status = "blocked";
    } else if (booleanFromValue(record.ready, false) || availability === true) {
      status = "ready";
    }
  }

  const label = stringFromValue(record?.label).trim();
  const summary = stringFromValue(record?.summary).trim();
  const detail = stringFromValue(record?.detail ?? record?.description).trim();
  return {
    ...fallback,
    label: label.length > 0 ? label : fallback.label,
    status,
    summary: summary.length > 0 ? summary : fallback.summary,
    detail: detail.length > 0 ? detail : fallback.detail,
  };
}

export function statusItemFromContract(
  parent: unknown,
  key: string | ReadonlyArray<string>,
  fallback: CapabilityStatusViewState,
): CapabilityStatusViewState {
  const keys = Array.isArray(key) ? key : [key];
  for (const currentKey of keys) {
    const contract = capabilityContractRecord(parent, currentKey);
    if (contract !== null) {
      return capabilityStatusViewStateFromValue(contract, fallback);
    }
  }
  return fallback;
}

export function workflowDetailsPendingStatusItem(): CapabilityStatusViewState {
  return {
    key: "details",
    label: "Details",
    status: "pending",
    summary: "workflow details pending",
    detail: "No additional details surface is published for this route.",
  };
}

export function capabilitySummaryText(
  items: ReadonlyArray<CapabilityStatusViewState>,
  fallback = "",
): string {
  const summaries: string[] = [];
  for (const item of items) {
    const summary = item.summary.trim();
    if (summary.length === 0 || summaries.includes(summary)) {
      continue;
    }
    summaries.push(summary);
  }
  return summaries.length > 0 ? summaries.join(" · ") : fallback;
}

export function combinedCapabilityStatus(
  items: ReadonlyArray<CapabilityStatusViewState>,
  fallback: CapabilityStatusViewState["status"] = "pending",
): CapabilityStatusViewState["status"] {
  if (items.some((item) => item.status === "active")) {
    return "active";
  }
  if (items.some((item) => item.status === "fallback")) {
    return "fallback";
  }
  if (items.some((item) => item.status === "ready")) {
    return "ready";
  }
  if (items.some((item) => item.status === "pending")) {
    return "pending";
  }
  if (items.some((item) => item.status === "blocked")) {
    return "blocked";
  }
  return fallback;
}

export function labeledValueField(
  label: string,
  value: string,
  wide = false,
): LabeledValueField {
  return { label, value, wide };
}

export function jobOutputText(job: JobState): string {
  const outputTail = job.output_tail.trim();
  if (outputTail.length > 0) {
    return outputTail;
  }
  if (job.recent_logs.length === 0) {
    return "No job output has been published yet.";
  }
  return job.recent_logs
    .map((entry) => `[${entry.sequence}] ${entry.level}: ${entry.message}`)
    .join("\n");
}

export function compactDisplayText(value: unknown, fallback = "-"): string {
  const text = stringFromValue(value).trim();
  return text.length > 0 ? text : fallback;
}

export function integerDisplayText(value: unknown, fallback = "-"): string {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? String(Math.round(parsed)) : fallback;
}

export const kWorkspaceSurfaceId = "workspace";
export const kWorkspaceTextureFormat = "rgba8unorm";

export function workspaceSurfaceInfoFromValue(value: unknown): WorkspaceSurfaceInfo | null {
  if (!isRecord(value)) {
    return null;
  }
  const surfaceId = String(value.surfaceId ?? value.surface_id ?? "").trim();
  const revision = String(value.revision ?? "").trim();
  const width = Number(value.width);
  const height = Number(value.height);
  const textureFormat = String(
    value.textureFormat ?? value.texture_format ?? "",
  ).trim();
  if (
    surfaceId !== kWorkspaceSurfaceId ||
    revision.length === 0 ||
    !Number.isFinite(width) ||
    !Number.isFinite(height) ||
    width <= 0 ||
    height <= 0 ||
    textureFormat !== kWorkspaceTextureFormat ||
    value.opaque !== true ||
    value.upright !== true
  ) {
    return null;
  }
  return {
    surfaceId: kWorkspaceSurfaceId,
    revision,
    width: Math.round(width),
    height: Math.round(height),
    textureFormat: kWorkspaceTextureFormat,
    opaque: true,
    upright: true,
  };
}

export function clampNumber(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

export function workflowStateRoot(snapshot: StateSnapshot): JsonRecord | null {
  return isRecord(snapshot.workflow_state) ? snapshot.workflow_state : null;
}

export function workflowFromValue(value: unknown, fallback: Workflow): Workflow {
  switch (value) {
    case "train":
    case "validate":
    case "predict":
    case "annotate":
    case "export":
    case "live":
      return value;
    default:
      return fallback;
  }
}

export function workflowRecordKey(workflow: Workflow): Exclude<Workflow, "live"> {
  return workflow === "live" ? "predict" : workflow;
}

export function rectLikeEquals<T extends RectLike>(
  left: T | null,
  right: T | null,
): boolean {
  if (left === right) {
    return true;
  }
  if (left === null || right === null) {
    return false;
  }
  return (
    left.x === right.x &&
    left.y === right.y &&
    left.width === right.width &&
    left.height === right.height
  );
}

export function captureRegionEquals(
  left: CaptureRegion | null,
  right: CaptureRegion | null,
): boolean {
  return rectLikeEquals(left, right);
}

export function workflowRecords(snapshot: StateSnapshot): JsonRecord | null {
  const root = workflowStateRoot(snapshot);
  if (root === null || !isRecord(root.workflows)) {
    return null;
  }
  return root.workflows;
}

export function workflowRecord(
  snapshot: StateSnapshot,
  workflow: Workflow,
): JsonRecord | null {
  const key = workflowRecordKey(workflow);
  const records = workflowRecords(snapshot);
  if (records === null || !isRecord(records[key])) {
    return null;
  }
  return records[key];
}

export function workflowLeafRecord(
  snapshot: StateSnapshot,
  workflow: Workflow,
): JsonRecord | null {
  const key = workflowRecordKey(workflow);
  const record = workflowRecord(snapshot, workflow);
  if (record === null || !isRecord(record[key])) {
    return null;
  }
  return record[key];
}

export function workflowSourceRecord(
  snapshot: StateSnapshot,
  workflow: Workflow,
): JsonRecord | null {
  const record = workflowRecord(snapshot, workflow);
  if (record === null || !isRecord(record.source)) {
    return null;
  }
  return record.source;
}

export function workflowSectionRecord(
  snapshot: StateSnapshot,
  workflow: Workflow,
  key: string,
): JsonRecord | null {
  const record = workflowRecord(snapshot, workflow);
  if (record === null || !isRecord(record[key])) {
    return null;
  }
  return record[key];
}

export function updateWorkflowSectionRecord(
  snapshot: StateSnapshot,
  workflow: Workflow,
  key: string,
  patch: JsonRecord,
): void {
  const recordKey = workflowRecordKey(workflow);
  const workflows = isRecord(snapshot.workflow_state.workflows)
    ? snapshot.workflow_state.workflows
    : {};
  const workflowRecordValue = isRecord(workflows[recordKey]) ? workflows[recordKey] : {};
  const sectionRecord = isRecord(workflowRecordValue[key]) ? workflowRecordValue[key] : {};
  snapshot.workflow_state.workflows = {
    ...workflows,
    [recordKey]: {
      ...workflowRecordValue,
      [key]: {
        ...sectionRecord,
        ...patch,
      },
    },
  };
}

export function annotateSidebarCategoryStateFromValue(
  value: unknown,
): AnnotateSidebarCategoryState | null {
  return parseRecord(value, (record) => ({
    id: integerFromValue(record.id, 0),
    name: stringFromValue(record.name, "Unnamed"),
  }));
}

export function annotateSidebarObjectStateFromValue(
  value: unknown,
): AnnotateSidebarObjectState | null {
  return parseRecord(value, (record) => ({
    index: integerFromValue(record.index, 0),
    label: stringFromValue(record.label, "Object"),
    selected: booleanFromValue(record.selected, false),
  }));
}

export function annotateColorRangeToleranceDefaults(): AnnotateColorToleranceState {
  return {
    hueMinusPct: 0,
    huePlusPct: 0,
    saturationMinusPct: 0,
    saturationPlusPct: 0,
    valueMinusPct: 0,
    valuePlusPct: 0,
  };
}

export function defaultAnnotateColorRangeState(): AnnotateColorRangeState {
  return {
    center: {
      hueDegrees: 180,
      saturation: 0.5,
      value: 0.5,
    },
    tolerance: annotateColorRangeToleranceDefaults(),
    sampling: false,
  };
}

export function annotateColorRangeStateFromValue(value: unknown): AnnotateColorRangeState {
  const fallback = defaultAnnotateColorRangeState();
  if (!isRecord(value)) {
    return fallback;
  }

  const center = isRecord(value.center) ? value.center : {};
  const tolerance = isRecord(value.tolerance) ? value.tolerance : {};
  return {
    center: {
      hueDegrees: clampNumber(
        numberFromValue(center.hue_degrees, fallback.center.hueDegrees),
        0,
        360,
      ),
      saturation: clampNumber(
        numberFromValue(center.saturation, fallback.center.saturation),
        0,
        1,
      ),
      value: clampNumber(numberFromValue(center.value, fallback.center.value), 0, 1),
    },
    tolerance: {
      hueMinusPct: clampNumber(
        numberFromValue(tolerance.hue_minus_pct, fallback.tolerance.hueMinusPct),
        0,
        100,
      ),
      huePlusPct: clampNumber(
        numberFromValue(tolerance.hue_plus_pct, fallback.tolerance.huePlusPct),
        0,
        100,
      ),
      saturationMinusPct: clampNumber(
        numberFromValue(
          tolerance.saturation_minus_pct,
          fallback.tolerance.saturationMinusPct,
        ),
        0,
        100,
      ),
      saturationPlusPct: clampNumber(
        numberFromValue(
          tolerance.saturation_plus_pct,
          fallback.tolerance.saturationPlusPct,
        ),
        0,
        100,
      ),
      valueMinusPct: clampNumber(
        numberFromValue(tolerance.value_minus_pct, fallback.tolerance.valueMinusPct),
        0,
        100,
      ),
      valuePlusPct: clampNumber(
        numberFromValue(tolerance.value_plus_pct, fallback.tolerance.valuePlusPct),
        0,
        100,
      ),
    },
    sampling: booleanFromValue(value.sampling, fallback.sampling),
  };
}

function annotateSplineState(value: unknown): AnnotateSplineState | null {
  if (!isRecord(value)) {
    return null;
  }
  const activeHandleModeRaw = stringFromValue(value.active_handle_mode);
  const activeHandleMode =
    activeHandleModeRaw === "corner" ||
    activeHandleModeRaw === "smooth" ||
    activeHandleModeRaw === "mirrored"
      ? activeHandleModeRaw
      : null;
  return {
    knotCount: integerFromValue(value.knot_count, 0),
    segmentCount: integerFromValue(value.segment_count, 0),
    closed: booleanFromValue(value.closed, false),
    activeKnotIndex:
      value.active_knot_index === null || value.active_knot_index === undefined
        ? null
        : integerFromValue(value.active_knot_index, 0),
    activeSegmentIndex:
      value.active_segment_index === null || value.active_segment_index === undefined
        ? null
        : integerFromValue(value.active_segment_index, 0),
    activeHandleMode,
    closeIntent: booleanFromValue(value.close_intent, false),
    reopenRequested: booleanFromValue(value.reopen_requested, false),
    canClose: booleanFromValue(value.can_close, false),
    canReopen: booleanFromValue(value.can_reopen, false),
    canInsertActiveSegmentKnot: booleanFromValue(
      value.can_insert_active_segment_knot,
      false,
    ),
    canDeleteActiveKnot: booleanFromValue(value.can_delete_active_knot, false),
    canEditActiveHandleMode: booleanFromValue(
      value.can_edit_active_handle_mode,
      false,
    ),
  };
}

function annotateSkeletonJointState(value: unknown): AnnotateSkeletonJointState | null {
  if (!isRecord(value)) {
    return null;
  }
  return {
    index: integerFromValue(value.index, 0),
    label: stringFromValue(value.label, "Joint"),
    visible: booleanFromValue(value.visible, false),
    active: booleanFromValue(value.active, false),
  };
}

function annotateSkeletonState(value: unknown): AnnotateSkeletonState | null {
  if (!isRecord(value)) {
    return null;
  }
  const joints = Array.isArray(value.joints)
    ? value.joints
        .map((joint) => annotateSkeletonJointState(joint))
        .filter((joint): joint is AnnotateSkeletonJointState => joint !== null)
    : [];
  return {
    visibleJointCount: integerFromValue(value.visible_joint_count, 0),
    totalJointCount: integerFromValue(value.total_joint_count, joints.length),
    activeJointIndex:
      value.active_joint_index === null || value.active_joint_index === undefined
        ? null
        : integerFromValue(value.active_joint_index, 0),
    activeJointKey:
      value.active_joint_key === null || value.active_joint_key === undefined
        ? null
        : stringFromValue(value.active_joint_key),
    nextJointIndex:
      value.next_joint_index === null || value.next_joint_index === undefined
        ? null
        : integerFromValue(value.next_joint_index, 0),
    nextJointKey:
      value.next_joint_key === null || value.next_joint_key === undefined
        ? null
        : stringFromValue(value.next_joint_key),
    activeJointVisible: booleanFromValue(value.active_joint_visible, false),
    reseedRequested: booleanFromValue(value.reseed_requested, false),
    canSkipJoint: booleanFromValue(value.can_skip_joint, false),
    canHideActiveJoint: booleanFromValue(value.can_hide_active_joint, false),
    canReactivateActiveJoint: booleanFromValue(
      value.can_reactivate_active_joint,
      false,
    ),
    canReseedActiveJoint: booleanFromValue(value.can_reseed_active_joint, false),
    joints,
  };
}

function annotateSelectedObjectState(
  value: unknown,
): AnnotateSidebarSelectedObjectState | null {
  if (!isRecord(value)) {
    return null;
  }
  const displayBox = isRecord(value.display_box)
    ? {
        x1: integerFromValue(value.display_box.x1, 0),
        y1: integerFromValue(value.display_box.y1, 0),
        x2: integerFromValue(value.display_box.x2, 0),
        y2: integerFromValue(value.display_box.y2, 0),
      }
    : null;
  return {
    enabled: booleanFromValue(value.enabled, true),
    categoryIndex: integerFromValue(value.category_index, 0),
    shapeLabel: stringFromValue(value.shape_label),
    displayBox,
    supportsMaskEditing: booleanFromValue(value.supports_mask_editing, false),
    sup: annotateColorRangeStateFromValue(value.sup),
    nosup: annotateColorRangeStateFromValue(value.nosup),
    point: isRecord(value.point)
      ? {
          x: numberFromValue(value.point.x, 0),
          y: numberFromValue(value.point.y, 0),
        }
      : null,
    spline: annotateSplineState(value.spline),
    skeleton: annotateSkeletonState(value.skeleton),
  };
}

export function annotateSidebarRecord(snapshot: StateSnapshot): JsonRecord {
  return isRecord(snapshot.workflow_state.annotate_sidebar)
    ? snapshot.workflow_state.annotate_sidebar
    : {};
}

export function annotateWorkflowLeaf(snapshot: StateSnapshot): JsonRecord | null {
  const workflows = isRecord(snapshot.workflow_state.workflows)
    ? snapshot.workflow_state.workflows
    : null;
  if (workflows === null || !isRecord(workflows.annotate)) {
    return null;
  }
  const annotateWorkflow = workflows.annotate;
  return isRecord(annotateWorkflow.annotate) ? annotateWorkflow.annotate : null;
}

export function annotateAssistAvailable(snapshot: StateSnapshot): boolean {
  const annotateLeaf = annotateWorkflowLeaf(snapshot);
  if (annotateLeaf === null) {
    return false;
  }
  const modelInput = annotateLeaf.model_input;
  return !(
    modelInput === "none" ||
    modelInput === 3 ||
    modelInput === "3"
  );
}

export function annotateSidebarState(snapshot: StateSnapshot): AnnotateSidebarState | null {
  const root = workflowStateRoot(snapshot);
  if (root === null || !isRecord(root.annotate_sidebar)) {
    return null;
  }

  const sidebar = root.annotate_sidebar;
  const categories = Array.isArray(sidebar.categories)
    ? sidebar.categories
        .map((value) => annotateSidebarCategoryStateFromValue(value))
        .filter((value): value is AnnotateSidebarCategoryState => value !== null)
    : [];
  const objects = Array.isArray(sidebar.objects)
    ? sidebar.objects
        .map((value) => annotateSidebarObjectStateFromValue(value))
        .filter((value): value is AnnotateSidebarObjectState => value !== null)
    : [];
  const selectedObject = annotateSelectedObjectState(sidebar.selected_object);

  return {
    hasAnnotationFrame: booleanFromValue(sidebar.has_annotation_frame, false),
    cleanupRadius: clampNumber(integerFromValue(sidebar.cleanup_radius, 1), 1, 32),
    assistAvailable: booleanFromValue(sidebar.assist_available, false),
    assistRunning: booleanFromValue(sidebar.assist_running, false),
    assistSummary: stringFromValue(sidebar.assist_summary),
    assistError: stringFromValue(sidebar.assist_error),
    preferredNewObjectCategoryIndex: integerFromValue(
      sidebar.preferred_new_object_category_index,
      selectedObject?.categoryIndex ?? 0,
    ),
    hasClasses: booleanFromValue(sidebar.has_classes, false),
    canUndo: booleanFromValue(sidebar.can_undo, false),
    canRedo: booleanFromValue(sidebar.can_redo, false),
    canCreateObjects: booleanFromValue(sidebar.can_create_objects, false),
    canDeleteSelected: booleanFromValue(sidebar.can_delete_selected, false),
    canRedrawSelectedBox: booleanFromValue(sidebar.can_redraw_selected_box, false),
    canEditSelectedMask: booleanFromValue(sidebar.can_edit_selected_mask, false),
    hasSelectedObject: booleanFromValue(sidebar.has_selected_object, false),
    selectedObjectIndex:
      sidebar.selected_object_index === null || sidebar.selected_object_index === undefined
        ? null
        : integerFromValue(sidebar.selected_object_index, 0),
    categories,
    objects,
    selectedObject,
  };
}

export function annotateSelectedSummary(sidebar: AnnotateSidebarState | null): string {
  if (sidebar?.selectedObject === null || sidebar?.selectedObject === undefined) {
    return "Select an object to enable redraw, mask, spline, and skeleton controls.";
  }
  const selected = sidebar.selectedObject;
  const box =
    selected.displayBox === null
      ? "no box"
      : `${selected.displayBox.x1},${selected.displayBox.y1} -> ${selected.displayBox.x2},${selected.displayBox.y2}`;
  return `${selected.shapeLabel || "object"} · category ${selected.categoryIndex} · ${box}`;
}

export function isRemoteTrainControlsActive(
  snapshot: StateSnapshot,
  trainTarget: unknown,
): boolean {
  return snapshot.active_workflow === "train" && trainTarget === "remote";
}

export function workspaceFrameRectangle(geometry: WorkspaceGeometry): {
  x: number;
  y: number;
  width: number;
  height: number;
} {
  return {
    x: geometry.frameX,
    y: geometry.frameY,
    width: geometry.frameWidth,
    height: geometry.frameHeight,
  };
}
