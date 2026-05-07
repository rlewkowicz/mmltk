import * as assert from "node:assert/strict";
import { test } from "node:test";

import { inject, runInInjectionContext } from "@angular/core";

import { BrowserAnnotateSidebarState } from "../src/app/state/browser-annotate-sidebar.service";
import { BrowserAnnotateWorkflowState } from "../src/app/state/browser-annotate-workflow.service";
import { BrowserExportWorkflowState } from "../src/app/state/browser-export-workflow.service";
import { BrowserLiveWorkflowState } from "../src/app/state/browser-live-workflow.service";
import { BrowserModelConfigState } from "../src/app/state/browser-model-config.service";
import { BrowserPredictWorkflowState } from "../src/app/state/browser-predict-workflow.service";
import { BrowserShellChromeState } from "../src/app/state/browser-shell-chrome.service";
import { BrowserSourceState } from "../src/app/state/browser-source-state.service";
import { BrowserWorkflowPrimaryActionState } from "../src/app/state/browser-workflow-primary-action.service";
import { BrowserWorkflowPanelsState } from "../src/app/state/browser-workflow-panels.service";
import { BrowserWorkflowRouteState } from "../src/app/state/browser-workflow-route.service";
import { BrowserValidateWorkflowState } from "../src/app/state/browser-validate-workflow.service";
import type { StateSnapshot } from "../src/host_api";
import { RF_DETR_PRESET_OPTIONS } from "../src/workflow_contract.generated";
import {
  createAngularServiceHarness,
  type SpyTransport,
} from "./support/angular-test-harness";
import {
  makeAnnotateSidebarFixture,
  makeBrowserWorkflowServiceProviders,
  makeBrowserTestSnapshot,
  makeBrowserTestSource,
  makeLivePredictRuntimeFixture,
} from "./support/browser-test-fixtures";

function createInjector(snapshot: StateSnapshot): {
  injector: ReturnType<typeof createAngularServiceHarness>["injector"];
  transport: SpyTransport;
} {
  const harness = createAngularServiceHarness(
    snapshot,
    makeBrowserWorkflowServiceProviders({ annotateSidebar: true }),
  );
  return { injector: harness.injector, transport: harness.transport };
}

test("BrowserWorkflowPanelsState follows the route workflow and composes export status from split services", () => {
  const { injector } = createInjector(
    makeBrowserTestSnapshot({
      active_workflow: "predict",
      workflow_state: {
        export: {
          onnx_path: "/tmp/model.onnx",
        },
      },
    }),
  );

  runInInjectionContext(injector, () => {
    const route = inject(BrowserWorkflowRouteState);
    const panels = inject(BrowserWorkflowPanelsState);
    const exportWorkflow = inject(BrowserExportWorkflowState);

    route.routeWorkflow.set("export");
    exportWorkflow.updateTextField("outputPath", "/tmp/export.engine");

    assert.equal(panels.selectedWorkflow(), "export");
    assert.equal(panels.workflowControlStatus()[0]?.label, "Export");
    assert.equal(panels.workflowDetailsStatus()[0]?.label, "Export Details");
  });
});

test("BrowserShellChromeState updates workflow selection immediately and coalesces duplicate view intents", () => {
  const { injector, transport } = createInjector(
    makeBrowserTestSnapshot({
      active_workflow: "predict",
    }),
  );

  runInInjectionContext(injector, () => {
    const chrome = inject(BrowserShellChromeState);
    const route = inject(BrowserWorkflowRouteState);

    chrome.activateWorkflow("train");
    assert.equal(route.selectedWorkflow(), "train");
    chrome.activateWorkflow("train");
  });

  assert.equal(transport.dispatched.length, 1);
  assert.equal(transport.dispatched[0]?.workflow, "train");
  assert.equal(transport.dispatched[0]?.intent, "settings.update");
  assert.deepEqual(transport.dispatched[0]?.payload, {
    patch: {
      current_view: "train",
    },
  });
});

