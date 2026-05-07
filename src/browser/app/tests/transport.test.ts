import { test } from "node:test";
import * as assert from "node:assert/strict";

import type {
  BrowserHostNativeBridge,
  BrowserHostTransport,
  StateSnapshot,
} from "../src/host_api";
import { createMockBrowserHostTransport } from "./mock_transport";
import {
  installBrowserTestWindow,
  makeBrowserTestSnapshot,
  makeBrowserTestSource,
  makeNativeBridgeContractCapabilities,
  type BrowserTestWindow,
} from "./support/browser-test-fixtures";
import {
  createIntent,
  installBrowserHostTransportForTesting,
  resolveBrowserHostTransport,
} from "../src/transport";

function makeSnapshot(): StateSnapshot {
  return makeBrowserTestSnapshot({
    source: makeBrowserTestSource({
      kind: "single_image",
      locator: "/tmp/frame.png",
      recursive: false,
      capture_fps: 0,
      v4l2_buffer_count: 0,
    }),
  });
}

type TestWindow = BrowserTestWindow;

type NativeBridgeTestWindow = TestWindow & {
  __MMLTK_NATIVE_BRIDGE__: BrowserHostNativeBridge & Record<string, unknown>;
};

type WebSocketTestWindow = TestWindow & {
  __MMLTK_BROWSER_WS_URL__: string;
  __MMLTK_WORKSPACE_GPU_BRIDGE__?: {
    acquireCurrentSurface(): unknown;
    importTexture(): unknown;
    releaseSurface(): unknown;
    releaseRendererTexture(): unknown;
  };
};

type FakeWebSocketListener = (event?: { data?: string }) => void;

class FakeWebSocket {
  static readonly CONNECTING = 0;
  static readonly OPEN = 1;
  static readonly CLOSED = 3;
  static readonly instances: FakeWebSocket[] = [];

  readonly sent: string[] = [];
  readyState = FakeWebSocket.CONNECTING;
  private readonly listeners = new Map<string, Set<FakeWebSocketListener>>();

  constructor(readonly url: string) {
    FakeWebSocket.instances.push(this);
  }

  addEventListener(type: string, listener: FakeWebSocketListener): void {
    const listeners = this.listeners.get(type) ?? new Set<FakeWebSocketListener>();
    listeners.add(listener);
    this.listeners.set(type, listeners);
  }

  send(messageText: string): void {
    this.sent.push(messageText);
  }

  open(): void {
    this.readyState = FakeWebSocket.OPEN;
    this.emit("open");
  }

  receive(messageText: string): void {
    this.emit("message", { data: messageText });
  }

  private emit(type: string, event: { data?: string } = {}): void {
    for (const listener of this.listeners.get(type) ?? []) {
      listener(event);
    }
  }
}

function asTestWindow(overrides: object): TestWindow {
  return overrides as unknown as TestWindow;
}

function installFakeWebSocket(): () => void {
  FakeWebSocket.instances.length = 0;
  const priorDescriptor = Object.getOwnPropertyDescriptor(globalThis, "WebSocket");
  Object.defineProperty(globalThis, "WebSocket", {
    value: FakeWebSocket,
    configurable: true,
    writable: true,
  });
  return () => {
    FakeWebSocket.instances.length = 0;
    if (priorDescriptor) {
      Object.defineProperty(globalThis, "WebSocket", priorDescriptor);
      return;
    }
    Reflect.deleteProperty(globalThis, "WebSocket");
  };
}

function createWebSocketTransportHarness(): {
  restore: () => void;
  socket: FakeWebSocket;
  transport: BrowserHostTransport;
  windowValue: WebSocketTestWindow;
} {
  const restoreWebSocket = installFakeWebSocket();
  const windowValue = asTestWindow({
    __MMLTK_BROWSER_WS_URL__: "ws://127.0.0.1:38571/browser",
  }) as WebSocketTestWindow;
  const restoreWindow = installBrowserTestWindow(windowValue);
  const transport = resolveBrowserHostTransport();
  assert.equal(transport.mode, "websocket");
  assert.equal(FakeWebSocket.instances.length, 1);
  return {
    socket: FakeWebSocket.instances[0],
    transport,
    windowValue,
    restore: () => {
      restoreWindow();
      restoreWebSocket();
    },
  };
}

function installMockTransportWindow(): {
  restoreWindow: () => void;
  transport: BrowserHostTransport;
} {
  const transport = createMockBrowserHostTransport();
  const windowValue = asTestWindow({});
  const restoreWindow = installBrowserTestWindow(windowValue);
  const restoreTransport = installBrowserHostTransportForTesting(
    windowValue,
    transport,
    { allowMockWithoutNativeBridge: true },
  );
  return {
    transport,
    restoreWindow: () => {
      restoreTransport();
      restoreWindow();
    },
  };
}

