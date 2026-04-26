import type {
  BrowserHostBackend,
  BrowserHostBridgeState,
  BrowserHostCapabilityContractMap,
  BrowserHostNativeBridge,
  BrowserHostSurfaceReadyMessage,
  BrowserHostTransport,
  BrowserRuntimeCapabilities,
  BrowserRuntimeCapabilityStatus,
  CaptureRegion,
  IntentMessage,
  StateSnapshot,
  Workflow,
} from "./host_api";
import {
  isRecord,
  ownWindowPropertyValue,
  workspaceSurfaceInfoFromValue,
} from "./app_shared";
import { kUnknownBrowserRuntimeCapabilities } from "./runtime_frame_contract";

const kProtocolVersion = 2;

const nativeTransportCache = new WeakMap<Window, BrowserHostTransport>();
const websocketTransportCache = new WeakMap<Window, BrowserHostTransport>();
const transportOverridesForTesting = new WeakMap<
  Window,
  BrowserHostTransportOverrideForTesting
>();

let requestCounter = 1;

interface BrowserHostTransportOverrideForTesting {
  transport: BrowserHostTransport;
  allowMockWithoutNativeBridge: boolean;
}

interface InstallBrowserHostTransportForTestingOptions {
  allowMockWithoutNativeBridge?: boolean;
}

function makeRegion(
  x: number,
  y: number,
  width: number,
  height: number,
): CaptureRegion {
  return { x, y, width, height };
}

function makeNativePlaceholderSnapshot(): StateSnapshot {
  return {
    type: "state.snapshot",
    protocol_version: kProtocolVersion,
    state_revision: 0,
    active_workflow: "predict",
    workflow_state: {},
    settings_state: {},
    job: {
      running: false,
      label: "connecting",
      summary: "Waiting for native browser bridge snapshot",
      error: "",
      output_tail: "",
      recent_logs: [],
    },
    source: {
      kind: "single_image",
      locator: "",
      recursive: false,
      device_index: 0,
      capture_width: 1,
      capture_height: 1,
      capture_fps: 0,
      v4l2_buffer_count: 0,
      has_crop: false,
      crop: makeRegion(0, 0, 0, 0),
    },
    annotation: {
      document_generation: 0,
      session_revision: 0,
      capture_width: 1,
      capture_height: 1,
      instance_count: 0,
      selected_instance: null,
    },
    runtime_capabilities: { ...kUnknownBrowserRuntimeCapabilities },
    workspace_surface: null,
  };
}

function createUnavailableNativeTransport(
  errorText = "native browser bridge unavailable",
): BrowserHostTransport {
  const snapshot = makeNativePlaceholderSnapshot();
  const bridgeState = makeBridgeState({
    connected: false,
    lastError: errorText,
  });

  return {
    mode: "native",
    getSnapshot() {
      return snapshot;
    },
    getBridgeState() {
      return bridgeState;
    },
    dispatch() {},
    subscribe(listener) {
      listener(snapshot);
      return () => {};
    },
    subscribeBridgeState(listener) {
      listener(bridgeState);
      return () => {};
    },
  };
}

function snapshotCacheSignature(snapshot: StateSnapshot): string {
  return [
    snapshot.state_revision,
    snapshot.active_workflow,
    snapshot.runtime_capabilities.host_backend,
    snapshot.runtime_capabilities.navigator_gpu,
    snapshot.runtime_capabilities.workspace_surface_bridge ?? "unknown",
    snapshot.runtime_capabilities.workspace_surface_zero_copy ?? "unknown",
    snapshot.workspace_surface?.revision ?? "none",
  ].join(":");
}

function makeBridgeState(
  overrides: Partial<BrowserHostBridgeState> = {},
): BrowserHostBridgeState {
  return {
    phase: "idle",
    connected: false,
    lastError: "",
    lastSuccessRevision: null,
    runtimeCapabilities: { ...kUnknownBrowserRuntimeCapabilities },
    ...overrides,
  };
}

function createTransportBridgeState(
  overrides: Partial<BrowserHostBridgeState> = {},
): BrowserHostBridgeState {
  return makeBridgeState(overrides);
}

