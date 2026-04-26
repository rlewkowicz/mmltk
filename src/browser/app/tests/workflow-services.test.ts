import * as assert from "node:assert/strict";
import { test } from "node:test";

import { inject, runInInjectionContext, type StaticProvider } from "@angular/core";

import { BrowserAnnotateSidebarState } from "../src/app/state/browser-annotate-sidebar.service";
import { BrowserAnnotateWorkflowState } from "../src/app/state/browser-annotate-workflow.service";
import { BrowserExportWorkflowState } from "../src/app/state/browser-export-workflow.service";
import { BrowserHostRuntimeState } from "../src/app/state/browser-host-runtime.service";
import { BrowserLiveWorkflowState } from "../src/app/state/browser-live-workflow.service";
import { BrowserPredictWorkflowState } from "../src/app/state/browser-predict-workflow.service";
import { BrowserShellChromeState } from "../src/app/state/browser-shell-chrome.service";
import { BrowserSourceState } from "../src/app/state/browser-source-state.service";
import { BrowserTrainWorkflowState } from "../src/app/state/browser-train-workflow.service";
import { BrowserWorkflowPrimaryActionState } from "../src/app/state/browser-workflow-primary-action.service";
import { BrowserWorkflowDraftSharedState } from "../src/app/state/browser-workflow-draft-shared.service";
import { BrowserValidateWorkflowState } from "../src/app/state/browser-validate-workflow.service";
import { BrowserWorkflowPanelsState } from "../src/app/state/browser-workflow-panels.service";
import { BrowserWorkflowRouteState } from "../src/app/state/browser-workflow-route.service";
import type { StateSnapshot } from "../src/host_api";
import { createAngularServiceHarness } from "./support/angular-test-harness";
import {
  makeAnnotateSidebarFixture,
  makeBrowserTestSnapshot,
  makeBrowserTestSource,
  makeLivePredictRuntimeFixture,
} from "./support/browser-test-fixtures";

function createInjector(snapshot: StateSnapshot): {
  injector: ReturnType<typeof createAngularServiceHarness>["injector"];
  transport: ReturnType<typeof createAngularServiceHarness>["transport"];
} {
  const providers: StaticProvider[] = [
    { provide: BrowserHostRuntimeState, useFactory: () => new BrowserHostRuntimeState() },
    { provide: BrowserWorkflowRouteState, useFactory: () => new BrowserWorkflowRouteState() },
    { provide: BrowserShellChromeState, useFactory: () => new BrowserShellChromeState() },
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
    { provide: BrowserWorkflowPanelsState, useFactory: () => new BrowserWorkflowPanelsState() },
    { provide: BrowserSourceState, useFactory: () => new BrowserSourceState() },
    {
      provide: BrowserWorkflowPrimaryActionState,
      useFactory: () => new BrowserWorkflowPrimaryActionState(),
    },
    {
      provide: BrowserAnnotateSidebarState,
      useFactory: () => new BrowserAnnotateSidebarState(),
    },
  ];
  const harness = createAngularServiceHarness(snapshot, providers);
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
    assert.equal(panels.workflowControlStatus()[0]?.summary, "workflow draft staged");
    assert.equal(panels.workflowControlStatus()[1]?.label, "Export");
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
    assert.equal(primary.primaryActionLabel(), "Stop Live Predict");
    primary.runPrimaryAction();
  });

  assert.equal(transport.dispatched.length, 1);
  assert.equal(transport.dispatched[0]?.workflow, "live");
  assert.equal(transport.dispatched[0]?.intent, "predict.stop");
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