function installNativeBridgeWindow(options: {
  overrides?: Record<string, unknown>;
  postMessage?: (messageText: string) => void;
} = {}): {
  postMessages: string[];
  restoreWindow: () => void;
  windowValue: NativeBridgeTestWindow;
} {
  const postMessages: string[] = [];
  const nativeBridgeRecord: BrowserHostNativeBridge & Record<string, unknown> = {
    postMessage(messageText: string) {
      postMessages.push(messageText);
      options.postMessage?.(messageText);
    },
    ...(options.overrides ?? {}),
  };
  const windowValue = asTestWindow({
    __MMLTK_NATIVE_BRIDGE__: nativeBridgeRecord,
  }) as NativeBridgeTestWindow;
  return {
    postMessages,
    restoreWindow: installBrowserTestWindow(windowValue),
    windowValue,
  };
}

function nativeBridge(
  windowValue: NativeBridgeTestWindow,
): NativeBridgeTestWindow["__MMLTK_NATIVE_BRIDGE__"] {
  const bridge = windowValue.__MMLTK_NATIVE_BRIDGE__;
  assert.ok(bridge);
  return bridge;
}

function deliverSnapshot(
  windowValue: NativeBridgeTestWindow,
  snapshot: StateSnapshot,
): void {
  nativeBridge(windowValue).deliverSnapshot?.(JSON.stringify(snapshot));
}

function setBridgeState(
  windowValue: NativeBridgeTestWindow,
  state: Record<string, unknown>,
): void {
  nativeBridge(windowValue).setBridgeState?.(JSON.stringify(state));
}

function reportBridgeError(
  windowValue: NativeBridgeTestWindow,
  messageText: string,
): void {
  nativeBridge(windowValue).reportError?.(messageText);
}

function postedMessages(
  rawMessages: ReadonlyArray<string>,
): Array<Record<string, unknown>> {
  return rawMessages.map((messageText) =>
    JSON.parse(messageText) as Record<string, unknown>,
  );
}

function assertNativeBridgeUnavailableTransport(
  transport: BrowserHostTransport,
): void {
  assert.equal(transport.mode, "native");
  assert.equal(transport.getSnapshot().state_revision, 0);
  assert.equal(
    transport.getBridgeState?.().lastError,
    "native browser bridge unavailable",
  );
}

function assertNativeBridgeTransport(
  transport: BrowserHostTransport,
): void {
  assert.equal(transport.mode, "native");
}

function dispatchSplineKnotDrag(
  transport: BrowserHostTransport,
  phase: "begin" | "update" | "end",
  captureX: number,
  captureY: number,
): void {
  transport.dispatch(
    createIntent("annotate", "annotate.workspace.handle_drag", {
      phase,
      handle: {
        object_index: 1,
        element_index: 1,
        role: "spline_knot",
      },
      capture_x: captureX,
      capture_y: captureY,
    }),
  );
}

async function waitFor(
  predicate: () => boolean,
  timeoutMs = 200,
): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() <= deadline) {
    if (predicate()) {
      return;
    }
    await new Promise((resolve) => globalThis.setTimeout(resolve, 0));
  }
  assert.fail("timed out waiting for async transport update");
}

function recordFromValue(value: unknown): Record<string, unknown> {
  assert.ok(
    value !== null && typeof value === "object" && !Array.isArray(value),
  );
  return value as Record<string, unknown>;
}

function annotateSidebar(snapshot: StateSnapshot): Record<string, unknown> {
  return recordFromValue(snapshot.workflow_state.annotate_sidebar);
}

function selectedAnnotateBoxState(snapshot: StateSnapshot): {
  selectedObject: Record<string, unknown>;
  displayBox: Record<string, unknown>;
  annotateWorkflow: Record<string, unknown>;
} {
  const sidebar = annotateSidebar(snapshot);
  const selectedObject = recordFromValue(sidebar.selected_object);
  const workflows = recordFromValue(snapshot.workflow_state.workflows);
  return {
    selectedObject,
    displayBox: recordFromValue(selectedObject.display_box),
    annotateWorkflow: recordFromValue(recordFromValue(workflows.annotate).annotate),
  };
}

function selectedAnnotateDisplayBox(snapshot: StateSnapshot): unknown {
  return recordFromValue(annotateSidebar(snapshot).selected_object).display_box;
}

function annotateWorkspaceScene(
  snapshot: StateSnapshot,
): Record<string, unknown> {
  return recordFromValue(snapshot.workflow_state.annotate_workspace_scene);
}

function annotateControls(snapshot: StateSnapshot): Record<string, unknown> {
  return recordFromValue(snapshot.workflow_state.annotate_controls);
}

function livePreviewControls(snapshot: StateSnapshot): Record<string, unknown> {
  return recordFromValue(snapshot.workflow_state.live_preview_controls);
}