test("BrowserModelConfigState exposes generated RF-DETR presets before backend hydration", () => {
  const { injector, transport } = createInjector(
    makeBrowserTestSnapshot({
      active_workflow: "annotate",
      workflow_state: {},
    }),
  );

  runInInjectionContext(injector, () => {
    const route = inject(BrowserWorkflowRouteState);
    const model = inject(BrowserModelConfigState);
    route.routeWorkflow.set("annotate");

    assert.deepEqual(
      model.presets().map((preset) => preset.presetName),
      RF_DETR_PRESET_OPTIONS.map((preset) => preset.presetName),
    );

    model.setPreset("rf-detr-seg-xxlarge");
    assert.equal(model.selectedPreset(), "rf-detr-seg-xxlarge");
  });

  assert.equal(transport.dispatched.length, 1);
  assert.equal(transport.dispatched[0]?.workflow, "annotate");
  assert.equal(transport.dispatched[0]?.intent, "settings.update");
  assert.deepEqual(transport.dispatched[0]?.payload, {
    patch: {
      selected_preset: "rf-detr-seg-xxlarge",
    },
  });
});

test("BrowserValidateWorkflowState applies dirty settings before browse actions", () => {
  const { injector, transport } = createInjector(
    makeBrowserTestSnapshot({
      active_workflow: "validate",
    }),
  );

  runInInjectionContext(injector, () => {
    const validate = inject(BrowserValidateWorkflowState);
    validate.updateTextField("compiledPath", "/datasets/next.bin");
    validate.browseField("onnxPath");
  });

  assert.equal(transport.dispatched.length, 2);
  assert.equal(transport.dispatched[0]?.intent, "settings.update");
  assert.equal(transport.dispatched[0]?.workflow, "validate");
  assert.equal(transport.dispatched[1]?.intent, "file_dialog.request");
  assert.equal(transport.dispatched[1]?.workflow, "validate");
});

test("BrowserWorkflowPrimaryActionState dispatches predict.stop for an active live predict session", () => {
  const { injector, transport } = createInjector(
    makeBrowserTestSnapshot({
      active_workflow: "live",
      source: makeBrowserTestSource({
        kind: "video_stream",
        locator: "/dev/video0",
        recursive: false,
      }),
      workflow_state: {
        live_runtime: makeLivePredictRuntimeFixture(),
      },
    }),
  );

  runInInjectionContext(injector, () => {
    const primary = inject(BrowserWorkflowPrimaryActionState);
    assert.equal(primary.primaryActionLabel(), "Stop Capture");
    primary.runPrimaryAction();
  });

  assert.equal(transport.dispatched.length, 1);
  assert.equal(transport.dispatched[0]?.workflow, "live");
  assert.equal(transport.dispatched[0]?.intent, "predict.stop");
});

test("BrowserWorkflowPrimaryActionState starts live capture with current source and predict drafts", () => {
  const { injector, transport } = createInjector(
    makeBrowserTestSnapshot({
      active_workflow: "live",
      source: makeBrowserTestSource({
        kind: "single_image",
        locator: "/tmp/stale.png",
      }),
    }),
  );

  runInInjectionContext(injector, () => {
    const source = inject(BrowserSourceState);
    const predict = inject(BrowserPredictWorkflowState);
    const primary = inject(BrowserWorkflowPrimaryActionState);

    source.patch((current) => ({
      ...current,
      kind: "video_stream",
      locator: "device:2",
      deviceIndex: "2",
      captureWidth: "1280",
      captureHeight: "720",
      captureFps: "60",
    }));
    predict.updateTextField("weightsPath", "/models/live.pt");

    assert.equal(primary.primaryActionLabel(), "Start Capture");
    primary.runPrimaryAction();
  });

  assert.equal(transport.dispatched.length, 3);
  assert.equal(transport.dispatched[0]?.workflow, "live");
  assert.equal(transport.dispatched[0]?.intent, "settings.update");
  assert.equal(
    (transport.dispatched[0]?.payload.patch as any).workflows.predict.source.kind,
    3,
  );
  assert.equal(
    (transport.dispatched[0]?.payload.patch as any).workflows.predict.source.device_index,
    2,
  );
  assert.equal(transport.dispatched[1]?.workflow, "live");
  assert.equal(transport.dispatched[1]?.intent, "settings.update");
  assert.equal(
    (transport.dispatched[1]?.payload.patch as any).workflows.predict.model_artifacts.weights_path,
    "/models/live.pt",
  );
  assert.equal(transport.dispatched[2]?.workflow, "live");
  assert.equal(transport.dispatched[2]?.intent, "predict.start");
});

