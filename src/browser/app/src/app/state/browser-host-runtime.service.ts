import { computed, DestroyRef, inject, Injectable, signal } from "@angular/core";

import {
  capabilitySummaryText,
  type CapabilityStatusViewState,
  statusItemFromContract,
} from "../../app_shared";
import type {
  BrowserHostBridgeState,
  BrowserHostTransport,
  BrowserRuntimeCapabilities,
  BrowserRuntimeCapabilityStatus,
  IntentMessage,
  StateSnapshot,
  Workflow,
} from "../../host_api";
import { HOST_API_CONTRACT_HASH } from "../../host_api.generated";
import {
  runtimeWorkspaceSurfaceBridgeViability,
} from "../../runtime_frame_contract";
import { createIntent } from "../../transport";
import { BROWSER_HOST_TRANSPORT } from "./browser-shell.tokens";

export interface TransportStatusVm {
  label: string;
  runtimeLabel: string;
  readbackLabel: string;
  detail: string;
  severity: "mock" | "ok" | "warning" | "error";
  statusItems: CapabilityStatusViewState[];
}

function bridgeStateFromTransport(
  transport: BrowserHostTransport,
): BrowserHostBridgeState {
  if (typeof transport.getBridgeState === "function") {
    return transport.getBridgeState();
  }
  const snapshot = transport.getSnapshot();
  const snapshotRuntimeCapabilities = snapshot.runtime_capabilities;
  return {
    phase: "idle",
    connected: true,
    lastError: "",
    lastSuccessRevision: snapshot.state_revision,
    runtimeCapabilities: snapshotRuntimeCapabilities,
  };
}

function resolvedRuntimeCapabilities(
  snapshotCapabilities: BrowserRuntimeCapabilities,
  bridgeCapabilities: BrowserRuntimeCapabilities | undefined,
): BrowserRuntimeCapabilities {
  if (bridgeCapabilities === undefined) {
    return snapshotCapabilities;
  }
  return {
    host_backend:
      bridgeCapabilities.host_backend === "unknown"
        ? snapshotCapabilities.host_backend
        : bridgeCapabilities.host_backend,
    navigator_gpu:
      bridgeCapabilities.navigator_gpu === "unknown"
        ? snapshotCapabilities.navigator_gpu
        : bridgeCapabilities.navigator_gpu,
    workspace_surface_bridge:
      (bridgeCapabilities.workspace_surface_bridge ?? "unknown") === "unknown"
        ? snapshotCapabilities.workspace_surface_bridge
        : bridgeCapabilities.workspace_surface_bridge,
    workspace_surface_zero_copy:
      (bridgeCapabilities.workspace_surface_zero_copy ?? "unknown") === "unknown"
        ? snapshotCapabilities.workspace_surface_zero_copy
        : bridgeCapabilities.workspace_surface_zero_copy,
  };
}

function capabilityStateFromRuntimeStatus(
  status: BrowserRuntimeCapabilityStatus,
): CapabilityStatusViewState["status"] {
  switch (status) {
    case "available":
      return "ready";
    case "unavailable":
      return "blocked";
    default:
      return "pending";
  }
}

function backendStatusItem(
  runtimeCapabilities: BrowserRuntimeCapabilities,
  bridgeState: BrowserHostBridgeState,
): CapabilityStatusViewState {
  const backendLabel =
    runtimeCapabilities.host_backend === "cef" ? "CEF" : "Unknown";
  return statusItemFromContract(
    bridgeState,
    ["host_backend", "hostBackend", "backend"],
    {
      key: "host_backend",
      label: "Host Backend",
      status:
        runtimeCapabilities.host_backend === "unknown" ? "pending" : "ready",
      summary:
        runtimeCapabilities.host_backend === "unknown"
          ? "host backend pending"
          : `host backend ${backendLabel}`,
      detail:
        runtimeCapabilities.host_backend === "unknown"
          ? "The native bridge has not published a browser backend yet."
          : `${backendLabel} owns the embedded browser shell.`,
    },
  );
}

function navigatorGpuStatusItem(
  runtimeCapabilities: BrowserRuntimeCapabilities,
  bridgeState: BrowserHostBridgeState,
): CapabilityStatusViewState {
  return statusItemFromContract(
    bridgeState,
    ["navigator_gpu", "navigatorGpu", "webgpu"],
    {
      key: "navigator_gpu",
      label: "navigator.gpu",
      status: capabilityStateFromRuntimeStatus(runtimeCapabilities.navigator_gpu),
      summary:
        runtimeCapabilities.navigator_gpu === "available"
          ? "navigator.gpu available"
          : runtimeCapabilities.navigator_gpu === "unavailable"
            ? "navigator.gpu unavailable"
            : "navigator.gpu pending",
      detail:
        runtimeCapabilities.navigator_gpu === "available"
          ? "The browser runtime reports WebGPU support."
          : runtimeCapabilities.navigator_gpu === "unavailable"
            ? "The browser runtime does not expose navigator.gpu."
            : "The browser runtime has not reported WebGPU availability yet.",
    },
  );
}