test("resolveBrowserHostTransport prefers an injected native transport", () => {
  const snapshot = makeSnapshot();
  const nativeTransport: BrowserHostTransport = {
    mode: "native",
    getSnapshot() {
      return snapshot;
    },
    dispatch() {},
    subscribe() {
      return () => {};
    },
  };
  const windowValue = asTestWindow({});
  const restoreWindow = installBrowserTestWindow(windowValue);
  const restoreTransport = installBrowserHostTransportForTesting(
    windowValue,
    nativeTransport,
  );

  try {
    const firstIntent = createIntent("annotate", "annotate.save", {
      output_dir: "/tmp/output",
    });
    const secondIntent = createIntent("predict", "predict.start", {});

    assert.equal(resolveBrowserHostTransport(), nativeTransport);
    assert.equal(firstIntent.type, "intent");
    assert.equal(firstIntent.protocol_version, 2);
    assert.equal(firstIntent.workflow, "annotate");
    assert.equal(firstIntent.intent, "annotate.save");
    assert.ok(firstIntent.request_id > 0);
    assert.ok(secondIntent.request_id > firstIntent.request_id);
  } finally {
    restoreTransport();
    restoreWindow();
  }
});

test("resolveBrowserHostTransport ignores an injected mock transport when no runtime bridge exists", () => {
  const windowValue = asTestWindow({});
  const restoreWindow = installBrowserTestWindow(windowValue);
  const restoreTransport = installBrowserHostTransportForTesting(
    windowValue,
    createMockBrowserHostTransport(),
  );

  try {
    assertNativeBridgeUnavailableTransport(resolveBrowserHostTransport());
  } finally {
    restoreTransport();
    restoreWindow();
  }
});

test("resolveBrowserHostTransport installs a native bridge transport, caches it, and accepts pushed snapshots", async () => {
  const streamedSnapshot = makeSnapshot();
  streamedSnapshot.state_revision = 5;
  streamedSnapshot.active_workflow = "live";
  streamedSnapshot.job.summary = "streamed";
  const { restoreWindow, windowValue } = installNativeBridgeWindow();

  try {
    const transport = resolveBrowserHostTransport();
    const cachedTransport = resolveBrowserHostTransport();
    const seenRevisions: number[] = [];
    const unsubscribe = transport.subscribe((snapshot) => {
      seenRevisions.push(snapshot.state_revision);
    });

    try {
      assert.equal(transport, cachedTransport);
      assert.equal(transport.mode, "native");
      assert.ok(typeof nativeBridge(windowValue).deliverSnapshot === "function");
      assert.ok(typeof nativeBridge(windowValue).setBridgeState === "function");
      assert.ok(typeof nativeBridge(windowValue).reportError === "function");
      assertNativeBridgeTransport(transport);
      assert.equal(transport.getSnapshot().state_revision, 0);
      assert.equal(transport.getBridgeState?.().phase, "polling");
      assert.equal(transport.getBridgeState?.().connected, true);

      deliverSnapshot(windowValue, streamedSnapshot);
      await waitFor(() => transport.getSnapshot().state_revision === 5);

      assert.deepEqual(seenRevisions, [0, 5]);
      assert.equal(transport.getSnapshot().job.summary, "streamed");
      assert.equal(transport.getBridgeState?.().phase, "idle");
      assert.equal(transport.getBridgeState?.().lastSuccessRevision, 5);
    } finally {
      unsubscribe();
    }
  } finally {
    restoreWindow();
  }
});

test("native bridge transport dispatches intents through window.__MMLTK_NATIVE_BRIDGE__.postMessage", () => {
  const { postMessages, restoreWindow } = installNativeBridgeWindow();

  try {
    const transport = resolveBrowserHostTransport();
    transport.dispatch(
      createIntent("predict", "viewport.commit", {
        zoom: 1.25,
      }),
    );

    assert.equal(postMessages.length, 1);
    assert.equal(postedMessages(postMessages)[0]?.intent, "viewport.commit");
    assert.equal(transport.getBridgeState?.().phase, "dispatch");
    assert.equal(transport.getBridgeState?.().connected, true);
  } finally {
    restoreWindow();
  }
});

test("native bridge transport keeps the browser transport surface minimal", () => {
  const { postMessages, restoreWindow } = installNativeBridgeWindow();

  try {
    const transport = resolveBrowserHostTransport();
    assert.equal(postMessages.length, 0);
  } finally {
    restoreWindow();
  }
});

