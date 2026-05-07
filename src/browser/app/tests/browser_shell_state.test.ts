import { test } from "node:test";
import * as assert from "node:assert/strict";

import type { StateSnapshot } from "../src/host_api";
import {
  annotateShellControlsState,
  annotateWorkspaceSceneState,
  annotateToolPaletteState,
  livePreviewControlsState,
  liveStatusState,
  normalizeAnnotateToolId,
  trainLocalGpuState,
} from "../src/browser_shell_state";
import {
  makeAnnotateSidebarFixture,
  makeBrowserTestSnapshot,
  makeBrowserTestSource,
  makeLivePredictRuntimeFixture,
  setAnnotateWorkflowState,
} from "./support/browser-test-fixtures";

function makeSnapshot(): StateSnapshot {
  const snapshot = makeBrowserTestSnapshot({
    state_revision: 7,
    active_workflow: "train",
    workflow_state: {
      selected_preset: "rf-detr-seg-medium",
      workflows: {
        train: {
          training: {
            local_device_ids: [1],
          },
        },
      },
    },
    source: makeBrowserTestSource({
      kind: "video_stream",
      locator: "device:0",
      recursive: false,
      device_index: 0,
    }),
  });
  setAnnotateWorkflowState(snapshot, "select");
  return snapshot;
}

test("trainLocalGpuState derives visible devices, draft selection, and refresh intent", () => {
  const snapshot = makeSnapshot();
  snapshot.workflow_state.train_local_gpu = {
    visible_devices: [
      {
        device_id: 0,
        name: "RTX 4090",
        total_memory_bytes: 24 * 1024 ** 3,
      },
      {
        device_id: 1,
        name: "RTX 4080 SUPER",
        total_memory_bytes: 16 * 1024 ** 3,
      },
    ],
    configured_device_ids: [1],
    selection_summary: "1/2 selected · RTX 4080 SUPER",
    banner: {
      kind: "warning",
      message: "Select at least one local GPU before running training.",
    },
    refresh_supported: true,
    refresh_intent: "train.local_gpu.refresh",
    refresh_workflow: "train",
  };

  const state = trainLocalGpuState(snapshot, [0, 1]);

  assert.equal(state.hasSnapshotState, true);
  assert.equal(state.hasEnumeratedDevices, true);
  assert.deepEqual(state.selectedDeviceIds, [1]);
  assert.deepEqual(state.draftSelectedDeviceIds, [0, 1]);
  assert.equal(state.visibleDevices.length, 2);
  assert.equal(state.visibleDevices[0].selected, true);
  assert.equal(state.visibleDevices[1].selected, true);
  assert.equal(state.refreshSupported, true);
  assert.equal(state.refreshIntent, "train.local_gpu.refresh");
  assert.equal(state.summary, "1/2 selected · RTX 4080 SUPER");
  assert.match(state.warning, /Select at least one local GPU/);
  assert.equal(state.statusItems.find((item) => item.key === "inventory")?.status, "ready");
  assert.equal(state.statusItems.find((item) => item.key === "selection")?.status, "active");
  assert.equal(
    state.statusItems.find((item) => item.key === "selection")?.summary,
    "draft device IDs 0, 1",
  );
  assert.match(
    state.statusItems.find((item) => item.key === "selection")?.detail ?? "",
    /Training will use local device IDs 0, 1/,
  );
});