function workspaceSurfaceBridgeStatusItem(
  runtimeCapabilities: BrowserRuntimeCapabilities,
  bridgeState: BrowserHostBridgeState,
): CapabilityStatusViewState {
  const bridgeViability =
    runtimeWorkspaceSurfaceBridgeViability(runtimeCapabilities);
  const fallback: CapabilityStatusViewState =
    bridgeViability === "ready"
      ? {
          key: "workspace_surface_bridge",
          label: "Workspace Surface Bridge",
          status: "ready",
          summary: "workspace surface bridge ready",
          detail:
            "The runtime reports a browser-owned workspace surface bridge backed by the dedicated GPU bridge.",
        }
      : runtimeCapabilities.host_backend === "unknown"
        ? {
            key: "workspace_surface_bridge",
            label: "Workspace Surface Bridge",
            status: "pending",
            summary: "workspace surface bridge pending",
            detail:
              "The native bridge has not published the workspace surface bridge capability yet.",
          }
        : {
            key: "workspace_surface_bridge",
            label: "Workspace Surface Bridge",
            status: "blocked",
            summary: "workspace surface bridge unavailable",
            detail:
              "The native runtime has not kept the workspace surface bridge available.",
          };

  return statusItemFromContract(
    bridgeState,
    [
      "workspace_surface_bridge",
      "workspaceSurfaceBridge",
    ],
    fallback,
  );
}

function workspaceSurfaceZeroCopyStatusItem(
  runtimeCapabilities: BrowserRuntimeCapabilities,
  bridgeState: BrowserHostBridgeState,
): CapabilityStatusViewState {
  const zeroCopyStatus =
    runtimeCapabilities.workspace_surface_zero_copy ?? "unknown";
  return statusItemFromContract(
    bridgeState,
    [
      "workspace_surface_zero_copy",
      "workspaceSurfaceZeroCopy",
      "frame_contract",
      "frameContract",
    ],
    {
      key: "workspace_surface_zero_copy",
      label: "Workspace Zero-Copy",
      status: capabilityStateFromRuntimeStatus(zeroCopyStatus),
      summary:
        zeroCopyStatus === "available"
          ? "workspace zero-copy available"
          : zeroCopyStatus === "unavailable"
            ? "workspace zero-copy unavailable"
            : "workspace zero-copy pending",
      detail:
        zeroCopyStatus === "available"
          ? "The workspace bridge reports strict zero-copy GPU texture aliasing."
          : zeroCopyStatus === "unavailable"
            ? "The workspace bridge cannot expose the workspace surface as a zero-copy GPU texture, so the dedicated canvas path is blocked."
            : "The workspace bridge has not reported zero-copy texture aliasing yet.",
    },
  );
}

function transportStatusVm(
  transport: BrowserHostTransport,
  bridgeState: BrowserHostBridgeState,
  runtimeCapabilities: BrowserRuntimeCapabilities,
): TransportStatusVm {
  const transportMode = transport.mode;
  if (transportMode === "mock") {
    const statusItems: CapabilityStatusViewState[] = [
      {
        key: "transport",
        label: "Transport",
        status: "ready",
        summary: "mock transport active",
        detail: "The browser app is running against the mock transport.",
      },
    ];
    return {
      label: "mock transport",
      runtimeLabel: "mock",
      readbackLabel: "n/a",
      detail: capabilitySummaryText(statusItems, "mock transport active"),
      severity: "mock",
      statusItems,
    };
  }

  const bridgeError = bridgeState.lastError.trim();
  const readbackLabel = "workspace surface";
  const bridgeLabel =
    transportMode === "websocket" ? "websocket bridge" : "native bridge";
  const transportStatus = statusItemFromContract(bridgeState, "transport", {
    key: "transport",
    label: "Transport",
    status:
      bridgeError.length > 0
        ? bridgeState.connected
          ? "fallback"
          : "blocked"
        : bridgeState.phase === "dispatch"
          ? "active"
          : bridgeState.phase === "polling" &&
              bridgeState.lastSuccessRevision === null
            ? "pending"
            : "ready",
    summary:
      bridgeError.length > 0
        ? bridgeState.connected
          ? `bridge degraded: ${bridgeError}`
          : `bridge blocked: ${bridgeError}`
        : bridgeState.phase === "dispatch"
          ? "bridge dispatch active"
        : bridgeState.phase === "polling" &&
              bridgeState.lastSuccessRevision === null
            ? `awaiting first ${bridgeLabel} snapshot`
            : `${bridgeLabel} attached`,
    detail:
      bridgeError.length > 0
        ? bridgeError
        : bridgeState.phase === "dispatch"
          ? `The ${bridgeLabel} is applying an intent.`
          : bridgeState.phase === "polling" &&
              bridgeState.lastSuccessRevision === null
            ? `The ${bridgeLabel} is waiting for its first snapshot.`
            : `The ${bridgeLabel} is attached.`,
  });
  const workspaceSurfaceBridgeStatus = workspaceSurfaceBridgeStatusItem(
    runtimeCapabilities,
    bridgeState,
  );
  const statusItems = [
    transportStatus,
    workspaceSurfaceBridgeStatus,
    workspaceSurfaceZeroCopyStatusItem(runtimeCapabilities, bridgeState),
  ];

  if (bridgeError.length > 0) {
    return {
      label: bridgeState.connected
        ? `${bridgeLabel} · warning`
        : `${bridgeLabel} · error`,
      runtimeLabel: bridgeState.connected ? "degraded" : "error",
      readbackLabel,
      detail: capabilitySummaryText(
        statusItems,
        bridgeState.connected
          ? `bridge degraded: ${bridgeError}`
          : `bridge blocked: ${bridgeError}`,
      ),
      severity: bridgeState.connected ? "warning" : "error",
      statusItems,
    };
  }

  if (bridgeState.phase === "dispatch") {
    return {
      label: `${bridgeLabel} · dispatch`,
      runtimeLabel: "dispatch",
      readbackLabel,
      detail: capabilitySummaryText(statusItems, "bridge dispatch active"),
      severity: "ok",
      statusItems,
    };
  }

  if (bridgeState.phase === "polling" && bridgeState.lastSuccessRevision === null) {
    return {
      label: `${bridgeLabel} · connecting`,
      runtimeLabel: "connecting",
      readbackLabel,
      detail: capabilitySummaryText(
        statusItems,
        `awaiting first ${bridgeLabel} snapshot`,
      ),
      severity: "ok",
      statusItems,
    };
  }

  return {
    label: bridgeLabel,
    runtimeLabel: "ready",
    readbackLabel,
    detail: capabilitySummaryText(statusItems, `${bridgeLabel} attached`),
    severity: "ok",
    statusItems,
  };
}