test("native bridge transport accepts pushed bridge state JSON and plain bridge errors", () => {
  const { restoreWindow, windowValue } = installNativeBridgeWindow();

  try {
    const transport = resolveBrowserHostTransport();
    setBridgeState(windowValue, {
      phase: "dispatch",
      connected: true,
      lastError: "native bridge warming",
      lastSuccessRevision: 12,
      runtimeCapabilities: {
        hostBackend: "legacy-backend",
        navigatorGpu: "available",
      },
    });

    assert.equal(transport.getBridgeState?.().phase, "dispatch");
    assert.equal(transport.getBridgeState?.().connected, true);
    assert.equal(transport.getBridgeState?.().lastError, "native bridge warming");
    assert.equal(transport.getBridgeState?.().lastSuccessRevision, 12);
    assert.deepEqual(transport.getBridgeState?.().runtimeCapabilities, {
      host_backend: "unknown",
      navigator_gpu: "available",
      workspace_surface_bridge: "unknown",
      workspace_surface_zero_copy: "unknown",
    });

    reportBridgeError(windowValue, "browser annotate.save blocked");
    assert.equal(
      transport.getBridgeState?.().lastError,
      "browser annotate.save blocked",
    );
    assert.equal(transport.getBridgeState?.().connected, true);
  } finally {
    restoreWindow();
  }
});

test("native bridge transport applies capability-contract updates even when the bridge phase is unchanged", () => {
  const { restoreWindow, windowValue } = installNativeBridgeWindow();

  try {
    const transport = resolveBrowserHostTransport();
    setBridgeState(windowValue, {
      phase: "polling",
      connected: true,
      lastError: "",
      lastSuccessRevision: null,
      capabilities: makeNativeBridgeContractCapabilities(),
    });

    assert.deepEqual(transport.getBridgeState?.().capabilities, {
      transport: {
        status: "ready",
        summary: "native bridge contract ready",
      },
      workspace_surface_zero_copy: {
        status: "ready",
        summary: "workspace zero-copy ready",
      },
    });
  } finally {
    restoreWindow();
  }
});

test("native bridge transport parses runtime capability details from pushed bridge state JSON", () => {
  const { restoreWindow, windowValue } = installNativeBridgeWindow();

  try {
    const transport = resolveBrowserHostTransport();
    setBridgeState(windowValue, {
      phase: "idle",
      connected: true,
      lastError: "",
      lastSuccessRevision: 9,
      runtimeCapabilities: {
        hostBackend: "cef",
        navigatorGpu: "available",
        workspaceSurfaceBridge: "available",
        workspaceSurfaceZeroCopy: "available",
      },
    });

    assert.deepEqual(transport.getBridgeState?.().runtimeCapabilities, {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "available",
    });
  } finally {
    restoreWindow();
  }
});

test("native bridge transport avoids snapshot churn when only bridge runtime diagnostics change", async () => {
  const snapshot = makeSnapshot();
  snapshot.state_revision = 14;
  snapshot.runtime_capabilities = {
    host_backend: "cef",
    navigator_gpu: "available",
    workspace_surface_bridge: "unknown",
    workspace_surface_zero_copy: "unknown",
  };
  const { restoreWindow, windowValue } = installNativeBridgeWindow();

  try {
    const transport = resolveBrowserHostTransport();
    const seenSnapshots: number[] = [];
    const unsubscribe = transport.subscribe((snapshot) => {
      seenSnapshots.push(snapshot.state_revision);
    });

    try {
      deliverSnapshot(windowValue, snapshot);
      await waitFor(() => transport.getSnapshot().state_revision === 14);

      setBridgeState(windowValue, {
        phase: "idle",
        connected: true,
        lastError: "",
        lastSuccessRevision: 14,
        runtimeCapabilities: {
          hostBackend: "cef",
          navigatorGpu: "available",
          workspaceSurfaceBridge: "available",
          workspaceSurfaceZeroCopy: "available",
        },
      });
      await waitFor(
        () =>
          transport.getBridgeState?.().runtimeCapabilities?.workspace_surface_bridge ===
          "available",
      );

      assert.deepEqual(seenSnapshots, [0, 14]);
      assert.equal(
        transport.getSnapshot().runtime_capabilities.workspace_surface_bridge,
        "unknown",
      );
      assert.equal(
        transport.getBridgeState?.().runtimeCapabilities?.workspace_surface_bridge,
        "available",
      );
    } finally {
      unsubscribe();
    }
  } finally {
    restoreWindow();
  }
});

test("native bridge transport keeps bridge-published runtime capability state authoritative over later snapshots", async () => {
  const snapshot = makeSnapshot();
  snapshot.state_revision = 14;
  snapshot.runtime_capabilities = {
    host_backend: "cef",
    navigator_gpu: "available",
    workspace_surface_bridge: "available",
    workspace_surface_zero_copy: "available",
  };
  const { restoreWindow, windowValue } = installNativeBridgeWindow();

  try {
    const transport = resolveBrowserHostTransport();
    assertNativeBridgeTransport(transport);
    setBridgeState(windowValue, {
      phase: "idle",
      connected: true,
      lastError: "",
      lastSuccessRevision: 13,
      runtimeCapabilities: {
        hostBackend: "cef",
        navigatorGpu: "available",
        workspaceSurfaceBridge: "unavailable",
        workspaceSurfaceZeroCopy: "unavailable",
      },
    });

    deliverSnapshot(windowValue, snapshot);
    await waitFor(() => transport.getSnapshot().state_revision === 14);

    assert.deepEqual(transport.getBridgeState?.().runtimeCapabilities, {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "unavailable",
      workspace_surface_zero_copy: "unavailable",
    });
  } finally {
    restoreWindow();
  }
});