test("BrowserAnnotateWorkflowState can start live annotate from a video draft without a visible apply step", () => {
  const { injector, transport } = createInjector(
    makeBrowserTestSnapshot({
      active_workflow: "annotate",
      source: makeBrowserTestSource({
        kind: "single_image",
        locator: "/datasets/frame.png",
      }),
    }),
  );

  runInInjectionContext(injector, () => {
    const source = inject(BrowserSourceState);
    const annotate = inject(BrowserAnnotateWorkflowState);

    source.patch((current) => ({
      ...current,
      kind: "video_stream",
      locator: "device:1",
      deviceIndex: "1",
      captureWidth: "1280",
      captureHeight: "720",
      captureFps: "60",
      v4l2BufferCount: "4",
    }));
    annotate.updateTextField("outputDir", "/tmp/annotated-live");

    assert.equal(annotate.annotateLiveVisible(), true);
    assert.equal(annotate.annotateLiveStartEnabled(), true);
    annotate.startLive();
  });

  assert.equal(transport.dispatched.length, 3);
  assert.equal(transport.dispatched[0]?.workflow, "annotate");
  assert.equal(transport.dispatched[0]?.intent, "settings.update");
  const sourcePatch =
    (transport.dispatched[0]?.payload.patch as any).workflows.annotate.source;
  assert.equal("compiled_path" in sourcePatch, false);
  assert.equal(
    sourcePatch.kind,
    3,
  );
  assert.equal(
    sourcePatch.device_index,
    1,
  );
  assert.equal(
    sourcePatch.capture_width,
    1280,
  );
  assert.equal(
    sourcePatch.capture_height,
    720,
  );
  assert.equal(
    sourcePatch.capture_fps,
    60,
  );
  assert.equal(
    sourcePatch.v4l2_buffer_count,
    4,
  );
  assert.equal(transport.dispatched[1]?.workflow, "annotate");
  assert.equal(transport.dispatched[1]?.intent, "settings.update");
  assert.equal(
    (transport.dispatched[1]?.payload.patch as any).workflows.annotate.annotate.output_dir,
    "/tmp/annotated-live",
  );
  assert.equal(
    (transport.dispatched[1]?.payload.patch as any).workflows.annotate.annotate.model_input,
    3,
  );
  assert.equal(transport.dispatched[2]?.workflow, "annotate");
  assert.equal(transport.dispatched[2]?.intent, "annotate.live.start");
});

test("BrowserAnnotateSidebarState routes sidebar and workspace annotate commands without BrowserShellStore", () => {
  const { injector, transport } = createInjector(
    makeBrowserTestSnapshot({
      active_workflow: "annotate",
      workflow_state: {
        annotate_sidebar: makeAnnotateSidebarFixture(),
      },
    }),
  );

  runInInjectionContext(injector, () => {
    const annotate = inject(BrowserAnnotateSidebarState);
    annotate.addCategory("wheel");
    annotate.dispatchWorkspaceBoxDrag({
      phase: "begin",
      drag_kind: "create",
      object_index: null,
      capture_x: 16,
      capture_y: 24,
    });
  });

  assert.equal(transport.dispatched.length, 2);
  assert.equal(transport.dispatched[0]?.workflow, "annotate");
  assert.equal(transport.dispatched[0]?.intent, "annotate.sidebar");
  assert.equal(transport.dispatched[0]?.payload.action, "add_category");
  assert.equal(transport.dispatched[0]?.payload.category_name, "wheel");
  assert.equal(transport.dispatched[1]?.workflow, "annotate");
  assert.equal(transport.dispatched[1]?.intent, "annotate.workspace.box_drag");
  assert.equal(transport.dispatched[1]?.payload.drag_kind, "create");
  assert.equal("object_index" in (transport.dispatched[1]?.payload ?? {}), false);
});

test("BrowserLiveWorkflowState dispatches preview fit-to-capture through the split live service", () => {
  const { injector, transport } = createInjector(
    makeBrowserTestSnapshot({
      active_workflow: "live",
      workflow_state: {
        live_runtime: makeLivePredictRuntimeFixture({
          preview: {
            fit_to_capture: false,
            crop_overlay_mode: false,
          },
        }),
      },
    }),
  );

  runInInjectionContext(injector, () => {
    const live = inject(BrowserLiveWorkflowState);
    live.setPreviewFitToCapture(true);
  });

  assert.equal(transport.dispatched.length, 1);
  assert.equal(transport.dispatched[0]?.workflow, "live");
  assert.equal(transport.dispatched[0]?.intent, "live.preview.fit_to_capture");
  assert.equal(transport.dispatched[0]?.payload.enabled, true);
});
