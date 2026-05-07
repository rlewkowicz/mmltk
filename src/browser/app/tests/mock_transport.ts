import type {
  BrowserHostTransport,
  IntentMessage,
  StateSnapshot,
  Workflow,
} from "../src/host_api";
import { HOST_API_CONTRACT_HASH } from "../src/host_api.generated";
import { assertHostApiIntent } from "./support/host-api-contract-validator";

const kProtocolVersion = 2;
const kCaptureWidth = 1280;
const kCaptureHeight = 720;
const kDefaultBrushRadius = 12;
const kSetupFrameCount = 6;

type JsonRecord = Record<string, unknown>;

interface MockPoint {
  x: number;
  y: number;
}

interface MockBox {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

interface MockColorRange {
  center: {
    hue_degrees: number;
    saturation: number;
    value: number;
  };
  tolerance: {
    hue_minus_pct: number;
    hue_plus_pct: number;
    saturation_minus_pct: number;
    saturation_plus_pct: number;
    value_minus_pct: number;
    value_plus_pct: number;
  };
  sampling: boolean;
}

interface MockSpline {
  knotPoints: MockPoint[];
  activeKnotIndex: number | null;
}

interface MockSkeletonJoint {
  label: string;
  visible: boolean;
  capturePoint: MockPoint;
}

interface MockSkeleton {
  activeJointIndex: number | null;
  joints: MockSkeletonJoint[];
}

interface MockObject {
  categoryIndex: number;
  shapeLabel: "Box" | "Spline" | "Point" | "Skeleton";
  displayBox: MockBox;
  point: MockPoint | null;
  spline: MockSpline | null;
  skeleton: MockSkeleton | null;
  sup: MockColorRange;
  nosup: MockColorRange;
}

interface MockBoxDragState {
  kind: "create" | "move";
  objectIndex: number | null;
  startPoint: MockPoint;
  startBox: MockBox | null;
}

interface MockTransportState {
  stateRevision: number;
  activeWorkflow: Workflow;
  sourceKind: number;
  activeTool: string;
  setupFrameIndex: number;
  brushRadius: number;
  holdSave: boolean;
  liveAnnotateRunning: boolean;
  liveMode: "none" | "predict";
  liveControllerRunning: boolean;
  fitToCapture: boolean;
  fullFrameDisplay: boolean;
  selectedObjectIndex: number;
  jobSummary: string;
  objects: MockObject[];
  boxDrag: MockBoxDragState | null;
}

function point(x: number, y: number): MockPoint {
  return { x, y };
}

function box(x1: number, y1: number, x2: number, y2: number): MockBox {
  return { x1, y1, x2, y2 };
}

function makeColorRange(): MockColorRange {
  return {
    center: {
      hue_degrees: 180,
      saturation: 0.5,
      value: 0.5,
    },
    tolerance: {
      hue_minus_pct: 0,
      hue_plus_pct: 0,
      saturation_minus_pct: 0,
      saturation_plus_pct: 0,
      value_minus_pct: 0,
      value_plus_pct: 0,
    },
    sampling: false,
  };
}

function cloneColorRange(value: MockColorRange): MockColorRange {
  return structuredClone(value);
}

function boxSupportsMaskEditing(shapeLabel: MockObject["shapeLabel"]): boolean {
  return shapeLabel === "Box";
}

function makePointBox(capturePoint: MockPoint, radius = 8): MockBox {
  return box(
    capturePoint.x - radius,
    capturePoint.y - radius,
    capturePoint.x + radius + 1,
    capturePoint.y + radius + 1,
  );
}

function normalizeDragBox(start: MockPoint, end: MockPoint): MockBox {
  return box(
    Math.min(start.x, end.x),
    Math.min(start.y, end.y),
    Math.max(start.x, end.x) + 1,
    Math.max(start.y, end.y) + 1,
  );
}

function translateBox(value: MockBox, deltaX: number, deltaY: number): MockBox {
  return box(
    value.x1 + deltaX,
    value.y1 + deltaY,
    value.x2 + deltaX,
    value.y2 + deltaY,
  );
}

function expandBoxAroundPoint(
  value: MockBox,
  capturePoint: MockPoint,
  radius: number,
): MockBox {
  return box(
    Math.min(value.x1, capturePoint.x - radius),
    Math.min(value.y1, capturePoint.y - radius),
    Math.max(value.x2, capturePoint.x + radius + 1),
    Math.max(value.y2, capturePoint.y + radius + 1),
  );
}

function initialObjects(): MockObject[] {
  return [
    {
      categoryIndex: 0,
      shapeLabel: "Box",
      displayBox: box(128, 84, 392, 316),
      point: null,
      spline: null,
      skeleton: null,
      sup: makeColorRange(),
      nosup: makeColorRange(),
    },
    {
      categoryIndex: 1,
      shapeLabel: "Spline",
      displayBox: box(412, 122, 734, 448),
      point: null,
      spline: {
        knotPoints: [point(452, 164), point(520, 240), point(684, 392)],
        activeKnotIndex: 1,
      },
      skeleton: null,
      sup: makeColorRange(),
      nosup: makeColorRange(),
    },
    {
      categoryIndex: 2,
      shapeLabel: "Point",
      displayBox: makePointBox(point(912, 188), 10),
      point: point(912, 188),
      spline: null,
      skeleton: null,
      sup: makeColorRange(),
      nosup: makeColorRange(),
    },
  ];
}

function makeInitialState(): MockTransportState {
  return {
    stateRevision: 1,
    activeWorkflow: "annotate",
    sourceKind: 1,
    activeTool: "box",
    setupFrameIndex: 0,
    brushRadius: kDefaultBrushRadius,
    holdSave: false,
    liveAnnotateRunning: false,
    liveMode: "none",
    liveControllerRunning: false,
    fitToCapture: false,
    fullFrameDisplay: false,
    selectedObjectIndex: 0,
    jobSummary: "mock browser transport ready",
    objects: initialObjects(),
    boxDrag: null,
  };
}

function selectedObject(state: MockTransportState): MockObject | null {
  return state.objects[state.selectedObjectIndex] ?? null;
}

function selectedBoxObject(state: MockTransportState): MockObject | null {
  const object = selectedObject(state);
  return object !== null && object.shapeLabel === "Box" ? object : null;
}

function integerFromValue(value: unknown, fallback: number): number {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? Math.round(parsed) : fallback;
}

function booleanFromValue(value: unknown, fallback = false): boolean {
  return typeof value === "boolean" ? value : fallback;
}

function stringFromValue(value: unknown, fallback = ""): string {
  return typeof value === "string" ? value : fallback;
}

function pointFromPayload(
  payload: Record<string, unknown>,
  fallback = point(0, 0),
): MockPoint {
  return point(
    integerFromValue(payload.capture_x, fallback.x),
    integerFromValue(payload.capture_y, fallback.y),
  );
}

function recordPoint(value: MockPoint): JsonRecord {
  return { x: value.x, y: value.y };
}

function recordBox(value: MockBox): JsonRecord {
  return { x1: value.x1, y1: value.y1, x2: value.x2, y2: value.y2 };
}

function cloneObjects(objects: MockObject[]): MockObject[] {
  return structuredClone(objects);
}

function objectRecord(object: MockObject): JsonRecord {
  const skeleton = object.skeleton;
  return {
    enabled: true,
    category_index: object.categoryIndex,
    shape_label: object.shapeLabel,
    display_box: recordBox(object.displayBox),
    supports_mask_editing: boxSupportsMaskEditing(object.shapeLabel),
    sup: structuredClone(object.sup),
    nosup: structuredClone(object.nosup),
    point: object.point === null ? null : recordPoint(object.point),
    spline:
      object.spline === null
        ? null
        : {
            knot_count: object.spline.knotPoints.length,
            segment_count: Math.max(object.spline.knotPoints.length - 1, 0),
            closed: false,
            active_knot_index: object.spline.activeKnotIndex,
            active_segment_index: null,
            active_handle_mode:
              object.spline.knotPoints.length > 1 ? "smooth" : null,
            close_intent: false,
            reopen_requested: false,
            can_close: object.spline.knotPoints.length > 2,
            can_reopen: false,
            can_insert_active_segment_knot:
              object.spline.knotPoints.length > 1,
            can_delete_active_knot: object.spline.knotPoints.length > 0,
            can_edit_active_handle_mode:
              object.spline.knotPoints.length > 0,
            knot_points: object.spline.knotPoints.map(recordPoint),
          },
    skeleton:
      skeleton === null
        ? null
        : {
            visible_joint_count: skeleton.joints.filter((joint) => joint.visible)
              .length,
            total_joint_count: skeleton.joints.length,
            active_joint_index: skeleton.activeJointIndex,
            active_joint_key:
              skeleton.activeJointIndex === null
                ? null
                : skeleton.joints[skeleton.activeJointIndex]?.label ?? null,
            next_joint_index: null,
            next_joint_key: null,
            active_joint_visible:
              skeleton.activeJointIndex === null
                ? false
                : skeleton.joints[skeleton.activeJointIndex]?.visible ?? false,
            reseed_requested: false,
            can_skip_joint: true,
            can_hide_active_joint: true,
            can_reactivate_active_joint: false,
            can_reseed_active_joint: true,
            joints: skeleton.joints.map((joint, index) => ({
              index,
              label: joint.label,
              visible: joint.visible,
              active: index === skeleton.activeJointIndex,
            })),
          },
  };
}

function geometryRecord(object: MockObject): JsonRecord {
  if (object.point !== null) {
    return {
      capture_points: [recordPoint(object.point)],
    };
  }
  if (object.spline !== null) {
    return {
      capture_points: object.spline.knotPoints.map(recordPoint),
    };
  }
  if (object.skeleton !== null) {
    return {
      capture_points: object.skeleton.joints.map((joint) =>
        recordPoint(joint.capturePoint),
      ),
    };
  }
  return {
    capture_points: [
      recordPoint(
        point(
          Math.round((object.displayBox.x1 + object.displayBox.x2) / 2),
          Math.round((object.displayBox.y1 + object.displayBox.y2) / 2),
        ),
      ),
    ],
  };
}

function visibleObjectsRecord(state: MockTransportState): JsonRecord[] {
  return state.objects.map((object, index) => ({
    index,
    category_index: object.categoryIndex,
    shape_type: object.shapeLabel.toLowerCase(),
    capture_box: recordBox(object.displayBox),
    fully_visible: true,
    geometry: geometryRecord(object),
  }));
}

function editableHandlesRecord(state: MockTransportState): JsonRecord[] {
  const object = selectedObject(state);
  if (object === null || object.spline === null) {
    return [];
  }
  return object.spline.knotPoints.map((capturePoint, elementIndex) => ({
    object_index: state.selectedObjectIndex,
    element_index: elementIndex,
    role: "spline_knot",
    capture_point: recordPoint(capturePoint),
  }));
}

function sourceKindName(kind: number): StateSnapshot["source"]["kind"] {
  return kind === 3 ? "video_stream" : "single_image";
}

function buildSnapshot(state: MockTransportState): StateSnapshot {
  const currentObject = selectedObject(state);
  return {
    type: "state.snapshot",
    protocol_version: kProtocolVersion,
    contract_hash: HOST_API_CONTRACT_HASH,
    state_revision: state.stateRevision,
    active_workflow: state.activeWorkflow,
    workflow_state: {
      workflows: {
        annotate: {
          source: {
            kind: state.sourceKind,
          },
          annotate: {
            active_tool: state.activeTool,
          },
        },
      },
      annotate_controls: {
        setup_frame_navigation: {
          reload: {
            intent: "annotate.setup_frame.reload",
          },
          current_index: state.setupFrameIndex,
          input_count: kSetupFrameCount,
        },
        save: {
          save_now: {
            intent: "annotate.save_now",
          },
          hold_save: {
            value: state.holdSave,
          },
        },
        brush: {
          visible: state.activeTool.startsWith("mask."),
          radius: state.brushRadius,
        },
        live_annotate: {
          visible: state.sourceKind === 3 || state.liveAnnotateRunning,
          running: state.liveAnnotateRunning,
        },
      },
      annotate_sidebar: {
        has_annotation_frame: true,
        cleanup_radius: 0,
        assist_available: false,
        assist_running: false,
        assist_summary: "",
        assist_error: "",
        preferred_new_object_category_index: 0,
        has_classes: true,
        can_undo: false,
        can_redo: false,
        can_create_objects: true,
        can_delete_selected: currentObject !== null,
        can_redraw_selected_box: currentObject?.shapeLabel === "Box",
        can_edit_selected_mask:
          currentObject !== null &&
          boxSupportsMaskEditing(currentObject.shapeLabel),
        has_selected_object: currentObject !== null,
        selected_object_index:
          currentObject === null ? null : state.selectedObjectIndex,
        categories: [
          { id: 0, name: "Vehicle" },
          { id: 1, name: "Person" },
          { id: 2, name: "Fixture" },
        ],
        objects: state.objects.map((object, index) => ({
          index,
          label: `${object.shapeLabel} ${index + 1}`,
          selected: index === state.selectedObjectIndex,
        })),
        selected_object:
          currentObject === null ? null : objectRecord(currentObject),
      },
      annotate_workspace_scene: {
        document_generation: state.stateRevision,
        selected_object_index:
          currentObject === null ? null : state.selectedObjectIndex,
        capture_width: kCaptureWidth,
        capture_height: kCaptureHeight,
        visible_objects: visibleObjectsRecord(state),
        editable_handles: editableHandlesRecord(state),
      },
      live_preview_controls: {
        fit_to_capture: {
          value: state.fitToCapture,
          enabled: true,
          intent: "live.preview.fit_to_capture",
          payload_key: "enabled",
        },
        full_frame_display: {
          value: state.fullFrameDisplay,
          enabled: true,
          intent: "live.preview.full_frame_display",
          payload_key: "enabled",
        },
      },
      live_runtime: {
        active_mode: state.liveMode,
        controller: {
          running: state.liveControllerRunning,
        },
        preview: {
          fit_to_capture: state.fitToCapture,
          crop_overlay_mode: state.fullFrameDisplay,
        },
      },
    },
    settings_state: {},
    job: {
      running: state.liveControllerRunning || state.liveAnnotateRunning,
      label:
        state.liveControllerRunning || state.liveAnnotateRunning
          ? "running"
          : "idle",
      summary: state.jobSummary,
      error: "",
      output_tail: "",
      recent_logs: [],
    },
    source: {
      kind: sourceKindName(state.sourceKind),
      locator:
        state.sourceKind === 3 ? "device:0" : "/tmp/mock-browser-frame.png",
      recursive: false,
      device_index: 0,
      capture_width: kCaptureWidth,
      capture_height: kCaptureHeight,
      capture_fps: state.sourceKind === 3 ? 30 : 0,
      v4l2_buffer_count: state.sourceKind === 3 ? 4 : 0,
      has_crop: false,
      crop: {
        x: 0,
        y: 0,
        width: 0,
        height: 0,
      },
    },
    annotation: {
      document_generation: state.stateRevision,
      session_revision: state.stateRevision,
      capture_width: kCaptureWidth,
      capture_height: kCaptureHeight,
      instance_count: state.objects.length,
      selected_instance:
        currentObject === null ? null : state.selectedObjectIndex,
    },
    runtime_capabilities: {
      host_backend: "unknown",
      navigator_gpu: "available",
      workspace_surface_bridge: "unknown",
      workspace_surface_zero_copy: "unknown",
    },
    workspace_surface: null,
  };
}

function copyColorRangeFromValue(
  value: unknown,
  fallback: MockColorRange,
): MockColorRange {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    return cloneColorRange(fallback);
  }
  const record = value as Record<string, unknown>;
  const center =
    record.center !== null &&
    typeof record.center === "object" &&
    !Array.isArray(record.center)
      ? (record.center as Record<string, unknown>)
      : {};
  const tolerance =
    record.tolerance !== null &&
    typeof record.tolerance === "object" &&
    !Array.isArray(record.tolerance)
      ? (record.tolerance as Record<string, unknown>)
      : {};
  return {
    center: {
      hue_degrees: Number(center.hue_degrees ?? fallback.center.hue_degrees),
      saturation: Number(center.saturation ?? fallback.center.saturation),
      value: Number(center.value ?? fallback.center.value),
    },
    tolerance: {
      hue_minus_pct: Number(
        tolerance.hue_minus_pct ?? fallback.tolerance.hue_minus_pct,
      ),
      hue_plus_pct: Number(
        tolerance.hue_plus_pct ?? fallback.tolerance.hue_plus_pct,
      ),
      saturation_minus_pct: Number(
        tolerance.saturation_minus_pct ??
          fallback.tolerance.saturation_minus_pct,
      ),
      saturation_plus_pct: Number(
        tolerance.saturation_plus_pct ??
          fallback.tolerance.saturation_plus_pct,
      ),
      value_minus_pct: Number(
        tolerance.value_minus_pct ?? fallback.tolerance.value_minus_pct,
      ),
      value_plus_pct: Number(
        tolerance.value_plus_pct ?? fallback.tolerance.value_plus_pct,
      ),
    },
    sampling: booleanFromValue(record.sampling, fallback.sampling),
  };
}

