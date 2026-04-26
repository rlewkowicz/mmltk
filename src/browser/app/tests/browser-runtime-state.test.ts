import * as assert from "node:assert/strict";
import { test } from "node:test";

import { inject, type StaticProvider } from "@angular/core";

import { BrowserHostRuntimeState } from "../src/app/state/browser-host-runtime.service";
import { BrowserWorkspaceStateService } from "../src/app/state/browser-workspace.service";
import { BrowserWorkflowRouteState } from "../src/app/state/browser-workflow-route.service";
import type { StateSnapshot } from "../src/host_api";
import { buildWorkspaceCanvasLayout } from "../src/workspace_renderer";
import {
  createAngularServiceHarness,
  type AngularServiceHarness,
} from "./support/angular-test-harness";
import {
  makeBrowserTestSnapshot,
  makeBrowserTestWorkspaceBridgeSnapshot,
} from "./support/browser-test-fixtures";
import { assertSemanticViewportCommitPayload } from "./support/browser-state-assertions";

function createHarness(
  snapshot: StateSnapshot,
): AngularServiceHarness {
  const providers: StaticProvider[] = [
    { provide: BrowserHostRuntimeState, useFactory: () => new BrowserHostRuntimeState() },
    { provide: BrowserWorkflowRouteState, useFactory: () => new BrowserWorkflowRouteState() },
    {
      provide: BrowserWorkspaceStateService,
      useFactory: () => new BrowserWorkspaceStateService(),
    },
  ];
  return createAngularServiceHarness(snapshot, providers, {
    mode: "native",
  });
}

function updateWorkspaceSurfaceReport(
  workspace: BrowserWorkspaceStateService,
  surfaceBoundsCss: { x: number; y: number; width: number; height: number },
): void {
  workspace.updateWorkspaceSurfaceReport(
    buildWorkspaceCanvasLayout(
      {
        ...workspace.workspaceRenderInput(),
        overlayPrimitives: [],
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
  const snapshot = makeBrowserTestSnapshot({
    state_revision: 4,
    runtime_capabilities: {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "available",
    },
  });
  const { destroy, run, transport } = createHarness(snapshot);

  try {
    const runtime = run(() => inject(BrowserHostRuntimeState));
    transport.publishBridgeState({
      phase: "idle",
      connected: true,
      lastError: "",
      lastSuccessRevision: snapshot.state_revision,
      runtimeCapabilities: {
        host_backend: "cef",
        navigator_gpu: "available",
        workspace_surface_bridge: "available",
        workspace_surface_zero_copy: "available",
      },
    });

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

test("BrowserHostRuntimeState keeps transport capability summaries scoped to transport, bridge, and zero-copy", () => {
  const snapshot = makeBrowserTestSnapshot({
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
    });
    assertWorkspaceBridgeFrameReady(workspace);

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
      "Workspace zero-copy unavailable",
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