function bridgeStateEquals(
  left: BrowserHostBridgeState,
  right: BrowserHostBridgeState,
): boolean {
  return (
    left.phase === right.phase &&
    left.connected === right.connected &&
    left.lastError === right.lastError &&
    left.lastSuccessRevision === right.lastSuccessRevision &&
    runtimeCapabilitiesEqual(left.runtimeCapabilities, right.runtimeCapabilities) &&
    capabilityContractMapEqual(left.capabilities, right.capabilities)
  );
}

function jsonValueEqual(left: unknown, right: unknown): boolean {
  if (Object.is(left, right)) {
    return true;
  }
  if (Array.isArray(left) && Array.isArray(right)) {
    if (left.length !== right.length) {
      return false;
    }
    for (let index = 0; index < left.length; index += 1) {
      if (!jsonValueEqual(left[index], right[index])) {
        return false;
      }
    }
    return true;
  }
  if (isRecord(left) && isRecord(right)) {
    const leftKeys = Object.keys(left);
    const rightKeys = Object.keys(right);
    if (leftKeys.length !== rightKeys.length) {
      return false;
    }
    for (const key of leftKeys) {
      if (!Object.prototype.hasOwnProperty.call(right, key)) {
        return false;
      }
      if (!jsonValueEqual(left[key], right[key])) {
        return false;
      }
    }
    return true;
  }
  return false;
}

function capabilityContractMapEqual(
  left: BrowserHostCapabilityContractMap | undefined,
  right: BrowserHostCapabilityContractMap | undefined,
): boolean {
  if (left === right) {
    return true;
  }
  if (left === undefined || right === undefined) {
    return left === right;
  }
  return jsonValueEqual(left, right);
}

function runtimeCapabilitiesEqual(
  left: BrowserRuntimeCapabilities | undefined,
  right: BrowserRuntimeCapabilities | undefined,
): boolean {
  const leftValue = left ?? kUnknownBrowserRuntimeCapabilities;
  const rightValue = right ?? kUnknownBrowserRuntimeCapabilities;
  return (
    leftValue.host_backend === rightValue.host_backend &&
    leftValue.navigator_gpu === rightValue.navigator_gpu &&
    (leftValue.workspace_surface_bridge ?? "unknown") ===
      (rightValue.workspace_surface_bridge ?? "unknown") &&
    (leftValue.workspace_surface_zero_copy ?? "unknown") ===
      (rightValue.workspace_surface_zero_copy ?? "unknown")
  );
}

function isStateSnapshot(value: unknown): value is StateSnapshot {
  return (
    isRecord(value) &&
    value.type === "state.snapshot" &&
    typeof value.state_revision === "number"
  );
}

function parseSurfaceReadyMessage(
  value: unknown,
): BrowserHostSurfaceReadyMessage | null {
  if (!isRecord(value) || value.type !== "surface.ready") {
    return null;
  }
  const surface = workspaceSurfaceInfoFromValue(value.surface);
  return surface === null ? null : { type: "surface.ready", surface };
}

function errorTextFromUnknown(error: unknown): string {
  if (error instanceof Error && error.message.trim().length > 0) {
    return error.message;
  }
  if (typeof error === "string" && error.trim().length > 0) {
    return error;
  }
  return "browser bridge request failed";
}

function notifyListeners<T>(
  listeners: Set<(value: T) => void>,
  value: T,
): void {
  for (const listener of listeners) {
    listener(value);
  }
}

function subscribeWithCurrentValue<T>(
  listeners: Set<(value: T) => void>,
  listener: (value: T) => void,
  currentValue: () => T,
): () => void {
  listeners.add(listener);
  listener(currentValue());
  return () => {
    listeners.delete(listener);
  };
}