test("websocket transport reports workspace bridge structure as pending until native helpers confirm", () => {
  const { restore, socket, windowValue } = createWebSocketTransportHarness();

  try {
    socket.open();
    let runtimeMessages = postedMessages(socket.sent).filter(
      (message) => message.type === "host.runtime.capabilities",
    );
    assert.equal(runtimeMessages.length, 1);
    assert.equal(runtimeMessages[0].workspace_surface_bridge, "unavailable");
    assert.equal(runtimeMessages[0].workspace_surface_zero_copy, "unknown");
    assert.equal(
      recordFromValue(
        recordFromValue(runtimeMessages[0].capabilities).workspace_surface_bridge,
      ).detail,
      "__MMLTK_WORKSPACE_GPU_BRIDGE__ is not installed",
    );

    windowValue.__MMLTK_WORKSPACE_GPU_BRIDGE__ = {
      acquireCurrentSurface() {
        return null;
      },
      importTexture() {
        return {} as GPUTexture;
      },
      releaseSurface() {
        return false;
      },
      releaseRendererTexture() {
        return false;
      },
    };

    runtimeMessages = postedMessages(socket.sent).filter(
      (message) => message.type === "host.runtime.capabilities",
    );
    assert.equal(runtimeMessages.length, 2);
    assert.equal(runtimeMessages[1].workspace_surface_bridge, "unknown");
    assert.equal(runtimeMessages[1].workspace_surface_zero_copy, "unknown");
    assert.equal(
      recordFromValue(
        recordFromValue(runtimeMessages[1].capabilities).workspace_surface_bridge,
      ).detail,
      "Workspace GPU bridge functions are installed; native helper capability is pending.",
    );
  } finally {
    restore();
  }
});

test("websocket transport keeps known CEF snapshot capabilities separate from unknown bridge diagnostics", async () => {
  const { restore, socket, transport } = createWebSocketTransportHarness();

  try {
    socket.open();

    const snapshot = makeSnapshot();
    snapshot.state_revision = 24;
    snapshot.runtime_capabilities = {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "available",
    };
    socket.receive(JSON.stringify(snapshot));
    await waitFor(() => transport.getSnapshot().state_revision === 24);

    socket.receive(
      JSON.stringify({
        phase: "idle",
        connected: true,
        lastSuccessRevision: 24,
        runtimeCapabilities: {
          hostBackend: "unknown",
          navigatorGpu: "unknown",
          workspaceSurfaceBridge: "unknown",
          workspaceSurfaceZeroCopy: "unknown",
        },
      }),
    );
    await waitFor(
      () =>
        transport.getBridgeState?.().runtimeCapabilities?.workspace_surface_bridge ===
        "unknown",
    );

    assert.deepEqual(transport.getSnapshot().runtime_capabilities, {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "available",
    });
    assert.deepEqual(transport.getBridgeState?.().runtimeCapabilities, {
      host_backend: "unknown",
      navigator_gpu: "unknown",
      workspace_surface_bridge: "unknown",
      workspace_surface_zero_copy: "unknown",
    });
  } finally {
    restore();
  }
});

test("resolveBrowserHostTransport keeps the shipped path native when no bridge is present", () => {
  const restoreWindow = installBrowserTestWindow(asTestWindow({}));

  try {
    const transport = resolveBrowserHostTransport();
    let notifications = 0;
    let latestSnapshot: StateSnapshot | null = null;
    const unsubscribe = transport.subscribe((snapshot) => {
      notifications += 1;
      latestSnapshot = snapshot;
    });

    assertNativeBridgeUnavailableTransport(transport);
    assert.ok(latestSnapshot);
    const initialSnapshot = latestSnapshot as StateSnapshot;
    assert.equal(initialSnapshot.state_revision, 0);
    assert.equal(initialSnapshot.workspace_surface, null);

    transport.dispatch(
      createIntent("predict", "viewport.commit", {
        zoom: 1.5,
        clip: {
          x: 4,
          y: 8,
          width: 120,
          height: 60,
        },
      }),
    );

    assert.deepEqual(transport.getSnapshot(), initialSnapshot);
    assert.equal(notifications, 1);
    assert.equal(
      transport.getBridgeState?.().lastError,
      "native browser bridge unavailable",
    );

    unsubscribe();
  } finally {
    restoreWindow();
  }
});

