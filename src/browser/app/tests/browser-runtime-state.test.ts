import * as assert from "node:assert/strict";
import { test } from "node:test";

import { inject } from "@angular/core";

import { BrowserHostRuntimeState } from "../src/app/state/browser-host-runtime.service";
import { BrowserWorkspaceStateService } from "../src/app/state/browser-workspace.service";
import { liveStatusState } from "../src/browser_shell_state";
import type { StateSnapshot } from "../src/host_api";
import { HOST_API_CONTRACT_HASH } from "../src/host_api.generated";
import { buildWorkspaceCanvasLayout } from "../src/workspace_renderer";
import {
  createAngularServiceHarness,
  type AngularServiceHarness,
} from "./support/angular-test-harness";
import {
  makeAvailableBrowserTestRuntimeCapabilities,
  makeBrowserTestSource,
  makeBrowserTestSnapshot,
  makeBrowserTestWorkspaceSurface,
  makeBrowserTestWorkspaceBridgeSnapshot,
  makeRuntimeWorkspaceProviders,
} from "./support/browser-test-fixtures";
import {
  assertPrimaryChromeOmitsRawDiagnostics,
  assertSemanticViewportCommitPayload,
} from "./support/browser-state-assertions";
import { makeTestOverlayRectPrimitive } from "./support/workspace-render-fixtures";

function createHarness(
  snapshot: StateSnapshot,
): AngularServiceHarness {
  return createAngularServiceHarness(snapshot, makeRuntimeWorkspaceProviders(), {
    mode: "native",
  });
}

function updateWorkspaceSurfaceReport(
  workspace: BrowserWorkspaceStateService,
  surfaceBoundsCss: { x: number; y: number; width: number; height: number },
  options: {
    overlayPrimitive: boolean;
  } = { overlayPrimitive: false },
): void {
  workspace.updateWorkspaceSurfaceReport(
    buildWorkspaceCanvasLayout(
      {
        ...workspace.workspaceRenderInput(),
        overlayPrimitives: options.overlayPrimitive
          ? [makeTestOverlayRectPrimitive()]
          : [],
      },
      {
        devicePixelRatio: 2,
        surfaceBoundsCss,
      },
    ),
  );
}

function assertWorkspaceBridgeFrameReady(
  workspace: BrowserWorkspaceStateService,
): void {
  const diagnostics = workspace.workspaceDiagnostics();
  assert.equal(diagnostics.framePathLabel, "workspace surface bridge target");
  assert.equal(
    diagnostics.statusItems.find((item) => item.key === "frame_path")?.status,
    "ready",
  );
}

function assertPrimaryWorkspaceChromeOmitsRawDiagnostics(
  workspace: BrowserWorkspaceStateService,
): void {
  assertPrimaryChromeOmitsRawDiagnostics([workspace.workspace().title]);
}

function makeAvailableRuntimeSnapshot(
  overrides: Partial<StateSnapshot> = {},
): StateSnapshot {
  return makeBrowserTestSnapshot({
    state_revision: 4,
    ...overrides,
    runtime_capabilities: makeAvailableBrowserTestRuntimeCapabilities(
      overrides.runtime_capabilities,
    ),
  });
}

function makeNativePresentedLiveSnapshot(
  overrides: Partial<StateSnapshot> = {},
): StateSnapshot {
  return makeAvailableRuntimeSnapshot({
    active_workflow: "live",
    workspace_surface: makeBrowserTestWorkspaceSurface({ revision: "401" }),
    ...overrides,
    workflow_state: {
      live_runtime: {
        active_mode: "predict",
        show_running_section: true,
        preview: {
          initialized: true,
          has_frame: true,
          displayed_region: { x: 0, y: 0, width: 1280, height: 720 },
          frame_id: 401,
          surface_revision: "401",
          owner: "live",
          native_presented: true,
          display_startup_state: "native presented",
        },
      },
      ...overrides.workflow_state,
    },
  });
}