function createTransportStateController(params: {
  listeners: Set<(nextSnapshot: StateSnapshot) => void>;
  bridgeListeners: Set<(state: BrowserHostBridgeState) => void>;
  getSnapshot(): StateSnapshot;
  setSnapshot(nextSnapshot: StateSnapshot): void;
  getSnapshotSignature(): string;
  setSnapshotSignature(nextSignature: string): void;
  getBridgeState(): BrowserHostBridgeState;
  setBridgeState(nextState: BrowserHostBridgeState): void;
  shouldSyncBridgeRuntimeCapabilitiesFromSnapshot(): boolean;
}) {
  const {
    listeners,
    bridgeListeners,
    getSnapshot,
    setSnapshot,
    getSnapshotSignature,
    setSnapshotSignature,
    getBridgeState,
    setBridgeState,
    shouldSyncBridgeRuntimeCapabilitiesFromSnapshot,
  } = params;

  const notifySnapshot = () => {
    notifyListeners(listeners, getSnapshot());
  };

  const notifyBridge = () => {
    notifyListeners(bridgeListeners, getBridgeState());
  };

  const applyBridgeState = (nextState: BrowserHostBridgeState) => {
    if (bridgeStateEquals(getBridgeState(), nextState)) {
      return;
    }
    setBridgeState(nextState);
    notifyBridge();
  };

  const markBridgeReady = (
    revision: number,
    phase: BrowserHostBridgeState["phase"] = "idle",
    clearError = true,
  ) => {
    const bridgeState = getBridgeState();
    applyBridgeState(
      createTransportBridgeState({
        phase,
        connected: true,
        lastError: clearError ? "" : bridgeState.lastError,
        lastSuccessRevision: revision,
        runtimeCapabilities: bridgeState.runtimeCapabilities,
        capabilities: bridgeState.capabilities,
      }),
    );
  };

  const markBridgeError = (
    errorText: string,
    phase: BrowserHostBridgeState["phase"],
    connected = false,
  ) => {
    const bridgeState = getBridgeState();
    applyBridgeState(
      createTransportBridgeState({
        phase,
        connected,
        lastError: errorText,
        lastSuccessRevision: bridgeState.lastSuccessRevision,
        runtimeCapabilities: bridgeState.runtimeCapabilities,
        capabilities: bridgeState.capabilities,
      }),
    );
  };

  const applySnapshot = (nextSnapshot: StateSnapshot) => {
    const bridgeState = getBridgeState();
    const syncRuntimeCapabilities =
      shouldSyncBridgeRuntimeCapabilitiesFromSnapshot();
    if (
      syncRuntimeCapabilities &&
      !runtimeCapabilitiesEqual(
        bridgeState.runtimeCapabilities,
        nextSnapshot.runtime_capabilities,
      )
    ) {
      applyBridgeState(
        createTransportBridgeState({
          ...bridgeState,
          runtimeCapabilities: syncRuntimeCapabilities
            ? nextSnapshot.runtime_capabilities
            : bridgeState.runtimeCapabilities,
        }),
      );
    }
    setSnapshot(nextSnapshot);
    const nextSignature = snapshotCacheSignature(nextSnapshot);
    if (nextSignature === getSnapshotSignature()) {
      return;
    }
    setSnapshotSignature(nextSignature);
    notifySnapshot();
  };

  return {
    applyBridgeState,
    applySnapshot,
    markBridgeError,
    markBridgeReady,
  };
}

function createBridgeTransportSession(initialBridgeState: Partial<BrowserHostBridgeState>) {
  let snapshot = makeNativePlaceholderSnapshot();
  let snapshotSignature = snapshotCacheSignature(snapshot);
  let bridgeState = createTransportBridgeState(initialBridgeState);
  const listeners = new Set<(nextSnapshot: StateSnapshot) => void>();
  const bridgeListeners = new Set<(state: BrowserHostBridgeState) => void>();
  let bridgeRuntimeCapabilitiesPinned = false;

  const controller = createTransportStateController({
    listeners,
    bridgeListeners,
    getSnapshot: () => snapshot,
    setSnapshot: (nextSnapshot) => {
      snapshot = nextSnapshot;
    },
    getSnapshotSignature: () => snapshotSignature,
    setSnapshotSignature: (nextSignature) => {
      snapshotSignature = nextSignature;
    },
    getBridgeState: () => bridgeState,
    setBridgeState: (nextState) => {
      bridgeState = nextState;
    },
    shouldSyncBridgeRuntimeCapabilitiesFromSnapshot: () =>
      !bridgeRuntimeCapabilitiesPinned,
  });

  return {
    controller,
    get snapshot() {
      return snapshot;
    },
    get bridgeState() {
      return bridgeState;
    },
    pinBridgeRuntimeCapabilities() {
      bridgeRuntimeCapabilitiesPinned = true;
    },
    subscribeSnapshot(listener: (nextSnapshot: StateSnapshot) => void) {
      return subscribeWithCurrentValue(listeners, listener, () => snapshot);
    },
    subscribeBridgeState(listener: (state: BrowserHostBridgeState) => void) {
      return subscribeWithCurrentValue(bridgeListeners, listener, () => bridgeState);
    },
  };
}

