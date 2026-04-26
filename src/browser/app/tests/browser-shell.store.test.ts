import * as assert from "node:assert/strict";
import { test } from "node:test";

import { type StaticProvider } from "@angular/core";

import { BrowserAnnotateWorkflowState } from "../src/app/state/browser-annotate-workflow.service";
import { BrowserExportWorkflowState } from "../src/app/state/browser-export-workflow.service";
import { BrowserHostRuntimeState } from "../src/app/state/browser-host-runtime.service";
import { BrowserLiveWorkflowState } from "../src/app/state/browser-live-workflow.service";
import { BrowserPredictWorkflowState } from "../src/app/state/browser-predict-workflow.service";
import { BrowserShellSettingsState } from "../src/app/state/browser-shell-settings.service";
import { BrowserShellStore } from "../src/app/state/browser-shell.store";
import { BrowserSourceState } from "../src/app/state/browser-source-state.service";
import { BrowserTrainWorkflowState } from "../src/app/state/browser-train-workflow.service";
import { BrowserValidateWorkflowState } from "../src/app/state/browser-validate-workflow.service";
import { BrowserWorkflowDraftSharedState } from "../src/app/state/browser-workflow-draft-shared.service";
import { BrowserWorkflowDraftsState } from "../src/app/state/browser-workflow-drafts.service";
import { BrowserWorkflowRouteState } from "../src/app/state/browser-workflow-route.service";
import { BrowserWorkspaceStateService } from "../src/app/state/browser-workspace.service";
import type { BrowserHostTransport, StateSnapshot } from "../src/host_api";
import {
  createAngularServiceHarness,
  type AngularServiceHarness,
} from "./support/angular-test-harness";
import {
  makeBrowserTestAnnotation,
  makeAnnotateSidebarFixture,
  makeBrowserTestSnapshot,
  makeBrowserTestSource,
  makeLivePredictRuntimeFixture,
  makeNativeBridgeContractCapabilities,
  setAnnotateWorkflowState,
} from "./support/browser-test-fixtures";
import { assertSemanticViewportCommitPayload } from "./support/browser-state-assertions";

function makeSnapshot(): StateSnapshot {
  const snapshot = makeBrowserTestSnapshot({
    state_revision: 11,
    active_workflow: "annotate",
    workflow_state: {
      annotate_sidebar: makeAnnotateSidebarFixture(),
      annotate_controls: {
        setup_frame_navigation: {
          reload: {
            enabled: true,
            intent: "annotate.frame.reload",
          },
          prev: {
            enabled: true,
            intent: "annotate.frame.prev",
          },
          next: {
            enabled: true,
            intent: "annotate.frame.next",
          },
        },
        live_annotate: {
          start: {
            enabled: true,
            intent: "annotate.live.start",
          },
          stop: {
            enabled: true,
            intent: "annotate.live.stop",
          },
        },
        save: {
          save_now: {
            enabled: true,
            intent: "annotate.save_now",
          },
          hold_save: {
            enabled: true,
            value: false,
            intent: "annotate.save.hold",
            payload_key: "armed",
          },
        },
        brush: {
          visible: true,
          radius: {
            value: 17,
            min: 1,
            max: 64,
            enabled: true,
            intent: "annotate.brush.set",
            payload_key: "size",
          },
        },
      },
      live_runtime: makeLivePredictRuntimeFixture({
        preview: {
          displayed_region: {
            x: 0,
            y: 0,
            width: 1280,
            height: 720,
          },
        },
      }),
      live_preview_controls: {
        fit_to_capture: {
          value: false,
          enabled: true,
          intent: "live.preview.fit",
          payload_key: "value",
        },
        full_frame_display: {
          value: false,
          enabled: true,
          intent: "live.preview.overlay",
          payload_key: "value",
        },
      },
    },
    source: makeBrowserTestSource({
      kind: "image_folder",
      locator: "/datasets/annotate",
      recursive: true,
    }),
    annotation: makeBrowserTestAnnotation({
      document_generation: 1,
      session_revision: 2,
      capture_width: 1280,
      capture_height: 720,
      instance_count: 1,
      selected_instance: 0,
    }),
  });
  setAnnotateWorkflowState(snapshot, "mask.paint");
  return snapshot;
}