test("liveStatusState prefers explicit native counters and preview metadata", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "live";
  snapshot.workflow_state.live_runtime = {
    active_mode: "predict",
    show_running_section: true,
    show_static_preview: false,
    video_stream_source: true,
    preview: {
      initialized: true,
      has_frame: true,
      frame_id: 44,
      live_frame_id: {
        session_nonce: 77,
        sequence: 4400,
      },
      displayed_region: {
        x: 0,
        y: 0,
        width: 1920,
        height: 1080,
      },
    },
    controller: {
      present: true,
      running: true,
    },
    analyzer: {
      attached: true,
      model_hot: true,
      running: true,
      frames_analyzed: 512,
      frames_skipped: 12,
      last_latency_ms: 7.8,
      backend_name: "TensorRT",
    },
    compositor: {
      frames_composited: 500,
    },
  };

  const state = liveStatusState(snapshot);

  assert.equal(state.hasSnapshotState, true);
  assert.equal(state.controllerLabel, "running");
  assert.match(state.previewLabel, /Frame 44/);
  assert.match(state.analyzerLabel, /512 analyzed \/ 12 skipped/);
  assert.equal(state.workspacePathLabel, "500 presented");
  assert.equal(state.backendLabel, "TensorRT");
  assert.equal(state.latencyLabel, "7.80 ms");
  assert.equal(state.runtime.activeMode, "predict");
  assert.equal(state.runtime.videoStreamSource, true);
  assert.deepEqual(state.preview.liveFrameId, {
    session_nonce: 77,
    sequence: 4400,
  });
  assert.equal(state.preview.initialized, true);
  assert.equal(state.controller.present, true);
  assert.equal(state.analyzer.attached, true);
  assert.equal(state.workspacePath.framesDropped, 0);
});

test("liveStatusState folds overlay/runtime metadata into the workspace-path view", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "live";
  snapshot.workflow_state.live_runtime = {
    active_mode: "annotate",
    show_running_section: true,
    show_static_preview: false,
    preview: {
      initialized: true,
      has_frame: true,
      frame_id: 91,
      live_frame_id: {
        session_nonce: 1234,
        sequence: 5678,
      },
      displayed_region: {
        x: 0,
        y: 0,
        width: 1600,
        height: 900,
      },
      crop_overlay_mode: true,
      fit_to_capture: false,
      interop_failed: true,
    },
    controller: {
      present: true,
      running: true,
    },
    analyzer: {
      attached: true,
      model_hot: false,
      running: true,
      frames_analyzed: 72,
      frames_skipped: 4,
      last_latency_ms: 12.3,
      backend_name: "TensorRT",
    },
    manual_overlay: {
      running: true,
      generations_rendered: 27,
      last_generation: 31,
      last_error: "manual overlay stale",
    },
    compositor: {
      running: true,
      frames_composited: 68,
      frames_dropped: 7,
      last_frame_id: {
        session_nonce: 1234,
        sequence: 5677,
      },
      manual_overlay_active: true,
      analysis_overlay_active: true,
    },
  };

  const state = liveStatusState(snapshot);

  assert.equal(state.runtime.activeMode, "annotate");
  assert.equal(state.preview.cropOverlayMode, true);
  assert.equal(state.preview.interopFailed, true);
  assert.deepEqual(state.preview.liveFrameId, {
    session_nonce: 1234,
    sequence: 5678,
  });
  assert.equal(state.workspacePath.running, true);
  assert.equal(state.workspacePath.framesPresented, 68);
  assert.equal(state.workspacePath.framesDropped, 7);
  assert.deepEqual(state.workspacePath.lastFrameId, {
    session_nonce: 1234,
    sequence: 5677,
  });
  assert.equal(state.workspacePath.overlayActive, true);
  assert.match(state.previewLabel, /full-frame/);
  assert.match(state.previewLabel, /interop fallback/);
  assert.match(state.workspacePathLabel, /7 dropped/);
  assert.match(state.workspacePathLabel, /overlay active/);
  assert.match(state.errorText, /workspace overlay updates reported an error/);
  assert.equal(
    state.statusItems.find((item) => item.key === "preview")?.status,
    "fallback",
  );
  assert.equal(
    state.statusItems.find((item) => item.key === "workspace_path")?.status,
    "active",
  );
  assert.equal(
    state.statusItems.find((item) => item.key === "workspace_path")?.summary,
    "workspace path 68 / 7 dropped",
  );
  assert.equal(
    state.statusItems.find((item) => item.key === "workspace_path")?.detail,
    "68 presented · 7 dropped · overlay active",
  );
});