function resolveNativeBridge(
  hostWindow: Window | undefined,
): BrowserHostNativeBridge | null {
  if (hostWindow === undefined) {
    return null;
  }
  const bridge = ownWindowPropertyValue<BrowserHostNativeBridge>(
    hostWindow,
    "__MMLTK_NATIVE_BRIDGE__",
  );
  return bridge && typeof bridge.postMessage === "function" ? bridge : null;
}

function resolveWebSocketUrl(hostWindow: Window | undefined): string | null {
  if (hostWindow === undefined) {
    return null;
  }
  const injectedUrl = ownWindowPropertyValue<string>(
    hostWindow,
    "__MMLTK_BROWSER_WS_URL__",
  );
  if (typeof injectedUrl === "string" && injectedUrl.trim().length > 0) {
    return injectedUrl;
  }
  try {
    const url = new URL(hostWindow.location.href);
    const queryUrl = url.searchParams.get("mmltk_ws_url");
    return queryUrl !== null && queryUrl.trim().length > 0 ? queryUrl : null;
  } catch {
    return null;
  }
}

function parseBridgeStatePhase(
  value: unknown,
  fallback: BrowserHostBridgeState["phase"],
): BrowserHostBridgeState["phase"] {
  return value === "idle" || value === "polling" || value === "dispatch"
    ? value
    : fallback;
}

function parseLastSuccessRevision(
  value: unknown,
  fallback: number | null,
): number | null {
  if (value === null) {
    return null;
  }
  if (typeof value === "number" && Number.isFinite(value)) {
    return Math.round(value);
  }
  return fallback;
}

function parseBrowserHostBackend(
  value: unknown,
  fallback: BrowserHostBackend,
): BrowserHostBackend {
  return value === "cef" || value === "unknown"
    ? value
    : fallback;
}

function parseRuntimeCapabilityStatus(
  value: unknown,
  fallback: BrowserRuntimeCapabilityStatus,
): BrowserRuntimeCapabilityStatus {
  if (value === true) {
    return "available";
  }
  if (value === false) {
    return "unavailable";
  }
  return value === "available" || value === "unavailable" || value === "unknown"
    ? value
    : fallback;
}

function parseRuntimeCapabilities(
  value: unknown,
  fallback: BrowserRuntimeCapabilities,
): BrowserRuntimeCapabilities {
  if (!isRecord(value)) {
    return fallback;
  }
  const workspaceSurfaceBridge = parseRuntimeCapabilityStatus(
    value.workspaceSurfaceBridge ??
      value.workspace_surface_bridge,
    fallback.workspace_surface_bridge ?? "unknown",
  );
  return {
    host_backend: parseBrowserHostBackend(
      value.hostBackend ?? value.host_backend,
      fallback.host_backend,
    ),
    navigator_gpu: parseRuntimeCapabilityStatus(
      value.navigatorGpu ?? value.navigator_gpu,
      fallback.navigator_gpu,
    ),
    workspace_surface_bridge: workspaceSurfaceBridge,
    workspace_surface_zero_copy: parseRuntimeCapabilityStatus(
      value.workspaceSurfaceZeroCopy ?? value.workspace_surface_zero_copy,
      fallback.workspace_surface_zero_copy ?? "unknown",
    ),
  };
}