function createStore(snapshot: StateSnapshot): {
  store: BrowserShellStore;
  transport: AngularServiceHarness["transport"];
  destroy: () => void;
}

function createStore(
  snapshot: StateSnapshot,
  mode: BrowserHostTransport["mode"],
): {
  store: BrowserShellStore;
  transport: AngularServiceHarness["transport"];
  destroy: () => void;
}

function createStore(
  snapshot: StateSnapshot,
  mode: BrowserHostTransport["mode"] = "mock",
): {
  store: BrowserShellStore;
  transport: AngularServiceHarness["transport"];
  destroy: () => void;
} {
  const providers: StaticProvider[] = [
    { provide: BrowserHostRuntimeState, useFactory: () => new BrowserHostRuntimeState() },
    { provide: BrowserWorkflowRouteState, useFactory: () => new BrowserWorkflowRouteState() },
    { provide: BrowserSourceState, useFactory: () => new BrowserSourceState() },
    { provide: BrowserTrainWorkflowState, useFactory: () => new BrowserTrainWorkflowState() },
    {
      provide: BrowserValidateWorkflowState,
      useFactory: () => new BrowserValidateWorkflowState(),
    },
    {
      provide: BrowserPredictWorkflowState,
      useFactory: () => new BrowserPredictWorkflowState(),
    },
    {
      provide: BrowserAnnotateWorkflowState,
      useFactory: () => new BrowserAnnotateWorkflowState(),
    },
    {
      provide: BrowserExportWorkflowState,
      useFactory: () => new BrowserExportWorkflowState(),
    },
    {
      provide: BrowserLiveWorkflowState,
      useFactory: () => new BrowserLiveWorkflowState(),
    },
    {
      provide: BrowserShellSettingsState,
      useFactory: () => new BrowserShellSettingsState(),
    },
    {
      provide: BrowserWorkflowDraftSharedState,
      useFactory: () => new BrowserWorkflowDraftSharedState(),
    },
    {
      provide: BrowserWorkflowDraftsState,
      useFactory: () => new BrowserWorkflowDraftsState(),
    },
    {
      provide: BrowserWorkspaceStateService,
      useFactory: () => new BrowserWorkspaceStateService(),
    },
  ];
  const harness = createAngularServiceHarness(snapshot, providers, { mode });
  const store = harness.run(() => new BrowserShellStore());
  harness.flush();
  return {
    store,
    transport: harness.transport,
    destroy: harness.destroy,
  };
}

function createNativeStore(snapshot = makeSnapshot()): {
  snapshot: StateSnapshot;
  store: BrowserShellStore;
  transport: AngularServiceHarness["transport"];
  destroy: () => void;
} {
  return {
    snapshot,
    ...createStore(snapshot, "native"),
  };
}

function publishRuntimeCapabilities(
  transport: AngularServiceHarness["transport"],
  snapshot: StateSnapshot,
  runtimeCapabilities: NonNullable<
    ReturnType<AngularServiceHarness["transport"]["getBridgeState"]>["runtimeCapabilities"]
  >,
): void {
  transport.publishBridgeState({
    phase: "idle",
    connected: true,
    lastError: "",
    lastSuccessRevision: snapshot.state_revision,
    runtimeCapabilities,
  });
}