test("liveStatusState keeps idle-start flags without inferring preview metadata from unrelated transport fields", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "live";
  snapshot.workflow_state.live_runtime = {
    active_mode: "none",
    show_running_section: false,
    show_static_preview: true,
    show_idle_start_error: true,
    preview: {
      has_frame: true,
      displayed_region: {
        x: 0,
        y: 0,
        width: 800,
        height: 600,
      },
      fit_to_capture: true,
      static_source_name: "demo.png",
    },
    start_error: "device busy",
  };

  const state = liveStatusState(snapshot);

  assert.equal(state.controllerLabel, "idle start failed");
  assert.equal(state.runtime.showIdleStartError, true);
  assert.equal(state.preview.fitToCapture, true);
  assert.equal(state.preview.frameId, 0);
  assert.equal(state.preview.liveFrameId, null);
  assert.match(state.previewLabel, /Static · demo\.png · 800x600/);
  assert.match(state.previewLabel, /fit/);
  assert.match(state.errorText, /device busy/);
  assert.equal(state.statusItems.find((item) => item.key === "session")?.status, "blocked");
  assert.equal(
    state.statusItems.find((item) => item.key === "session")?.summary,
    "idle start failed",
  );
  assert.equal(state.statusItems.find((item) => item.key === "preview")?.status, "ready");
  assert.equal(
    state.statusItems.find((item) => item.key === "workspace_path")?.status,
    "ready",
  );
  assert.equal(
    state.statusItems.find((item) => item.key === "workspace_path")?.summary,
    "workspace path staged",
  );
});

test("liveStatusState carries stage-specific workspace failure context", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "live";
  snapshot.workflow_state.live_runtime = {
    active_mode: "predict",
    show_running_section: true,
    preview: {
      has_frame: false,
      surface_revision: "733",
      owner: "live",
      display_startup_state: "surface published",
      error: "dawn_import_failure: Dawn import failed",
      renderer_last_result_code: "dawn_import_failure",
      renderer_last_result_detail: "Dawn import failed",
      last_failure_reason: "dawn_import_failure",
      last_failure_detail: "Dawn import failed",
      last_failure_revision: "733",
      last_failure_stage: "renderer.import_texture",
      last_failure_live_frame_id: {
        session_nonce: 44,
        sequence: 733,
      },
      last_failure_cuda_device: 2,
      last_failure_result_code: "dawn_import_failure",
      last_failure_result_detail: "Dawn import failed",
      publication_counters: {
        attempted_workspace_acquisitions: 5,
        startup_workspace_acquisition_misses: 2,
        post_startup_workspace_acquisition_misses: 1,
      },
    },
  };

  const state = liveStatusState(snapshot);

  assert.equal(state.preview.lastFailureStage, "renderer.import_texture");
  assert.deepEqual(state.preview.lastFailureLiveFrameId, {
    session_nonce: 44,
    sequence: 733,
  });
  assert.equal(state.preview.lastFailureCudaDevice, 2);
  assert.equal(state.preview.lastFailureResultCode, "dawn_import_failure");
  assert.equal(state.preview.publicationCounters.attemptedWorkspaceAcquisitions, 5);
  assert.equal(state.preview.publicationCounters.startupWorkspaceAcquisitionMisses, 2);
  assert.equal(state.preview.publicationCounters.postStartupWorkspaceAcquisitionMisses, 1);
  assert.equal(state.preview.rendererLastResultDetail, "Dawn import failed");
  assert.match(state.errorText, /Dawn import failed/);
});

test("liveStatusState ignores steady-state runtime state without explicit preview metadata", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "live";
  snapshot.workflow_state.live_runtime = {
    active_mode: "none",
    show_running_section: false,
    show_static_preview: false,
  };

  const state = liveStatusState(snapshot);

  assert.equal(state.preview.hasFrame, false);
  assert.equal(state.preview.frameId, 0);
  assert.equal(state.preview.liveFrameId, null);
  assert.equal(state.workspacePath.lastFrameId, null);
  assert.equal(state.previewLabel, "No preview frame");
  assert.equal(state.workspacePathLabel, "workspace path pending");
  assert.equal(state.statusItems.find((item) => item.key === "preview")?.status, "pending");
  assert.equal(
    state.statusItems.find((item) => item.key === "workspace_path")?.status,
    "pending",
  );
});