function parseCapabilityContractMap(
  value: unknown,
  fallback: BrowserHostCapabilityContractMap | undefined,
): BrowserHostCapabilityContractMap | undefined {
  if (!isRecord(value)) {
    return fallback;
  }
  return { ...value };
}

interface ParsedBridgeStateUpdate {
  state: BrowserHostBridgeState;
  hasRuntimeCapabilities: boolean;
}

function parseBridgeStateJsonText(
  jsonText: string,
  fallback: BrowserHostBridgeState,
): ParsedBridgeStateUpdate | null {
  let parsed: unknown;
  try {
    parsed = JSON.parse(jsonText);
  } catch {
    return null;
  }
  if (!isRecord(parsed)) {
    return null;
  }

  const rawRuntimeCapabilities =
    parsed.runtimeCapabilities ?? parsed.runtime_capabilities;
  const rawRuntimeCapabilitiesRecord = isRecord(rawRuntimeCapabilities)
    ? rawRuntimeCapabilities
    : null;
  const hasRuntimeCapabilities = rawRuntimeCapabilitiesRecord !== null;
  const runtimeCapabilities = parseRuntimeCapabilities(
    rawRuntimeCapabilities,
    fallback.runtimeCapabilities ?? kUnknownBrowserRuntimeCapabilities,
  );
  const capabilities = parseCapabilityContractMap(
    parsed.capabilities ??
      parsed.capabilityStatus ??
      parsed.capability_status ??
      parsed.capability_statuses,
    fallback.capabilities,
  );

  return {
    state: createTransportBridgeState({
      phase: parseBridgeStatePhase(parsed.phase, fallback.phase),
      connected:
        typeof parsed.connected === "boolean"
          ? parsed.connected
          : fallback.connected,
      lastError:
        typeof parsed.lastError === "string"
          ? parsed.lastError
          : fallback.lastError,
      lastSuccessRevision: parseLastSuccessRevision(
        parsed.lastSuccessRevision,
        fallback.lastSuccessRevision,
      ),
      runtimeCapabilities,
      capabilities,
    }),
    hasRuntimeCapabilities,
  };
}

function createNativeBridgeTransport(
  nativeBridge: BrowserHostNativeBridge,
): BrowserHostTransport {
  const session = createBridgeTransportSession({
    phase: "polling",
    connected: true,
  });
  const { applyBridgeState, applySnapshot, markBridgeError, markBridgeReady } =
    session.controller;

  const postBridgeMessage = (
    messageText: string,
    phase: BrowserHostBridgeState["phase"] | null,
  ): boolean => {
    if (phase !== null) {
      applyBridgeState(
        createTransportBridgeState({
          ...session.bridgeState,
          connected: true,
          phase,
        }),
      );
    }
    try {
      nativeBridge.postMessage(messageText);
      return true;
    } catch (error) {
      markBridgeError(
        errorTextFromUnknown(error),
        phase ?? session.bridgeState.phase,
        true,
      );
      return false;
    }
  };

  nativeBridge.deliverSnapshot = (jsonText: string) => {
    let parsed: unknown;
    try {
      parsed = JSON.parse(jsonText);
    } catch {
      markBridgeError("browser bridge delivered malformed snapshot JSON", "polling", true);
      return;
    }
    if (!isStateSnapshot(parsed)) {
      markBridgeError("browser bridge delivered malformed snapshot JSON", "polling", true);
      return;
    }

    applySnapshot(parsed);
    markBridgeReady(parsed.state_revision);
  };

  nativeBridge.setBridgeState = (jsonText: string) => {
    const parsedBridgeState = parseBridgeStateJsonText(jsonText, session.bridgeState);
    if (parsedBridgeState === null) {
      markBridgeError(
        "browser bridge delivered malformed bridge state JSON",
        session.bridgeState.phase,
        true,
      );
      return;
    }
    if (parsedBridgeState.hasRuntimeCapabilities) {
      session.pinBridgeRuntimeCapabilities();
    }
    applyBridgeState(parsedBridgeState.state);
  };

  nativeBridge.reportError = (messageText: string) => {
    const errorText = String(messageText ?? "").trim();
    if (errorText.length === 0) {
      return;
    }
    markBridgeError(errorText, session.bridgeState.phase, true);
  };

  const transport: BrowserHostTransport = {
    mode: "native",
    getSnapshot() {
      return session.snapshot;
    },
    getBridgeState() {
      return session.bridgeState;
    },
    dispatch(intent) {
      postBridgeMessage(JSON.stringify(intent), "dispatch");
    },
    subscribe(listener) {
      return session.subscribeSnapshot(listener);
    },
    subscribeBridgeState(listener) {
      return session.subscribeBridgeState(listener);
    },
  };
  return transport;
}