function setJobSummary(state: MockTransportState, summary: string): void {
  state.jobSummary = summary;
}

function handleSidebarIntent(
  state: MockTransportState,
  payload: Record<string, unknown>,
): void {
  const action = stringFromValue(payload.action);
  if (action === "select_object") {
    const objectIndex = integerFromValue(
      payload.object_index,
      state.selectedObjectIndex,
    );
    if (state.objects[objectIndex] !== undefined) {
      state.selectedObjectIndex = objectIndex;
      setJobSummary(state, `annotate.sidebar selected object ${objectIndex}`);
    }
    return;
  }

  if (action === "mask.update_color_ranges") {
    const object = selectedObject(state);
    if (object === null) {
      return;
    }
    object.sup = copyColorRangeFromValue(payload.sup, object.sup);
    object.nosup = copyColorRangeFromValue(payload.nosup, object.nosup);
    setJobSummary(state, "annotate.sidebar updated mask color ranges");
  }
}

function handleWorkspaceClick(
  state: MockTransportState,
  payload: Record<string, unknown>,
): void {
  const tool = stringFromValue(payload.tool, state.activeTool);
  const capturePoint = pointFromPayload(payload, point(160, 140));
  const categoryIndex = integerFromValue(payload.default_category_index, 0);

  if (tool === "point") {
    state.objects.push({
      categoryIndex,
      shapeLabel: "Point",
      displayBox: makePointBox(capturePoint),
      point: capturePoint,
      spline: null,
      skeleton: null,
      sup: makeColorRange(),
      nosup: makeColorRange(),
    });
    state.selectedObjectIndex = state.objects.length - 1;
    setJobSummary(state, "annotate.workspace.click created point object");
    return;
  }

  if (tool === "spline") {
    state.objects.push({
      categoryIndex,
      shapeLabel: "Spline",
      displayBox: makePointBox(capturePoint, 20),
      point: null,
      spline: {
        knotPoints: [capturePoint],
        activeKnotIndex: 0,
      },
      skeleton: null,
      sup: makeColorRange(),
      nosup: makeColorRange(),
    });
    state.selectedObjectIndex = state.objects.length - 1;
    setJobSummary(state, "annotate.workspace.click created spline object");
    return;
  }

  if (tool === "skeleton") {
    const joints = [
      {
        label: "head",
        visible: true,
        capturePoint,
      },
      {
        label: "right_hip",
        visible: true,
        capturePoint: point(capturePoint.x, capturePoint.y + 72),
      },
      {
        label: "left_hip",
        visible: false,
        capturePoint: point(capturePoint.x - 32, capturePoint.y + 96),
      },
    ];
    state.objects.push({
      categoryIndex,
      shapeLabel: "Skeleton",
      displayBox: normalizeDragBox(
        point(capturePoint.x - 24, capturePoint.y - 24),
        point(capturePoint.x + 24, capturePoint.y + 96),
      ),
      point: null,
      spline: null,
      skeleton: {
        activeJointIndex: 1,
        joints,
      },
      sup: makeColorRange(),
      nosup: makeColorRange(),
    });
    state.selectedObjectIndex = state.objects.length - 1;
    setJobSummary(state, "annotate.workspace.click created skeleton object");
  }
}