test("BrowserShellStore dispatches annotate and live shell controls from published state", () => {
  const { store, transport, destroy } = createStore(makeSnapshot());

  try {
    store.reloadAnnotateSetupFrame();
    store.previousAnnotateSetupFrame();
    store.nextAnnotateSetupFrame();
    store.startAnnotateLive();
    store.stopAnnotateLive();
    store.requestAnnotateSaveNow();
    store.setAnnotateHoldSave(true);
    store.setAnnotateBrushRadius("31");
    store.setLivePreviewFitToCapture(true);
    store.setLivePreviewFullFrameDisplay(true);

    assert.deepEqual(
      transport.dispatched.map((intent) => ({
        workflow: intent.workflow,
        intent: intent.intent,
        payload: intent.payload,
      })),
      [
        { workflow: "annotate", intent: "annotate.frame.reload", payload: {} },
        { workflow: "annotate", intent: "annotate.frame.prev", payload: {} },
        { workflow: "annotate", intent: "annotate.frame.next", payload: {} },
        { workflow: "annotate", intent: "annotate.live.start", payload: {} },
        { workflow: "annotate", intent: "annotate.live.stop", payload: {} },
        { workflow: "annotate", intent: "annotate.save_now", payload: {} },
        {
          workflow: "annotate",
          intent: "annotate.save.hold",
          payload: { armed: true },
        },
        {
          workflow: "annotate",
          intent: "annotate.brush.set",
          payload: { size: 31 },
        },
        { workflow: "live", intent: "live.preview.fit", payload: { value: true } },
        {
          workflow: "live",
          intent: "live.preview.overlay",
          payload: { value: true },
        },
      ],
    );
  } finally {
    destroy();
  }
});

test("BrowserShellStore toggles the live primary action to predict.stop while live predict is active", () => {
  const snapshot = makeSnapshot();
  snapshot.active_workflow = "live";
  snapshot.source.kind = "video_stream";
  snapshot.workflow_state.live_runtime = makeLivePredictRuntimeFixture();

  const { store, transport, destroy } = createStore(snapshot);

  try {
    assert.equal(store.primaryActionLabel(), "Stop Live Predict");
    store.runPrimaryAction();

    assert.deepEqual(
      transport.dispatched.map((intent) => ({
        workflow: intent.workflow,
        intent: intent.intent,
        payload: intent.payload,
      })),
      [{ workflow: "live", intent: "predict.stop", payload: {} }],
    );
  } finally {
    destroy();
  }
});

test("BrowserShellStore applies a later transport snapshot after starting from a placeholder cache", () => {
  const placeholder = makeSnapshot();
  placeholder.state_revision = 0;
  placeholder.active_workflow = "predict";
  placeholder.job.label = "connecting";
  placeholder.job.summary = "Waiting for native bridge";
  placeholder.source.locator = "";

  const { store, transport, destroy } = createStore(placeholder);

  try {
    assert.equal(store.snapshot().state_revision, 0);
    assert.equal(store.workflow(), "predict");

    const publishedSnapshot = makeSnapshot();
    transport.publish(publishedSnapshot);

    assert.equal(store.snapshot().state_revision, publishedSnapshot.state_revision);
    assert.equal(store.workflow(), "annotate");
    assert.equal(store.annotateSidebar()?.hasAnnotationFrame, true);
  } finally {
    destroy();
  }
});

test("BrowserShellStore keeps mock workspace diagnostics centered on the single-canvas path", () => {
  const snapshot = makeSnapshot();
  const { store, destroy } = createStore(snapshot);

  try {
    assert.match(
      store.workspaceDiagnostics().detail,
      /frame path mock workspace pending/,
    );
    assert.match(store.workspaceCanvasChrome().statusLabel, /canvas pending/);
    assert.doesNotMatch(store.workspaceCanvasChrome().statusLabel, /source \+ workspace/);
    assert.equal(
      store.workspaceDiagnostics().statusItems.find((item) => item.key === "overlay_path")?.status,
      "pending",
    );
  } finally {
    destroy();
  }
});

test("BrowserShellStore surfaces native bridge errors through the shell status copy", () => {
  const snapshot = makeSnapshot();
  const { store, transport, destroy } = createStore(snapshot, "native");

  try {
    transport.publishBridgeState({
      phase: "dispatch",
      connected: false,
      lastError: "browser annotate.save blocked",
      lastSuccessRevision: snapshot.state_revision,
    });

    assert.equal(store.transportLabel(), "native bridge · error");
    assert.equal(
      store.transportStatus().statusItems.find((item) => item.key === "transport")?.status,
      "blocked",
    );
    assert.match(
      store.workspaceDiagnostics().detail,
      /bridge blocked: browser annotate\.save blocked/,
    );
    assert.match(
      store.intentSummary(),
      /bridge blocked: browser annotate\.save blocked/,
    );
  } finally {
    destroy();
  }
});