test("annotateShellControlsState prefers published control sections and normalizes payload metadata", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "annotate";
  setAnnotateWorkflowState(snapshot, "mask.paint");
  snapshot.workflow_state.annotate_sidebar = makeAnnotateSidebarFixture();
  snapshot.workflow_state.annotate_controls = {
    note: "native annotate control surface",
    setup_frame_navigation: {
      current_index: 3,
      input_count: 9,
      reload: {
        enabled: true,
        intent: "annotate.setup_frame.reload",
      },
      prev: {
        enabled: true,
        workflow: "annotate",
        intent: "annotate.setup_frame.prev",
      },
      next: {
        enabled: false,
        intent: "annotate.setup_frame.next",
      },
    },
    save: {
      summary: "native save controls",
      save_now: {
        enabled: true,
        intent: "annotate.save_now",
      },
      hold_save: {
        enabled: true,
        value: true,
        blocked: false,
        intent: "annotate.hold_save",
        payload_key: "enabled",
      },
    },
    brush: {
      summary: "brush comes from native state",
      visible: true,
      radius: {
        value: 19,
        min: 1,
        max: 64,
        enabled: true,
        intent: "annotate.brush_radius",
        payload_key: "radius",
      },
    },
  };

  const state = annotateShellControlsState(snapshot);

  assert.equal(state.statusItems.find((item) => item.key === "save")?.status, "active");
  assert.equal(state.statusItems.find((item) => item.key === "brush")?.status, "active");
  assert.equal(state.statusItems.find((item) => item.key === "save")?.summary, "hold save armed");
  assert.equal(state.setupFrame.currentIndex, 3);
  assert.equal(state.setupFrame.inputCount, 9);
  assert.equal(state.setupFrame.reload.intent, "annotate.setup_frame.reload");
  assert.equal(state.setupFrame.previous.intent, "annotate.setup_frame.prev");
  assert.equal(state.setupFrame.next.enabled, false);
  assert.equal(state.save.summary, "native save controls");
  assert.equal(state.statusItems.find((item) => item.key === "save")?.detail, "native save controls");
  assert.equal(state.save.saveNow.intent, "annotate.save_now");
  assert.equal(state.save.holdSave.value, true);
  assert.equal(state.save.holdSave.payloadKey, "enabled");
  assert.equal(state.brush.visible, true);
  assert.equal(state.brush.radius.value, 19);
  assert.equal(state.brush.radius.max, 64);
  assert.equal(state.brush.radius.payloadKey, "radius");
});

test("annotateShellControlsState supports flattened save and brush control metadata", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "annotate";
  setAnnotateWorkflowState(snapshot, "mask.erase");
  snapshot.workflow_state.annotate_sidebar = makeAnnotateSidebarFixture();
  snapshot.workflow_state.annotate_controls = {
    save: {
      hold_save_enabled: true,
      hold_save_active: true,
      hold_save_blocked: false,
      hold_save_intent: "annotate.hold_save",
      hold_save_payload_key: "enabled",
    },
    brush: {
      visible: true,
      radius_value: 23,
      radius_min: 2,
      radius_max: 40,
      radius_enabled: true,
      radius_intent: "annotate.brush_radius",
      radius_payload_key: "radius",
    },
  };

  const state = annotateShellControlsState(snapshot);

  assert.equal(state.save.holdSave.value, true);
  assert.equal(state.save.holdSave.intent, "annotate.hold_save");
  assert.equal(state.save.holdSave.payloadKey, "enabled");
  assert.equal(state.brush.visible, true);
  assert.equal(state.brush.radius.value, 23);
  assert.equal(state.brush.radius.min, 2);
  assert.equal(state.brush.radius.max, 40);
  assert.equal(state.brush.radius.intent, "annotate.brush_radius");
  assert.equal(state.brush.radius.payloadKey, "radius");
  assert.equal(state.statusItems.find((item) => item.key === "save")?.status, "active");
  assert.equal(state.statusItems.find((item) => item.key === "brush")?.status, "active");
});