function measuredWorkspace(
  run: AngularServiceHarness["run"],
): BrowserWorkspaceStateService {
  const workspace = run(() => inject(BrowserWorkspaceStateService));
  updateWorkspaceSurfaceReport(workspace, {
    x: 20,
    y: 30,
    width: 640,
    height: 360,
  });
  return workspace;
}

function publishBridgeCapabilities(
  transport: AngularServiceHarness["transport"],
  snapshot: StateSnapshot,
  runtimeCapabilities: StateSnapshot["runtime_capabilities"],
  capabilities?: NonNullable<
    ReturnType<AngularServiceHarness["transport"]["getBridgeState"]>["capabilities"]
  >,
): void {
  transport.publishBridgeState({
    phase: "idle",
    connected: true,
    lastError: "",
    lastSuccessRevision: snapshot.state_revision,
    runtimeCapabilities,
    ...(capabilities === undefined ? {} : { capabilities }),
  });
}

test("BrowserHostRuntimeState reacts to browser runtime capability snapshots", () => {
  const initialSnapshot = makeBrowserTestSnapshot({
    state_revision: 3,
    runtime_capabilities: {
      host_backend: "unknown",
      navigator_gpu: "unknown",
      workspace_surface_bridge: "unknown",
      workspace_surface_zero_copy: "unknown",
    },
  });
  const { destroy, run, transport } = createHarness(initialSnapshot);

  try {
    const runtime = run(() => inject(BrowserHostRuntimeState));
    assert.deepEqual(
      runtime.runtimeCapabilityStatus().map((item) => ({
        key: item.key,
        status: item.status,
      })),
      [
        { key: "host_backend", status: "pending" },
        { key: "navigator_gpu", status: "pending" },
        { key: "workspace_surface_bridge", status: "pending" },
        { key: "workspace_surface_zero_copy", status: "pending" },
      ],
    );

    transport.publish(
      makeBrowserTestSnapshot({
        state_revision: 4,
        runtime_capabilities: {
          host_backend: "cef",
          navigator_gpu: "available",
          workspace_surface_bridge: "available",
          workspace_surface_zero_copy: "available",
        },
      }),
    );

    assert.equal(runtime.transportStatus().label, "native bridge");
    assert.equal(runtime.transportStatus().readbackLabel, "workspace surface");
    assert.deepEqual(
      runtime.runtimeCapabilityStatus().map((item) => ({
        key: item.key,
        status: item.status,
        summary: item.summary,
      })),
      [
        { key: "host_backend", status: "ready", summary: "host backend CEF" },
        {
          key: "navigator_gpu",
          status: "ready",
          summary: "navigator.gpu available",
        },
        {
          key: "workspace_surface_bridge",
          status: "ready",
          summary: "workspace surface bridge ready",
        },
        {
          key: "workspace_surface_zero_copy",
          status: "ready",
          summary: "workspace zero-copy available",
        },
      ],
    );
    assert.deepEqual(
      runtime.transportStatus().statusItems.map((item) => item.key),
      ["transport", "workspace_surface_bridge", "workspace_surface_zero_copy"],
    );
  } finally {
    destroy();
  }
});

test("BrowserHostRuntimeState keeps transport diagnostics focused on the workspace bridge contract", () => {
  const snapshot = makeAvailableRuntimeSnapshot();
  const { destroy, run, transport } = createHarness(snapshot);

  try {
    const runtime = run(() => inject(BrowserHostRuntimeState));
    publishBridgeCapabilities(
      transport,
      snapshot,
      makeAvailableBrowserTestRuntimeCapabilities(),
    );

    assert.equal(
      runtime.runtimeCapabilityStatus().find((item) => item.key === "workspace_surface_bridge")
        ?.status,
      "ready",
    );
    assert.equal(
      runtime.transportStatus().statusItems.some(
        (item) => item.key === "browser_frame_import",
      ),
      false,
    );
    assert.doesNotMatch(runtime.transportStatus().detail, /browser frame import/i);
  } finally {
    destroy();
  }
});