test("mock transport exposes annotate shell controls and updates setup, save, live, and brush state", () => {
  const { restoreWindow, transport } = installMockTransportWindow();

  try {
    let snapshot = transport.getSnapshot();
    let controls = annotateControls(snapshot);
    let setup = recordFromValue(controls.setup_frame_navigation);

    assert.equal(recordFromValue(setup.reload).intent, "annotate.setup_frame.reload");
    assert.equal(setup.current_index, 0);
    assert.equal(setup.input_count, 6);

    transport.dispatch(createIntent("annotate", "annotate.setup_frame.next", {}));
    snapshot = transport.getSnapshot();
    controls = annotateControls(snapshot);
    setup = recordFromValue(controls.setup_frame_navigation);
    assert.equal(setup.current_index, 1);
    assert.match(snapshot.job.summary, /moved to frame 2/);

    transport.dispatch(createIntent("annotate", "annotate.save_now", {}));
    snapshot = transport.getSnapshot();
    controls = annotateControls(snapshot);
    const save = recordFromValue(controls.save);
    assert.equal(recordFromValue(save.save_now).intent, "annotate.save_now");
    assert.match(snapshot.job.summary, /captured the current mock frame/);

    transport.dispatch(
      createIntent("annotate", "tool.select", { tool: "mask.paint" }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.brush_radius", { radius: 27 }),
    );
    snapshot = transport.getSnapshot();
    controls = annotateControls(snapshot);
    const brush = recordFromValue(controls.brush);
    assert.equal(brush.visible, true);
    assert.equal(brush.radius, 27);

    transport.dispatch(
      createIntent("annotate", "settings.update", {
        patch: {
          workflows: {
            annotate: {
              source: {
                kind: 3,
              },
            },
          },
        },
      }),
    );
    transport.dispatch(createIntent("annotate", "annotate.live.start", {}));
    snapshot = transport.getSnapshot();
    controls = annotateControls(snapshot);
    const liveAnnotate = recordFromValue(controls.live_annotate);
    assert.equal(liveAnnotate.visible, true);
    assert.equal(liveAnnotate.running, true);

    transport.dispatch(
      createIntent("annotate", "annotate.hold_save", { enabled: true }),
    );
    snapshot = transport.getSnapshot();
    controls = annotateControls(snapshot);
    const holdSave = recordFromValue(recordFromValue(controls.save).hold_save);
    assert.equal(holdSave.value, true);
    assert.match(snapshot.job.summary, /annotate\.hold_save armed/);

    transport.dispatch(
      createIntent("annotate", "annotate.hold_save", { enabled: false }),
    );
    transport.dispatch(createIntent("annotate", "annotate.live.stop", {}));
    snapshot = transport.getSnapshot();
    controls = annotateControls(snapshot);
    assert.equal(recordFromValue(controls.live_annotate).running, false);
    assert.equal(
      recordFromValue(recordFromValue(controls.save).hold_save).value,
      false,
    );
  } finally {
    restoreWindow();
  }
});

test("mock transport updates live preview control state when fit and full-frame toggles dispatch", () => {
  const { restoreWindow, transport } = installMockTransportWindow();

  try {
    transport.dispatch(
      createIntent("live", "live.preview.fit_to_capture", { enabled: true }),
    );
    transport.dispatch(
      createIntent("live", "live.preview.full_frame_display", {
        enabled: true,
      }),
    );

    const snapshot = transport.getSnapshot();
    const controls = livePreviewControls(snapshot);
    const fitToCapture = recordFromValue(controls.fit_to_capture);
    const fullFrame = recordFromValue(controls.full_frame_display);
    const liveRuntime = recordFromValue(snapshot.workflow_state.live_runtime);
    const preview = recordFromValue(liveRuntime.preview);

    assert.equal(fitToCapture.value, true);
    assert.equal(fullFrame.value, true);
    assert.equal(preview.fit_to_capture, true);
    assert.equal(preview.crop_overlay_mode, true);
    assert.match(snapshot.job.summary, /full_frame_display enabled/);
  } finally {
    restoreWindow();
  }
});

test("mock transport enters and exits live predict through predict.start and predict.stop", () => {
  const { restoreWindow, transport } = installMockTransportWindow();

  try {
    transport.dispatch(createIntent("live", "predict.start", {}));

    let snapshot = transport.getSnapshot();
    let liveRuntime = recordFromValue(snapshot.workflow_state.live_runtime);
    let controller = recordFromValue(liveRuntime.controller);
    assert.equal(snapshot.active_workflow, "live");
    assert.equal(liveRuntime.active_mode, "predict");
    assert.equal(controller.running, true);

    transport.dispatch(createIntent("live", "predict.stop", {}));
    snapshot = transport.getSnapshot();
    liveRuntime = recordFromValue(snapshot.workflow_state.live_runtime);
    controller = recordFromValue(liveRuntime.controller);
    assert.equal(snapshot.active_workflow, "predict");
    assert.equal(liveRuntime.active_mode, "none");
    assert.equal(controller.running, false);
    assert.match(snapshot.job.summary, /predict\.stop exited mock live predict/);
  } finally {
    restoreWindow();
  }
});

test("mock annotate workspace canvas clicks create point, spline, and skeleton objects", () => {
  const { restoreWindow, transport } = installMockTransportWindow();

  try {
    transport.dispatch(
      createIntent("annotate", "tool.select", { tool: "point" }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.click", {
        capture_x: 244,
        capture_y: 156,
      }),
    );

    let snapshot = transport.getSnapshot();
    let sidebar = annotateSidebar(snapshot);
    let selectedObject = recordFromValue(sidebar.selected_object);
    assert.equal(snapshot.annotation.instance_count, 4);
    assert.equal(selectedObject.shape_label, "Point");
    assert.deepEqual(recordFromValue(selectedObject.point), {
      x: 244,
      y: 156,
    });

    transport.dispatch(
      createIntent("annotate", "tool.select", { tool: "spline" }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.click", {
        capture_x: 520,
        capture_y: 248,
      }),
    );

    snapshot = transport.getSnapshot();
    sidebar = annotateSidebar(snapshot);
    selectedObject = recordFromValue(sidebar.selected_object);
    assert.equal(snapshot.annotation.instance_count, 5);
    assert.equal(selectedObject.shape_label, "Spline");
    assert.equal(recordFromValue(selectedObject.spline).knot_count, 1);

    transport.dispatch(
      createIntent("annotate", "tool.select", { tool: "skeleton" }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.click", {
        capture_x: 904,
        capture_y: 332,
      }),
    );

    snapshot = transport.getSnapshot();
    sidebar = annotateSidebar(snapshot);
    selectedObject = recordFromValue(sidebar.selected_object);
    const scene = annotateWorkspaceScene(snapshot);
    const visibleObjects = scene.visible_objects as Array<
      Record<string, unknown>
    >;
    assert.equal(snapshot.annotation.instance_count, 6);
    assert.equal(selectedObject.shape_label, "Skeleton");
    assert.equal(
      recordFromValue(selectedObject.skeleton).active_joint_index,
      1,
    );
    assert.equal(visibleObjects.length, 6);
    assert.match(snapshot.job.summary, /created skeleton object/);
  } finally {
    restoreWindow();
  }
});