test("livePreviewControlsState falls back to live runtime preview booleans and supports published intents", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "live";
  snapshot.workflow_state.live_runtime = makeLivePredictRuntimeFixture({
    preview: {
      frame_id: 12,
      fit_to_capture: true,
      crop_overlay_mode: false,
    },
  });
  snapshot.workflow_state.live_preview_controls = {
    note: "native live preview controls",
    fit_to_capture: {
      value: true,
      enabled: true,
      intent: "live.preview.fit_to_capture",
      payload_key: "enabled",
    },
    full_frame_display: {
      value: false,
      enabled: true,
      intent: "live.preview.full_frame_display",
      payload_key: "enabled",
    },
  };

  const state = livePreviewControlsState(snapshot);

  assert.equal(state.statusItems.find((item) => item.key === "display")?.status, "ready");
  assert.equal(state.statusItems.find((item) => item.key === "fit_to_capture")?.status, "active");
  assert.equal(
    state.statusItems.find((item) => item.key === "full_frame_display")?.status,
    "ready",
  );
  assert.equal(
    state.statusItems.find((item) => item.key === "fit_to_capture")?.summary,
    "fit to capture enabled",
  );
  assert.equal(state.fitToCapture.value, true);
  assert.equal(state.fitToCapture.intent, "live.preview.fit_to_capture");
  assert.equal(state.fitToCapture.payloadKey, "enabled");
  assert.equal(state.fullFrameDisplay.value, false);
  assert.equal(state.fullFrameDisplay.intent, "live.preview.full_frame_display");
});

test("livePreviewControlsState consumes native capability contract summaries when published", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "live";
  snapshot.workflow_state.live_runtime = makeLivePredictRuntimeFixture({
    preview: {
      frame_id: 18,
    },
  });
  snapshot.workflow_state.live_preview_controls = {
    capabilities: {
      display: {
        status: "ready",
        summary: "native display contract ready",
      },
      fit_to_capture: {
        status: "active",
        summary: "native fit contract active",
      },
      full_frame_display: {
        status: "blocked",
        summary: "native full-frame contract blocked",
      },
    },
    fit_to_capture: {
      value: true,
      enabled: true,
    },
    full_frame_display: {
      value: false,
      enabled: false,
    },
  };

  const state = livePreviewControlsState(snapshot);

  assert.equal(
    state.statusItems.find((item) => item.key === "display")?.summary,
    "native display contract ready",
  );
  assert.equal(
    state.statusItems.find((item) => item.key === "fit_to_capture")?.summary,
    "native fit contract active",
  );
  assert.equal(
    state.statusItems.find((item) => item.key === "full_frame_display")?.status,
    "blocked",
  );
  assert.equal(
    state.statusItems.find((item) => item.key === "full_frame_display")?.summary,
    "native full-frame contract blocked",
  );
});

test("annotateToolPaletteState normalizes active tools and disables mask tools without mask selection", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "annotate";
  const workflows = snapshot.workflow_state.workflows as Record<string, unknown>;
  snapshot.workflow_state.workflows = {
    ...workflows,
    annotate: {
      annotate: {
        active_tool: "mask_erase",
      },
    },
  };
  snapshot.workflow_state.annotate_sidebar = makeAnnotateSidebarFixture({
    assist_available: false,
    can_undo: false,
    can_delete_selected: false,
    can_redraw_selected_box: false,
    can_edit_selected_mask: false,
    has_selected_object: false,
    selected_object_index: null,
    categories: [],
    selected_object: null,
  });

  const state = annotateToolPaletteState(snapshot, "select");
  const erase = state.options.find((option) => option.id === "mask.erase");
  const box = state.options.find((option) => option.id === "box");

  assert.equal(normalizeAnnotateToolId("mask_paint"), "mask.paint");
  assert.equal(state.activeTool, "mask.erase");
  assert.equal(erase?.active, true);
  assert.equal(erase?.disabled, true);
  assert.equal(box?.disabled, false);
  assert.equal(state.statusItems.find((item) => item.key === "mask_tools")?.status, "blocked");
  assert.equal(state.statusItems.find((item) => item.key === "create_tools")?.status, "ready");
  assert.equal(
    state.statusItems.find((item) => item.key === "mask_tools")?.summary,
    "mask tools blocked until selection",
  );
  assert.equal(
    state.statusItems.find((item) => item.key === "mask_tools")?.detail,
    "Select a mask-capable object to enable Paint, Erase, and Fill.",
  );
});