test("BrowserHostRuntimeState keeps known snapshot capabilities when bridge updates are unknown", () => {
  const snapshot = makeAvailableRuntimeSnapshot();
  const { destroy, run, transport } = createHarness(snapshot);

  try {
    const runtime = run(() => inject(BrowserHostRuntimeState));
    publishBridgeCapabilities(transport, snapshot, {
      host_backend: "unknown",
      navigator_gpu: "unknown",
      workspace_surface_bridge: "unknown",
      workspace_surface_zero_copy: "unknown",
    });

    assert.deepEqual(runtime.runtimeCapabilities(), {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "available",
    });
    assert.equal(
      runtime.runtimeCapabilityStatus().find((item) => item.key === "host_backend")
        ?.summary,
      "host backend CEF",
    );
    assert.equal(
      runtime.runtimeCapabilityStatus().find((item) => item.key === "workspace_surface_bridge")
        ?.status,
      "ready",
    );
  } finally {
    destroy();
  }
});

test("BrowserHostRuntimeState uses bridge capability contracts for CEF structural failures", () => {
  const snapshot = makeAvailableRuntimeSnapshot({
    state_revision: 4,
    runtime_capabilities: {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "unknown",
    },
  });
  const { destroy, run, transport } = createHarness(snapshot);

  try {
    const runtime = run(() => inject(BrowserHostRuntimeState));
    publishBridgeCapabilities(
      transport,
      snapshot,
      {
        host_backend: "unknown",
        navigator_gpu: "unknown",
        workspace_surface_bridge: "unavailable",
        workspace_surface_zero_copy: "unknown",
      },
      {
        workspace_surface_bridge: {
          status: "blocked",
          summary: "workspace surface bridge unavailable",
          detail: "CEF workspace texture import helper is absent",
        },
      },
    );

    const bridgeStatus = runtime.runtimeCapabilityStatus().find(
      (item) => item.key === "workspace_surface_bridge",
    );
    assert.equal(runtime.runtimeCapabilities().host_backend, "cef");
    assert.equal(bridgeStatus?.status, "blocked");
    assert.equal(
      bridgeStatus?.detail,
      "CEF workspace texture import helper is absent",
    );
  } finally {
    destroy();
  }
});

test("BrowserHostRuntimeState keeps transport capability summaries scoped to transport, bridge, and zero-copy", () => {
  const snapshot = makeAvailableRuntimeSnapshot({
    state_revision: 5,
    runtime_capabilities: {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "available",
    },
  });
  const { destroy, run } = createHarness(snapshot);

  try {
    const runtime = run(() => inject(BrowserHostRuntimeState));
    assert.equal(
      runtime.runtimeCapabilityStatus().find((item) => item.key === "workspace_surface_bridge")
        ?.status,
      "ready",
    );
    assert.deepEqual(
      runtime.transportStatus().statusItems.map((item) => item.key),
      ["transport", "workspace_surface_bridge", "workspace_surface_zero_copy"],
    );
  } finally {
    destroy();
  }
});

test("Live runtime preview reports native-presented workspace without renderer draw state", () => {
  const snapshot = makeNativePresentedLiveSnapshot();

  const liveStatus = liveStatusState(snapshot);
  assert.equal(liveStatus.preview.hasFrame, true);
  assert.equal(liveStatus.preview.displayStartupState, "native presented");
  assert.equal(liveStatus.preview.rendererImportedRevision, "");
  assert.equal(liveStatus.preview.rendererSubmittedRevision, "");
  assert.equal(liveStatus.preview.rendererDrawnRevision, "");
  assert.equal(
    liveStatus.statusItems.find((item) => item.key === "preview")?.status,
    "active",
  );
});

test("Live runtime preview does not imply native-presented from active live mode", () => {
  const snapshot = makeAvailableRuntimeSnapshot({
    active_workflow: "live",
    source: makeBrowserTestSource({ kind: "video_stream" }),
    workspace_surface: makeBrowserTestWorkspaceSurface({ revision: "400" }),
    workflow_state: {
      live_runtime: {
        active_mode: "predict",
        show_running_section: true,
        preview: {
          initialized: true,
          has_frame: true,
          surface_revision: "400",
          owner: "live",
          display_startup_state: "surface published",
        },
      },
    },
  });

  const liveStatus = liveStatusState(snapshot);
  assert.equal(liveStatus.runtime.activeMode, "predict");
  assert.equal(liveStatus.preview.nativePresented, false);
  assert.equal(liveStatus.preview.displayStartupState, "surface published");
});