test("mock annotate workspace handle drag updates spline handle geometry", () => {
  const { restoreWindow, transport } = installMockTransportWindow();

  try {
    transport.dispatch(
      createIntent("annotate", "annotate.sidebar", {
        action: "select_object",
        object_index: 1,
      }),
    );

    dispatchSplineKnotDrag(transport, "begin", 520, 240);
    dispatchSplineKnotDrag(transport, "update", 548, 226);
    dispatchSplineKnotDrag(transport, "end", 548, 226);

    const snapshot = transport.getSnapshot();
    const sidebar = annotateSidebar(snapshot);
    const selectedObject = recordFromValue(sidebar.selected_object);
    const spline = recordFromValue(selectedObject.spline);
    const knotPoints = spline.knot_points as Array<Record<string, unknown>>;
    const scene = annotateWorkspaceScene(snapshot);
    const editableHandles = scene.editable_handles as Array<
      Record<string, unknown>
    >;
    const movedHandle = editableHandles.find(
      (handle) =>
        handle.role === "spline_knot" &&
        handle.element_index === 1 &&
        handle.object_index === 1,
    );

    assert.equal(selectedObject.shape_label, "Spline");
    assert.deepEqual(knotPoints[1], {
      x: 548,
      y: 226,
    });
    assert.deepEqual(recordFromValue(movedHandle?.capture_point), {
      x: 548,
      y: 226,
    });
    assert.match(snapshot.job.summary, /committed spline handle drag/);
  } finally {
    restoreWindow();
  }
});

test("mock annotate workspace box drag creates a new box object", () => {
  const { restoreWindow, transport } = installMockTransportWindow();

  try {
    const initialSnapshot = transport.getSnapshot();

    transport.dispatch(
      createIntent("annotate", "tool.select", { tool: "box" }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.box_drag", {
        phase: "begin",
        drag_kind: "create",
        capture_x: 188,
        capture_y: 140,
      }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.box_drag", {
        phase: "update",
        drag_kind: "create",
        capture_x: 336,
        capture_y: 284,
      }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.box_drag", {
        phase: "end",
        drag_kind: "create",
        capture_x: 336,
        capture_y: 284,
      }),
    );

    const snapshot = transport.getSnapshot();
    const { selectedObject, displayBox, annotateWorkflow } =
      selectedAnnotateBoxState(snapshot);

    assert.equal(
      snapshot.annotation.instance_count,
      initialSnapshot.annotation.instance_count + 1,
    );
    assert.equal(selectedObject.shape_label, "Box");
    assert.equal(Number(displayBox.x1), 188);
    assert.equal(Number(displayBox.y1), 140);
    assert.equal(Number(displayBox.x2), 337);
    assert.equal(Number(displayBox.y2), 285);
    assert.equal(annotateWorkflow.active_tool, "box");
    assert.match(snapshot.job.summary, /created box object/);
  } finally {
    restoreWindow();
  }
});