function handleSplineDrag(
  state: MockTransportState,
  payload: Record<string, unknown>,
): void {
  const handle =
    payload.handle !== null &&
    typeof payload.handle === "object" &&
    !Array.isArray(payload.handle)
      ? (payload.handle as Record<string, unknown>)
      : {};
  const objectIndex = integerFromValue(
    handle.object_index,
    state.selectedObjectIndex,
  );
  const elementIndex = integerFromValue(handle.element_index, -1);
  const phase = stringFromValue(payload.phase);
  if (phase === "cancel") {
    setJobSummary(state, "annotate.workspace.handle_drag cancelled");
    return;
  }
  const object = state.objects[objectIndex];
  if (
    object === undefined ||
    object.spline === null ||
    object.spline.knotPoints[elementIndex] === undefined
  ) {
    return;
  }
  const capturePoint = pointFromPayload(
    payload,
    object.spline.knotPoints[elementIndex],
  );
  object.spline.knotPoints[elementIndex] = capturePoint;
  object.spline.activeKnotIndex = elementIndex;
  object.displayBox = normalizeDragBox(
    object.spline.knotPoints[0] ?? capturePoint,
    object.spline.knotPoints[object.spline.knotPoints.length - 1] ?? capturePoint,
  );
  state.selectedObjectIndex = objectIndex;
  if (phase === "end") {
    setJobSummary(
      state,
      "annotate.workspace.handle_drag committed spline handle drag",
    );
  }
}