test("BrowserWorkspaceStateService reports native-presented live diagnostics without bridge capability rows", () => {
  const snapshot = makeNativePresentedLiveSnapshot({
    source: makeBrowserTestSource({ kind: "video_stream" }),
  });
  const { destroy, run } = createHarness(snapshot);

  try {
    const workspace = measuredWorkspace(run);

    const diagnostics = workspace.workspaceDiagnostics();
    assert.equal(diagnostics.framePathLabel, "native-presented live workspace");
    assert.deepEqual(
      diagnostics.statusItems.map((item) => item.key),
      ["frame_path", "overlay_path", "transport"],
    );
    assert.equal(
      /navigator\.gpu|zero-copy|surface bridge/i.test(diagnostics.detail),
      false,
    );
  } finally {
    destroy();
  }
});

test("BrowserHostRuntimeState reports generated contract hash mismatch", () => {
  const snapshot = makeBrowserTestSnapshot({
    contract_hash: "stale-contract",
  });
  const { destroy, run } = createHarness(snapshot);

  try {
    const runtime = run(() => inject(BrowserHostRuntimeState));
    assert.equal(runtime.contractHashMatched(), false);
    assert.match(runtime.contractMismatchDetail(), /stale-contract/);
    assert.match(runtime.contractMismatchDetail(), new RegExp(HOST_API_CONTRACT_HASH));
  } finally {
    destroy();
  }
});

test("BrowserHostRuntimeState blocks the workspace bridge when the runtime reports it unavailable", () => {
  const snapshot = makeBrowserTestSnapshot({
    state_revision: 5,
    runtime_capabilities: {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "unavailable",
      workspace_surface_zero_copy: "unknown",
    },
  });
  const { destroy, run } = createHarness(snapshot);

  try {
    const runtime = run(() => inject(BrowserHostRuntimeState));
    assert.equal(
      runtime.runtimeCapabilityStatus().find((item) => item.key === "workspace_surface_bridge")
        ?.status,
      "blocked",
    );
    assert.match(
      runtime.runtimeCapabilityStatus().find((item) => item.key === "workspace_surface_bridge")
        ?.detail ?? "",
      /has not kept the workspace surface bridge available/i,
    );
  } finally {
    destroy();
  }
});

test("BrowserWorkspaceStateService treats known-backend unknown bridge state as blocked diagnostics", () => {
  const snapshot = makeBrowserTestWorkspaceBridgeSnapshot({
    state_revision: 6,
    runtime_capabilities: {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "unknown",
      workspace_surface_zero_copy: "unknown",
    },
  });
  const { destroy, run } = createHarness(snapshot);

  try {
    const runtime = run(() => inject(BrowserHostRuntimeState));
    const workspace = measuredWorkspace(run);

    const bridgeStatus = runtime.runtimeCapabilityStatus().find(
      (item) => item.key === "workspace_surface_bridge",
    );
    assert.equal(bridgeStatus?.status, "blocked");
    assert.equal(
      workspace.workspaceDiagnostics().framePathLabel,
      "workspace surface bridge blocked",
    );
    assertPrimaryWorkspaceChromeOmitsRawDiagnostics(workspace);
    assert.equal(
      workspace.workspaceDiagnostics().statusItems.find((item) => item.key === "frame_path")
        ?.status,
      "blocked",
    );
    assert.equal(workspace.workspaceHardError()?.code, "workspace_surface_bridge");
    assert.equal(workspace.workspaceInteractionBlocked(), true);
  } finally {
    destroy();
  }
});