@Injectable({ providedIn: "root" })
export class BrowserHostRuntimeState {
  private readonly destroyRef = inject(DestroyRef);
  private transportSubscription: (() => void) | null = null;
  private transportBridgeSubscription: (() => void) | null = null;

  readonly transport = signal<BrowserHostTransport>(inject(BROWSER_HOST_TRANSPORT));
  readonly snapshot = signal<StateSnapshot>(this.transport().getSnapshot());
  readonly bridgeState = signal<BrowserHostBridgeState>(
    bridgeStateFromTransport(this.transport()),
  );
  readonly lastIntent = signal<IntentMessage | null>(null);

  readonly transportMode = computed(() => this.transport().mode);
  readonly runtimeCapabilities = computed(() =>
    resolvedRuntimeCapabilities(
      this.snapshot().runtime_capabilities,
      this.bridgeState().runtimeCapabilities,
    ),
  );
  readonly runtimeCapabilityStatus = computed<CapabilityStatusViewState[]>(() => [
    backendStatusItem(this.runtimeCapabilities(), this.bridgeState()),
    navigatorGpuStatusItem(this.runtimeCapabilities(), this.bridgeState()),
    workspaceSurfaceBridgeStatusItem(
      this.runtimeCapabilities(),
      this.bridgeState(),
    ),
    workspaceSurfaceZeroCopyStatusItem(
      this.runtimeCapabilities(),
      this.bridgeState(),
    ),
  ]);
  readonly transportStatus = computed<TransportStatusVm>(() =>
    transportStatusVm(
      this.transport(),
      this.bridgeState(),
      this.runtimeCapabilities(),
    ),
  );
  readonly contractHashMatched = computed(
    () => this.snapshot().contract_hash === HOST_API_CONTRACT_HASH,
  );
  readonly contractMismatchDetail = computed(() => {
    const backendHash = this.snapshot().contract_hash.trim();
    return backendHash.length === 0
      ? "The backend did not publish a browser UI contract hash."
      : `Backend contract ${backendHash} does not match UI contract ${HOST_API_CONTRACT_HASH}.`;
  });

  constructor() {
    this.bindTransport(this.transport());
    this.destroyRef.onDestroy(() => {
      this.unbindTransport();
    });
  }

  connectTransport(transport: BrowserHostTransport): void {
    if (transport === this.transport()) {
      return;
    }
    this.bindTransport(transport);
  }

  dispatch(
    workflow: Workflow,
    intent: string,
    payload: Record<string, unknown>,
  ): IntentMessage {
    const message = createIntent(workflow, intent, payload);
    this.lastIntent.set(message);
    this.transport().dispatch(message);
    return message;
  }

  private bindTransport(transport: BrowserHostTransport): void {
    this.unbindTransport();
    this.transport.set(transport);
    this.snapshot.set(transport.getSnapshot());
    this.bridgeState.set(bridgeStateFromTransport(transport));
    this.transportSubscription = transport.subscribe((nextSnapshot) => {
      this.snapshot.set(nextSnapshot);
    });
    if (typeof transport.subscribeBridgeState === "function") {
      this.transportBridgeSubscription = transport.subscribeBridgeState(
        (nextBridgeState) => {
          this.bridgeState.set(nextBridgeState);
        },
      );
    }
  }

  private unbindTransport(): void {
    this.transportSubscription?.();
    this.transportSubscription = null;
    this.transportBridgeSubscription?.();
    this.transportBridgeSubscription = null;
  }
}