function handleBoxDrag(
  state: MockTransportState,
  payload: Record<string, unknown>,
): void {
  const phase = stringFromValue(payload.phase);
  const dragKind = stringFromValue(payload.drag_kind);
  const capturePoint = pointFromPayload(payload, point(0, 0));

  if (phase === "cancel") {
    state.boxDrag = null;
    setJobSummary(state, "annotate.workspace.box_drag cancelled");
    return;
  }

  if (phase === "begin" && (dragKind === "create" || dragKind === "move")) {
    const objectIndex =
      dragKind === "move"
        ? integerFromValue(payload.object_index, state.selectedObjectIndex)
        : null;
    const startBox =
      objectIndex === null || state.objects[objectIndex] === undefined
        ? null
        : structuredClone(state.objects[objectIndex].displayBox);
    state.boxDrag = {
      kind: dragKind,
      objectIndex,
      startPoint: capturePoint,
      startBox,
    };
    return;
  }

  if (state.boxDrag === null || phase !== "end") {
    return;
  }

  if (state.boxDrag.kind === "create") {
    state.objects.push({
      categoryIndex: 0,
      shapeLabel: "Box",
      displayBox: normalizeDragBox(state.boxDrag.startPoint, capturePoint),
      point: null,
      spline: null,
      skeleton: null,
      sup: makeColorRange(),
      nosup: makeColorRange(),
    });
    state.selectedObjectIndex = state.objects.length - 1;
    state.boxDrag = null;
    setJobSummary(state, "annotate.workspace.box_drag created box object");
    return;
  }

  const objectIndex = state.boxDrag.objectIndex;
  if (
    objectIndex === null ||
    state.boxDrag.startBox === null ||
    state.objects[objectIndex] === undefined
  ) {
    state.boxDrag = null;
    return;
  }

  const deltaX = capturePoint.x - state.boxDrag.startPoint.x;
  const deltaY = capturePoint.y - state.boxDrag.startPoint.y;
  const object = state.objects[objectIndex];
  object.displayBox = translateBox(state.boxDrag.startBox, deltaX, deltaY);
  state.selectedObjectIndex = objectIndex;
  state.boxDrag = null;
  setJobSummary(state, "annotate.workspace.box_drag committed box move");
}