test("BrowserHostRuntimeState blocks workspace zero-copy when the runtime reports it unavailable", () => {
  const snapshot = makeBrowserTestSnapshot({
    state_revision: 5,
    runtime_capabilities: {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "unavailable",
    },
  });
  const { destroy, run } = createHarness(snapshot);

  try {
    const runtime = run(() => inject(BrowserHostRuntimeState));
    assert.equal(runtime.transportStatus().readbackLabel, "workspace surface");
    assert.equal(
      runtime.runtimeCapabilityStatus().find((item) => item.key === "workspace_surface_zero_copy")
        ?.status,
      "blocked",
    );
  } finally {
    destroy();
  }
});

test("BrowserWorkspaceStateService keeps zero-copy pending distinct from blocked bridge diagnostics", () => {
  const snapshot = makeBrowserTestWorkspaceBridgeSnapshot({
    state_revision: 7,
    runtime_capabilities: {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "unknown",
    },
  });
  const { destroy, run } = createHarness(snapshot);

  try {
    const workspace = run(() => inject(BrowserWorkspaceStateService));
    updateWorkspaceSurfaceReport(workspace, {
      x: 20,
      y: 30,
      width: 640,
      height: 360,
    });

    assert.equal(
      workspace.workspaceDiagnostics().framePathLabel,
      "workspace zero-copy pending",
    );
    assertPrimaryWorkspaceChromeOmitsRawDiagnostics(workspace);
    assert.equal(
      workspace.workspaceDiagnostics().statusItems.find((item) => item.key === "frame_path")
        ?.status,
      "pending",
    );
    assert.equal(workspace.workspaceHardError(), null);
    assert.equal(workspace.workspaceInteractionBlocked(), false);
  } finally {
    destroy();
  }
});

test("BrowserWorkspaceStateService keeps the workspace on the bridge path once the canvas is measured and a surface is published", () => {
  const initialSnapshot = makeBrowserTestWorkspaceBridgeSnapshot({
    state_revision: 10,
  });
  const { destroy, run, transport } = createHarness(initialSnapshot);

  try {
    const workspace = run(() => inject(BrowserWorkspaceStateService));
    updateWorkspaceSurfaceReport(workspace, {
      x: 24,
      y: 48,
      width: 640,
      height: 360,
    }, {
      overlayPrimitive: true,
    });
    assertWorkspaceBridgeFrameReady(workspace);
    assertPrimaryWorkspaceChromeOmitsRawDiagnostics(workspace);
    assert.equal(workspace.rendererStatus(), "surface-ready · annotations-ready");

    transport.publish(
      makeBrowserTestWorkspaceBridgeSnapshot({
        state_revision: 11,
        runtime_capabilities: {
          workspace_surface_zero_copy: "unavailable",
        },
      }),
    );

    assert.equal(
      workspace.workspaceDiagnostics().framePathLabel,
      "workspace zero-copy blocked",
    );
    assert.equal(
      workspace.workspaceDiagnostics().statusItems.find((item) => item.key === "frame_path")
        ?.status,
      "blocked",
    );
    assert.equal(
      workspace.workspaceCanvasChrome().fallbackLabel,
      "Zero-copy unavailable",
    );
    assert.equal(
      workspace.workspaceHardError()?.code,
      "workspace_surface_zero_copy",
    );
    assert.equal(workspace.workspaceInteractionBlocked(), true);
  } finally {
    destroy();
  }
});