test("mock annotate workspace box drag moves the selected box object", () => {
  const { restoreWindow, transport } = installMockTransportWindow();

  try {
    const initialSnapshot = transport.getSnapshot();
    const initialBox = selectedAnnotateDisplayBox(initialSnapshot);

    transport.dispatch(
      createIntent("annotate", "tool.select", { tool: "direct" }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.box_drag", {
        phase: "begin",
        drag_kind: "move",
        object_index: 0,
        capture_x: 260,
        capture_y: 200,
      }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.box_drag", {
        phase: "update",
        drag_kind: "move",
        object_index: 0,
        capture_x: 304,
        capture_y: 226,
      }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.box_drag", {
        phase: "end",
        drag_kind: "move",
        object_index: 0,
        capture_x: 304,
        capture_y: 226,
      }),
    );

    const snapshot = transport.getSnapshot();
    const selectedObject = recordFromValue(
      annotateSidebar(snapshot).selected_object,
    );
    const displayBox = recordFromValue(selectedObject.display_box);

    assert.equal(
      Number(displayBox.x1),
      Number(recordFromValue(initialBox).x1) + 44,
    );
    assert.equal(
      Number(displayBox.y1),
      Number(recordFromValue(initialBox).y1) + 26,
    );
    assert.equal(
      Number(displayBox.x2),
      Number(recordFromValue(initialBox).x2) + 44,
    );
    assert.equal(
      Number(displayBox.y2),
      Number(recordFromValue(initialBox).y2) + 26,
    );
    assert.match(snapshot.job.summary, /committed box move/);
  } finally {
    restoreWindow();
  }
});

test("mock annotate workspace color sample updates the armed mask color range", () => {
  const { restoreWindow, transport } = installMockTransportWindow();

  try {
    transport.dispatch(
      createIntent("annotate", "annotate.sidebar", {
        action: "mask.update_color_ranges",
        sup: {
          center: {
            hue_degrees: 24,
            saturation: 0.42,
            value: 0.58,
          },
          tolerance: {
            hue_minus_pct: 8,
            hue_plus_pct: 10,
            saturation_minus_pct: 12,
            saturation_plus_pct: 14,
            value_minus_pct: 16,
            value_plus_pct: 18,
          },
          sampling: true,
        },
        nosup: {
          center: {
            hue_degrees: 122,
            saturation: 0.36,
            value: 0.44,
          },
          tolerance: {
            hue_minus_pct: 6,
            hue_plus_pct: 9,
            saturation_minus_pct: 10,
            saturation_plus_pct: 11,
            value_minus_pct: 13,
            value_plus_pct: 15,
          },
          sampling: false,
        },
      }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.color_sample", {
        capture_x: 640,
        capture_y: 220,
      }),
    );

    const snapshot = transport.getSnapshot();
    const selectedObject = recordFromValue(
      annotateSidebar(snapshot).selected_object,
    );
    const sup = recordFromValue(selectedObject.sup);
    const nosup = recordFromValue(selectedObject.nosup);
    const supCenter = recordFromValue(sup.center);

    assert.notEqual(Number(supCenter.hue_degrees), 24);
    assert.notEqual(Number(supCenter.saturation), 0.42);
    assert.equal(Boolean(sup.sampling), false);
    assert.equal(Boolean(nosup.sampling), false);
    assert.match(snapshot.job.summary, /sampled suppress mask color/);
  } finally {
    restoreWindow();
  }
});

test("mock annotate workspace mask brush and fill mutate the selected mask-capable object", () => {
  const { restoreWindow, transport } = installMockTransportWindow();

  try {
    const initialSnapshot = transport.getSnapshot();
    const initialBox = selectedAnnotateDisplayBox(initialSnapshot);

    transport.dispatch(
      createIntent("annotate", "tool.select", { tool: "mask.paint" }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.brush", {
        phase: "begin",
        capture_x: 424,
        capture_y: 338,
        radius: 12,
      }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.brush", {
        phase: "update",
        capture_x: 448,
        capture_y: 356,
        radius: 12,
      }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.brush", {
        phase: "end",
        capture_x: 448,
        capture_y: 356,
        radius: 12,
      }),
    );
    transport.dispatch(
      createIntent("annotate", "annotate.workspace.fill", {
        capture_x: 470,
        capture_y: 372,
      }),
    );

    const snapshot = transport.getSnapshot();
    const { selectedObject, displayBox, annotateWorkflow } =
      selectedAnnotateBoxState(snapshot);

    assert.equal(selectedObject.shape_label, "Box");
    assert.ok(Number(displayBox.x2) > Number(recordFromValue(initialBox).x2));
    assert.ok(Number(displayBox.y2) > Number(recordFromValue(initialBox).y2));
    assert.equal(annotateWorkflow.active_tool, "mask.fill");
    assert.match(snapshot.job.summary, /filled selected mask/);
  } finally {
    restoreWindow();
  }
});