function handleColorSample(
  state: MockTransportState,
  payload: Record<string, unknown>,
): void {
  const object = selectedObject(state);
  if (object === null) {
    return;
  }
  const capturePoint = pointFromPayload(payload, point(0, 0));
  object.sup = {
    ...object.sup,
    center: {
      hue_degrees: (capturePoint.x + capturePoint.y) % 360,
      saturation: Math.min(1, Math.max(0, capturePoint.x / kCaptureWidth)),
      value: Math.min(1, Math.max(0, capturePoint.y / kCaptureHeight)),
    },
    sampling: false,
  };
  object.nosup = {
    ...object.nosup,
    sampling: false,
  };
  setJobSummary(
    state,
    "annotate.workspace.color_sample sampled suppress mask color",
  );
}

function handleMaskBrush(
  state: MockTransportState,
  payload: Record<string, unknown>,
): void {
  if (stringFromValue(payload.phase) === "cancel") {
    setJobSummary(state, "annotate.workspace.brush cancelled");
    return;
  }
  const selection = selectedBoxPayloadTarget(state, payload);
  if (selection === null) {
    return;
  }
  const radius = integerFromValue(payload.radius, state.brushRadius);
  selection.object.displayBox = expandBoxAroundPoint(
    selection.object.displayBox,
    selection.capturePoint,
    radius,
  );
  setJobSummary(state, "annotate.workspace.brush updated selected mask");
}