function makeWebSocketRuntimeCapabilitiesMessage(
  hostWindow: Window | undefined,
): string {
  const navigatorGpuAvailable =
    typeof navigator === "object" &&
    navigator !== null &&
    typeof navigator.gpu === "object" &&
    navigator.gpu !== null;
  const workspaceGpuBridge =
    hostWindow === undefined
      ? null
      : ownWindowPropertyValue<unknown>(
          hostWindow,
          "__MMLTK_WORKSPACE_GPU_BRIDGE__",
        );
  const workspaceGpuBridgeAvailable =
    isRecord(workspaceGpuBridge) &&
    typeof workspaceGpuBridge.acquireCurrentSurface === "function" &&
    typeof workspaceGpuBridge.importTexture === "function" &&
    typeof workspaceGpuBridge.releaseSurface === "function";
  return JSON.stringify({
    type: "host.runtime.capabilities",
    navigator_gpu: navigatorGpuAvailable,
    workspace_surface_bridge: workspaceGpuBridgeAvailable
      ? "available"
      : "unavailable",
    workspace_surface_zero_copy: workspaceGpuBridgeAvailable
      ? "unknown"
      : "unavailable",
  });
}

function createWebSocketBridgeTransport(
  hostWindow: Window,
  websocketUrl: string,
): BrowserHostTransport {
  const session = createBridgeTransportSession({
    phase: "polling",
    connected: false,
  });
  let socket: WebSocket | null = null;
  const pendingMessages: string[] = [];

  const { applyBridgeState, applySnapshot, markBridgeError, markBridgeReady } =
    session.controller;

  const sendText = (messageText: string) => {
    if (socket !== null && socket.readyState === WebSocket.OPEN) {
      socket.send(messageText);
      return;
    }
    pendingMessages.push(messageText);
  };

  const flushPendingMessages = () => {
    if (socket === null || socket.readyState !== WebSocket.OPEN) {
      return;
    }
    while (pendingMessages.length > 0) {
      socket.send(pendingMessages.shift() ?? "");
    }
  };

  const sendBridgeMessage = (
    messageText: string,
    phase: BrowserHostBridgeState["phase"] | null,
  ) => {
    if (phase !== null) {
      applyBridgeState(
        createTransportBridgeState({
          ...session.bridgeState,
          connected: true,
          phase,
        }),
      );
    }
    sendText(messageText);
  };

  const handleMessageText = (messageText: string) => {
    let parsed: unknown;
    try {
      parsed = JSON.parse(messageText);
    } catch {
      markBridgeError("browser websocket delivered malformed JSON", session.bridgeState.phase, true);
      return;
    }
    if (isStateSnapshot(parsed)) {
      applySnapshot(parsed);
      markBridgeReady(parsed.state_revision);
      return;
    }
    const surfaceReady = parseSurfaceReadyMessage(parsed);
    if (surfaceReady !== null) {
      applySnapshot({
        ...session.snapshot,
        workspace_surface: surfaceReady.surface,
      });
      markBridgeReady(session.snapshot.state_revision);
      return;
    }
    if (isRecord(parsed) && parsed.type === "bridge.error") {
      markBridgeError(
        typeof parsed.message === "string"
          ? parsed.message
          : "browser websocket bridge error",
        session.bridgeState.phase,
        true,
      );
      return;
    }
    const parsedBridgeState = parseBridgeStateJsonText(messageText, session.bridgeState);
    if (parsedBridgeState !== null) {
      if (parsedBridgeState.hasRuntimeCapabilities) {
        session.pinBridgeRuntimeCapabilities();
      }
      applyBridgeState(parsedBridgeState.state);
      return;
    }
    markBridgeError("browser websocket delivered unsupported message", session.bridgeState.phase, true);
  };

  const connect = () => {
    try {
      socket = new WebSocket(websocketUrl);
    } catch (error) {
      markBridgeError(errorTextFromUnknown(error), "polling", false);
      return;
    }
    socket.addEventListener("open", () => {
      applyBridgeState(
        createTransportBridgeState({
          ...session.bridgeState,
          phase: "polling",
          connected: true,
          lastError: "",
        }),
      );
      sendText(JSON.stringify({ type: "host.callbacks.ready" }));
      sendText(makeWebSocketRuntimeCapabilitiesMessage(hostWindow));
      flushPendingMessages();
    });
    socket.addEventListener("message", (event: MessageEvent) => {
      if (typeof event.data === "string") {
        handleMessageText(event.data);
      }
    });
    socket.addEventListener("error", () => {
      markBridgeError("browser websocket transport error", session.bridgeState.phase, false);
    });
    socket.addEventListener("close", () => {
      markBridgeError("browser websocket transport closed", "polling", false);
    });
  };

  connect();

  return {
    mode: "websocket",
    getSnapshot() {
      return session.snapshot;
    },
    getBridgeState() {
      return session.bridgeState;
    },
    dispatch(intent) {
      sendBridgeMessage(JSON.stringify(intent), "dispatch");
    },
    subscribe(listener) {
      return session.subscribeSnapshot(listener);
    },
    subscribeBridgeState(listener) {
      return session.subscribeBridgeState(listener);
    },
  };
}