test("BrowserShellStore keeps native import diagnostics stable when bridge runtime updates arrive", () => {
  const snapshot = makeSnapshot();
  snapshot.runtime_capabilities = {
    host_backend: "cef",
    navigator_gpu: "available",
    workspace_surface_bridge: "unknown",
    workspace_surface_zero_copy: "unknown",
  };
  const { store, transport, destroy } = createNativeStore(snapshot);

  try {
    transport.publishBridgeState({
      phase: "idle",
      connected: true,
      lastError: "",
      lastSuccessRevision: snapshot.state_revision,
      runtimeCapabilities: {
        ...snapshot.runtime_capabilities,
        workspace_surface_bridge: "available",
        workspace_surface_zero_copy: "available",
      },
    });

    assert.equal(store.transportStatus().readbackLabel, "workspace surface");
    assert.doesNotMatch(store.workspaceDiagnostics().detail, /compatibility|frame import/i);
  } finally {
    destroy();
  }
});

test("BrowserShellStore keeps transport diagnostics centered on the workspace canvas when runtime capability updates arrive", () => {
  const { snapshot, store, transport, destroy } = createNativeStore();

  try {
    publishRuntimeCapabilities(transport, snapshot, {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "available",
    });

    assert.equal(store.workspaceDiagnostics().framePathLabel, "workspace canvas pending");
    assert.equal(
      store.transportStatus().statusItems.some(
        (item) =>
          item.key === "browser_frame_import" || item.key === "frame_import_hook",
      ),
      false,
    );
    assert.doesNotMatch(store.transportStatus().detail, /frame import/i);
  } finally {
    destroy();
  }
});

test("BrowserShellStore omits legacy import-hook language from shell intent diagnostics", () => {
  const { snapshot, store, transport, destroy } = createNativeStore();

  try {
    publishRuntimeCapabilities(transport, snapshot, {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "unavailable",
    });

    assert.equal(store.workspaceDiagnostics().framePathLabel, "workspace canvas pending");
    assert.equal(
      store.workspaceDiagnostics().statusItems.find((item) => item.key === "frame_path")
        ?.status,
      "pending",
    );
    assert.equal(
      store.workspaceCanvasChrome().fallbackLabel,
      "Workspace canvas pending",
    );
    assert.equal(
      store.transportStatus().statusItems.some(
        (item) =>
          item.key === "browser_frame_import" || item.key === "frame_import_hook",
      ),
      false,
    );
    assert.doesNotMatch(store.intentSummary(), /frame import/i);
  } finally {
    destroy();
  }
});

test("BrowserShellStore keeps viewport commits semantic and omits stale browser-only payloads", () => {
  const { store, transport, destroy } = createNativeStore();

  try {
    assert.equal(store.commitViewport(), true);
    const dispatched = transport.dispatched[0];
    assert.equal(dispatched?.intent, "viewport.commit");
    assertSemanticViewportCommitPayload(dispatched?.payload);
  } finally {
    destroy();
  }
});

test("BrowserShellStore consumes bridge capability contract summaries when published", () => {
  const { snapshot, store, transport, destroy } = createNativeStore();

  try {
    transport.publishBridgeState({
      phase: "idle",
      connected: true,
      lastError: "",
      lastSuccessRevision: snapshot.state_revision,
      capabilities: makeNativeBridgeContractCapabilities(),
    });

    assert.equal(
      store.transportStatus().statusItems.find((item) => item.key === "transport")?.summary,
      "native bridge contract ready",
    );
    assert.equal(
      store.transportStatus().statusItems.find(
        (item) => item.key === "workspace_surface_zero_copy",
      )
        ?.summary,
      "workspace zero-copy ready",
    );
    assert.match(store.intentSummary(), /workspace zero-copy ready/);
  } finally {
    destroy();
  }
});