test("BrowserWorkspaceStateService exposes a hard error and clears viewport interactions when the workspace bridge is blocked", () => {
  const initialSnapshot = makeBrowserTestWorkspaceBridgeSnapshot({
    state_revision: 20,
  });
  const { destroy, run, transport } = createHarness(initialSnapshot);

  try {
    const workspace = run(() => inject(BrowserWorkspaceStateService));
    updateWorkspaceSurfaceReport(workspace, {
      x: 18,
      y: 30,
      width: 720,
      height: 405,
    });

    assert.equal(
      workspace.beginWorkspaceInteraction(7, 320, 180, 0, false),
      true,
    );
    assert.equal(workspace.workspaceState().pointerId, 7);

    transport.publish(
      makeBrowserTestWorkspaceBridgeSnapshot({
        state_revision: 21,
        runtime_capabilities: {
          workspace_surface_bridge: "unavailable",
          workspace_surface_zero_copy: "unknown",
        },
      }),
    );

    assert.equal(
      workspace.workspaceDiagnostics().framePathLabel,
      "workspace surface bridge blocked",
    );
    assert.equal(
      workspace.workspaceHardError()?.code,
      "workspace_surface_bridge",
    );
    assert.match(
      workspace.workspaceHardError()?.detail ?? "",
      /has not kept the workspace surface bridge available/i,
    );
    assert.equal(workspace.workspaceInteractionBlocked(), true);
    assert.equal(workspace.canFitWorkspace(), false);
    assert.equal(workspace.workspaceState().pointerId, null);
    assert.equal(workspace.workspaceState().mode, "idle");
    assert.equal(
      workspace.beginWorkspaceInteraction(8, 320, 180, 0, false),
      false,
    );
    assert.equal(workspace.moveWorkspaceInteraction(7, 360, 200), false);
    assert.equal(workspace.zoomWorkspaceAtCanvasPoint(320, 180, -120), false);
    assert.equal(workspace.commitViewport(), false);
    assert.equal(transport.dispatched.length, 0);
  } finally {
    destroy();
  }
});

test("BrowserWorkspaceStateService keeps the surface pending until Angular reports workspace placement", () => {
  const snapshot = makeBrowserTestWorkspaceBridgeSnapshot({
    state_revision: 12,
  });
  const { destroy, run } = createHarness(snapshot);

  try {
    const workspace = run(() => inject(BrowserWorkspaceStateService));
    assert.equal(
      workspace.workspaceDiagnostics().framePathLabel,
      "workspace canvas pending",
    );
    assertPrimaryWorkspaceChromeOmitsRawDiagnostics(workspace);
    assert.equal(
      workspace.workspaceDiagnostics().statusItems.find((item) => item.key === "frame_path")
        ?.status,
      "pending",
    );
    assert.equal(
      workspace.workspaceCanvasChrome().fallbackLabel,
      "Workspace canvas pending",
    );
  } finally {
    destroy();
  }
});

test("BrowserWorkspaceStateService does not require steady-state transport metadata once Angular reports workspace placement", () => {
  const snapshot = makeBrowserTestWorkspaceBridgeSnapshot({
    state_revision: 12,
  });
  const { destroy, run } = createHarness(snapshot);

  try {
    const workspace = run(() => inject(BrowserWorkspaceStateService));
    updateWorkspaceSurfaceReport(workspace, {
      x: 10,
      y: 16,
      width: 960,
      height: 540,
    });
    assertWorkspaceBridgeFrameReady(workspace);
    assert.equal(workspace.workspaceCanvasChrome().fallbackLabel, null);
    assert.equal(workspace.rendererStatus(), "surface-ready");
  } finally {
    destroy();
  }
});

test("BrowserWorkspaceStateService keeps viewport commits semantic and omits stale browser-only payloads", () => {
  const snapshot = makeBrowserTestWorkspaceBridgeSnapshot({
    state_revision: 13,
    active_workflow: "annotate",
  });
  const { destroy, run, transport: hostTransport } = createHarness(snapshot);

  try {
    const workspace = run(() => inject(BrowserWorkspaceStateService));
    workspace.updateWorkspaceSurfaceReport(
      buildWorkspaceCanvasLayout(
        {
          ...workspace.workspaceRenderInput(),
          overlayPrimitives: [
            {
              kind: "rect",
              x: 40,
              y: 56,
              width: 120,
              height: 80,
              strokeStyle: "rgba(255, 220, 96, 1)",
              lineWidth: 2,
              fillStyle: null,
              dash: [],
            },
          ],
        },
        {
          devicePixelRatio: 2,
          surfaceBoundsCss: {
            x: 12,
            y: 18,
            width: 800,
            height: 450,
          },
        },
      ),
    );

    assert.equal(workspace.commitViewport(), true);
    assertSemanticViewportCommitPayload(hostTransport.dispatched[0]?.payload);
  } finally {
    destroy();
  }
});