export function createIntent(
  workflow: Workflow,
  intent: string,
  payload: Record<string, unknown>,
): IntentMessage {
  return {
    type: "intent",
    protocol_version: kProtocolVersion,
    request_id: requestCounter++,
    workflow,
    intent,
    payload,
  };
}

export function installBrowserHostTransportForTesting(
  targetWindow: Window,
  transport: BrowserHostTransport,
  options: InstallBrowserHostTransportForTestingOptions = {},
): () => void {
  const override: BrowserHostTransportOverrideForTesting = {
    transport,
    allowMockWithoutNativeBridge: options.allowMockWithoutNativeBridge === true,
  };
  transportOverridesForTesting.set(targetWindow, override);
  return () => {
    if (transportOverridesForTesting.get(targetWindow) === override) {
      transportOverridesForTesting.delete(targetWindow);
    }
  };
}

export function resolveBrowserHostTransport(): BrowserHostTransport {
  const hostWindow = typeof window === "undefined" ? undefined : window;
  const nativeBridge = resolveNativeBridge(hostWindow);
  if (hostWindow !== undefined) {
    const injectedOverride = transportOverridesForTesting.get(hostWindow);
    if (
      injectedOverride !== undefined &&
      (injectedOverride.transport.mode !== "mock" ||
        nativeBridge !== null ||
        injectedOverride.allowMockWithoutNativeBridge)
    ) {
      return injectedOverride.transport;
    }
  }

  if (hostWindow !== undefined) {
    const websocketUrl = resolveWebSocketUrl(hostWindow);
    if (websocketUrl !== null) {
      const cachedTransport = websocketTransportCache.get(hostWindow);
      if (cachedTransport !== undefined) {
        return cachedTransport;
      }
      const websocketTransport = createWebSocketBridgeTransport(
        hostWindow,
        websocketUrl,
      );
      websocketTransportCache.set(hostWindow, websocketTransport);
      return websocketTransport;
    }
  }

  if (hostWindow !== undefined && nativeBridge !== null) {
    const cachedTransport = nativeTransportCache.get(hostWindow);
    if (cachedTransport !== undefined) {
      return cachedTransport;
    }
    const nativeTransport = createNativeBridgeTransport(nativeBridge);
    nativeTransportCache.set(hostWindow, nativeTransport);
    return nativeTransport;
  }

  return createUnavailableNativeTransport();
}