function selectedBoxPayloadTarget(
  state: MockTransportState,
  payload: Record<string, unknown>,
): { object: MockObject; capturePoint: MockPoint } | null {
  const object = selectedBoxObject(state);
  return object === null
    ? null
    : {
        object,
        capturePoint: pointFromPayload(payload, point(0, 0)),
      };
}

function handleMaskFill(
  state: MockTransportState,
  payload: Record<string, unknown>,
): void {
  const selection = selectedBoxPayloadTarget(state, payload);
  if (selection === null) {
    return;
  }
  state.activeTool = stringFromValue(payload.tool, "mask.fill");
  selection.object.displayBox = expandBoxAroundPoint(
    selection.object.displayBox,
    selection.capturePoint,
    18,
  );
  setJobSummary(state, "annotate.workspace.fill filled selected mask");
}

function handleWorkspacePointer(
  state: MockTransportState,
  payload: Record<string, unknown>,
): void {
  const phase = stringFromValue(payload.phase);
  if (phase === "cancel") {
    state.boxDrag = null;
    setJobSummary(state, "annotate.workspace.pointer cancelled interaction");
    return;
  }
  if (payload.drag_kind !== null && payload.drag_kind !== undefined) {
    handleBoxDrag(state, payload);
    return;
  }
  if (payload.handle !== null && payload.handle !== undefined) {
    handleSplineDrag(state, payload);
    return;
  }
  const tool = stringFromValue(payload.tool, state.activeTool);
  if (tool === "mask.paint" || tool === "mask.erase") {
    handleMaskBrush(state, {
      ...payload,
      radius: payload.brush_radius,
    });
    return;
  }
  if (phase !== "end") {
    return;
  }
  if (tool === "mask.fill") {
    handleMaskFill(state, payload);
    return;
  }
  if (tool === "mask.color_sample") {
    handleColorSample(state, payload);
    return;
  }
  handleWorkspaceClick(state, payload);
}