test("annotateWorkspaceSceneState parses visible objects and editable handles", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "annotate";
  snapshot.workflow_state.annotate_workspace_scene = {
    document_generation: 44,
    selected_object_index: 2,
    capture_width: 1280,
    capture_height: 720,
    visible_objects: [
      {
        index: 1,
        category_index: 3,
        shape_type: "spline",
        capture_box: {
          x1: 180,
          y1: 96,
          x2: 420,
          y2: 336,
        },
        fully_visible: true,
        geometry: {
          capture_points: [
            { x: 180, y: 160 },
            { x: 260, y: 240 },
            { x: 420, y: 188 },
          ],
          edges: [],
          closed: false,
        },
      },
    ],
    editable_handles: [
      {
        object_index: 1,
        element_index: 1,
        role: "spline_knot",
        category_index: 3,
        capture_point: {
          x: 260,
          y: 240,
        },
        tether_capture_point: null,
        materialized: true,
      },
    ],
    visible_dense_masks: [
      {
        object_index: 4,
        category_index: 5,
        capture_box: {
          x1: 96,
          y1: 48,
          x2: 132,
          y2: 92,
        },
        mask_rle_encoding: "row_major_start_length",
        mask_rle: "0:4 40:2",
      },
    ],
    selected_dense_mask: {
      object_index: 2,
      category_index: 3,
      capture_box: {
        x1: 196,
        y1: 128,
        x2: 264,
        y2: 244,
      },
      mask_rle_encoding: "row_major_start_length",
      mask_rle: "0:2 68:1",
    },
    brush_preview: {
      visible: true,
      capture_x: 300,
      capture_y: 220,
      radius: 18,
      erase: true,
    },
  };

  const state = annotateWorkspaceSceneState(snapshot);

  if (state === null) {
    assert.fail("expected annotate workspace scene state");
  }
  assert.equal(state.documentGeneration, 44);
  assert.equal(state.selectedObjectIndex, 2);
  assert.equal(state.captureWidth, 1280);
  assert.equal(state.captureHeight, 720);
  assert.equal(state.visibleObjects.length, 1);
  assert.equal(state.visibleObjects[0].shapeType, "spline");
  assert.deepEqual(state.visibleObjects[0].captureBox, {
    x1: 180,
    y1: 96,
    x2: 420,
    y2: 336,
  });
  assert.deepEqual(state.visibleObjects[0].geometry.capturePoints[1], {
    x: 260,
    y: 240,
  });
  assert.equal(state.editableHandles.length, 1);
  assert.equal(state.editableHandles[0].role, "spline_knot");
  assert.equal(state.editableHandles[0].objectIndex, 1);
  assert.equal(state.editableHandles[0].tetherCapturePoint, null);
  assert.deepEqual(state.visibleDenseMasks, [
    {
      objectIndex: 4,
      categoryIndex: 5,
      captureBox: {
        x1: 96,
        y1: 48,
        x2: 132,
        y2: 92,
      },
      maskRle: "0:4 40:2",
    },
  ]);
  assert.deepEqual(state.selectedDenseMask, {
    objectIndex: 2,
    categoryIndex: 3,
    captureBox: {
      x1: 196,
      y1: 128,
      x2: 264,
      y2: 244,
    },
    maskRle: "0:2 68:1",
  });
  assert.deepEqual(state.brushPreview, {
    visible: true,
    capturePoint: {
      x: 300,
      y: 220,
    },
    radius: 18,
    erase: true,
  });
});