function applySettingsUpdate(
  state: MockTransportState,
  payload: Record<string, unknown>,
): void {
  const patch =
    payload.patch !== null &&
    typeof payload.patch === "object" &&
    !Array.isArray(payload.patch)
      ? (payload.patch as Record<string, unknown>)
      : null;
  const workflows =
    patch?.workflows !== null &&
    typeof patch?.workflows === "object" &&
    !Array.isArray(patch?.workflows)
      ? (patch.workflows as Record<string, unknown>)
      : null;
  const annotate =
    workflows?.annotate !== null &&
    typeof workflows?.annotate === "object" &&
    !Array.isArray(workflows?.annotate)
      ? (workflows.annotate as Record<string, unknown>)
      : null;
  const source =
    annotate?.source !== null &&
    typeof annotate?.source === "object" &&
    !Array.isArray(annotate?.source)
      ? (annotate.source as Record<string, unknown>)
      : null;
  if (source?.kind !== undefined) {
    state.sourceKind = integerFromValue(source.kind, state.sourceKind);
  }
  setJobSummary(state, "settings.update applied mock workflow patch");
}

function applyIntent(
  state: MockTransportState,
  intent: IntentMessage,
): MockTransportState {
  const nextState: MockTransportState = {
    ...state,
    objects: cloneObjects(state.objects),
  };

  switch (intent.intent) {
    case "annotate.setup_frame.next":
      nextState.setupFrameIndex = Math.min(
        nextState.setupFrameIndex + 1,
        kSetupFrameCount - 1,
      );
      setJobSummary(
        nextState,
        `annotate.setup_frame.next moved to frame ${nextState.setupFrameIndex + 1}`,
      );
      break;
    case "annotate.save_now":
      setJobSummary(
        nextState,
        "annotate.save_now captured the current mock frame",
      );
      break;
    case "tool.select":
      nextState.activeTool = stringFromValue(
        intent.payload.tool,
        nextState.activeTool,
      );
      setJobSummary(nextState, `tool.select selected ${nextState.activeTool}`);
      break;
    case "annotate.brush_radius":
      nextState.brushRadius = integerFromValue(
        intent.payload.radius,
        nextState.brushRadius,
      );
      setJobSummary(
        nextState,
        `annotate.brush_radius set radius ${nextState.brushRadius}`,
      );
      break;
    case "settings.update":
      applySettingsUpdate(nextState, intent.payload);
      break;
    case "annotate.live.start":
      nextState.liveAnnotateRunning = true;
      setJobSummary(
        nextState,
        "annotate.live.start entered mock live annotate",
      );
      break;
    case "annotate.live.stop":
      nextState.liveAnnotateRunning = false;
      setJobSummary(
        nextState,
        "annotate.live.stop exited mock live annotate",
      );
      break;
    case "annotate.hold_save":
      nextState.holdSave = booleanFromValue(
        intent.payload.enabled,
        nextState.holdSave,
      );
      setJobSummary(
        nextState,
        nextState.holdSave
          ? "annotate.hold_save armed"
          : "annotate.hold_save disarmed",
      );
      break;
    case "live.preview.fit_to_capture":
      nextState.fitToCapture = booleanFromValue(
        intent.payload.enabled,
        nextState.fitToCapture,
      );
      setJobSummary(
        nextState,
        `live.preview.fit_to_capture ${nextState.fitToCapture ? "enabled" : "disabled"}`,
      );
      break;
    case "live.preview.full_frame_display":
      nextState.fullFrameDisplay = booleanFromValue(
        intent.payload.enabled,
        nextState.fullFrameDisplay,
      );
      setJobSummary(
        nextState,
        `live.preview.full_frame_display ${nextState.fullFrameDisplay ? "enabled" : "disabled"}`,
      );
      break;
    case "predict.start":
      nextState.activeWorkflow = "live";
      nextState.liveMode = "predict";
      nextState.liveControllerRunning = true;
      setJobSummary(nextState, "predict.start entered mock live predict");
      break;
    case "predict.stop":
      nextState.activeWorkflow = "predict";
      nextState.liveMode = "none";
      nextState.liveControllerRunning = false;
      setJobSummary(nextState, "predict.stop exited mock live predict");
      break;
    case "annotate.sidebar":
      handleSidebarIntent(nextState, intent.payload);
      break;
    case "annotate.workspace.click":
      handleWorkspaceClick(nextState, intent.payload);
      break;
    case "annotate.workspace.handle_drag":
      handleSplineDrag(nextState, intent.payload);
      break;
    case "annotate.workspace.box_drag":
      handleBoxDrag(nextState, intent.payload);
      break;
    case "annotate.workspace.color_sample":
      handleColorSample(nextState, intent.payload);
      break;
    case "annotate.workspace.brush":
      handleMaskBrush(nextState, intent.payload);
      break;
    case "annotate.workspace.pointer":
      handleWorkspacePointer(nextState, intent.payload);
      break;
    case "annotate.workspace.fill":
      handleMaskFill(nextState, intent.payload);
      break;
    default:
      setJobSummary(nextState, `intent dispatched: ${intent.intent}`);
      break;
  }

  nextState.stateRevision += 1;
  return nextState;
}

export function createMockBrowserHostTransport(): BrowserHostTransport {
  let state = makeInitialState();
  let snapshot = buildSnapshot(state);
  const listeners = new Set<(nextSnapshot: StateSnapshot) => void>();

  const notify = () => {
    for (const listener of listeners) {
      listener(snapshot);
    }
  };

  return {
    mode: "mock",
    getSnapshot() {
      return snapshot;
    },
    dispatch(intent) {
      assertHostApiIntent(intent);
      state = applyIntent(state, intent);
      snapshot = buildSnapshot(state);
      notify();
    },
    subscribe(listener) {
      listeners.add(listener);
      listener(snapshot);
      return () => {
        listeners.delete(listener);
      };
    },
  };
}
