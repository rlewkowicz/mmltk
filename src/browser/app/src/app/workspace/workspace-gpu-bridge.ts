import type {
  BrowserHostWorkspaceGpuBridge,
  WorkspaceSurfaceInfo,
} from "../../host_api";
import {
  isRecord,
  kWorkspaceSurfaceId,
  ownWindowPropertyValue,
  workspaceSurfaceInfoFromValue,
} from "../../app_shared";

export type WorkspaceGpuBridgeBlockedReason =
  | "allocation_failure"
  | "gpu_bridge_missing"
  | "invalid_dmabuf_metadata"
  | "unsupported_modifier"
  | "missing_sync_metadata"
  | "shared_image_creation_failure"
  | "software_shared_image_rejected"
  | "shared_image_export_rejected"
  | "dawn_import_failure"
  | "renderer_import_rejected"
  | "release_failure"
  | "no_published_live_surface";

export type WorkspaceGpuBridgeOperation =
  | "acquireCurrentSurface"
  | "importTexture"
  | "releaseSurface"
  | "releaseRendererTexture";

export type WorkspaceGpuBridgeResultCode = string;

export interface WorkspaceSurfaceAcquireResult {
  operation: "acquireCurrentSurface";
  surface: WorkspaceSurfaceInfo | null;
  blockedReason: WorkspaceGpuBridgeBlockedReason | null;
  resultCode: WorkspaceGpuBridgeResultCode | null;
  resultDetail: string;
}

export interface WorkspaceTextureImportResult {
  operation: "importTexture";
  texture: GPUTexture | null;
  blockedReason: WorkspaceGpuBridgeBlockedReason | null;
  resultCode: WorkspaceGpuBridgeResultCode | null;
  resultDetail: string;
}

export interface WorkspaceSurfaceReleaseResult {
  operation: "releaseSurface" | "releaseRendererTexture";
  released: boolean;
  blockedReason: WorkspaceGpuBridgeBlockedReason | null;
  resultCode: WorkspaceGpuBridgeResultCode | null;
  resultDetail: string;
}

const kBlockedReasonCodePrefix = "workspace gpu bridge blocked [";

function resolveWorkspaceGpuBridge(): BrowserHostWorkspaceGpuBridge | null {
  const hostWindow = typeof window === "undefined" ? undefined : window;
  const bridge =
    ownWindowPropertyValue<BrowserHostWorkspaceGpuBridge>(
      hostWindow,
      "__MMLTK_WORKSPACE_GPU_BRIDGE__",
    ) ?? globalThis.__MMLTK_WORKSPACE_GPU_BRIDGE__;
  return bridge !== undefined &&
    bridge !== null &&
    typeof bridge.acquireCurrentSurface === "function" &&
    typeof bridge.importTexture === "function" &&
    typeof bridge.releaseSurface === "function" &&
    typeof bridge.releaseRendererTexture === "function"
    ? bridge
    : null;
}

export function workspaceGpuBridgeAvailable(): boolean {
  return resolveWorkspaceGpuBridge() !== null;
}

function bridgeBlockedReasonFromCode(code: string): WorkspaceGpuBridgeBlockedReason | null {
  switch (code.toLowerCase()) {
    case "allocation_failure":
      return "allocation_failure";
    case "gpu_bridge_missing":
      return "gpu_bridge_missing";
    case "invalid_dmabuf_metadata":
      return "invalid_dmabuf_metadata";
    case "unsupported_modifier":
      return "unsupported_modifier";
    case "missing_sync_metadata":
      return "missing_sync_metadata";
    case "shared_image_creation_failure":
      return "shared_image_creation_failure";
    case "software_shared_image_rejected":
      return "software_shared_image_rejected";
    case "shared_image_export_rejected":
      return "shared_image_export_rejected";
    case "dawn_import_failure":
      return "dawn_import_failure";
    case "renderer_import_rejected":
      return "renderer_import_rejected";
    case "release_failure":
      return "release_failure";
    case "no_published_live_surface":
      return "no_published_live_surface";
    default:
      return null;
  }
}

function bridgeErrorMessage(error: unknown): string {
  if (error instanceof Error && error.message.trim().length > 0) {
    return error.message;
  }
  const message = String(error);
  return message.trim().length > 0 ? message : "workspace gpu bridge operation failed";
}

function explicitBridgeFailureFromMessage(
  message: string,
  fallback: WorkspaceGpuBridgeBlockedReason,
): {
  blockedReason: WorkspaceGpuBridgeBlockedReason;
  resultCode: WorkspaceGpuBridgeResultCode;
  resultDetail: string;
} | null {
  const lowerMessage = message.toLowerCase();
  const codeStart = lowerMessage.indexOf(kBlockedReasonCodePrefix);
  if (codeStart < 0) {
    return null;
  }
  const reasonStart = codeStart + kBlockedReasonCodePrefix.length;
  const reasonEnd = lowerMessage.indexOf("]", reasonStart);
  if (reasonEnd <= reasonStart) {
    return null;
  }
  const resultCode = message.slice(reasonStart, reasonEnd);
  const detailStart = message.indexOf(":", reasonEnd);
  return {
    blockedReason: bridgeBlockedReasonFromCode(resultCode) ?? fallback,
    resultCode,
    resultDetail:
      detailStart >= 0 ? message.slice(detailStart + 1).trim() : message,
  };
}

function bridgeFailureFromError(
  error: unknown,
  fallback: WorkspaceGpuBridgeBlockedReason,
): {
  blockedReason: WorkspaceGpuBridgeBlockedReason;
  resultCode: WorkspaceGpuBridgeResultCode;
  resultDetail: string;
} {
  const message = bridgeErrorMessage(error);
  const explicitFailure = explicitBridgeFailureFromMessage(message, fallback);
  if (explicitFailure !== null) {
    return explicitFailure;
  }
  const lowerMessage = message.toLowerCase();
  let blockedReason = fallback;
  if (fallback === "renderer_import_rejected") {
    blockedReason = fallback;
  } else if (lowerMessage.includes("allocation")) {
    blockedReason = "allocation_failure";
  } else if (
    lowerMessage.includes("sharedimage") ||
    lowerMessage.includes("shared image") ||
    lowerMessage.includes("export")
  ) {
    blockedReason = "shared_image_export_rejected";
  } else if (lowerMessage.includes("import")) {
    blockedReason = "renderer_import_rejected";
  }
  return {
    blockedReason,
    resultCode: blockedReason,
    resultDetail: message,
  };
}

function stringFromReleaseValue(value: unknown): string {
  return typeof value === "string" ? value : "";
}

function releaseResultFromBridgeValue(
  operation: "releaseSurface" | "releaseRendererTexture",
  value: unknown,
): WorkspaceSurfaceReleaseResult {
  if (typeof value === "boolean") {
    return {
      operation,
      released: value,
      blockedReason: value ? null : "release_failure",
      resultCode: value ? "ok" : "release_failure",
      resultDetail: value ? "" : "workspace surface release rejected",
    };
  }
  if (value === undefined || value === null) {
    return {
      operation,
      released: true,
      blockedReason: null,
      resultCode: "ok",
      resultDetail: "",
    };
  }
  if (!isRecord(value)) {
    return {
      operation,
      released: false,
      blockedReason: "release_failure",
      resultCode: "release_failure",
      resultDetail: "workspace surface release returned an invalid result",
    };
  }

  const released = value.released === true;
  const resultCode =
    stringFromReleaseValue(value.resultCode ?? value.result_code) ||
    (released ? "ok" : "release_failure");
  const resultDetail = stringFromReleaseValue(
    value.resultDetail ?? value.result_detail,
  );
  return {
    operation,
    released,
    blockedReason: released
      ? null
      : bridgeBlockedReasonFromCode(resultCode) ?? "release_failure",
    resultCode,
    resultDetail:
      resultDetail.length > 0
        ? resultDetail
        : released
          ? ""
          : "workspace surface release rejected",
  };
}

export function acquireWorkspaceSurfaceResult(): WorkspaceSurfaceAcquireResult {
  const bridge = resolveWorkspaceGpuBridge();
  if (bridge === null) {
    return {
      operation: "acquireCurrentSurface",
      surface: null,
      blockedReason: "gpu_bridge_missing",
      resultCode: "gpu_bridge_missing",
      resultDetail: "workspace gpu bridge missing",
    };
  }
  try {
    const surface = workspaceSurfaceInfoFromValue(
      bridge.acquireCurrentSurface(kWorkspaceSurfaceId),
    );
    return {
      operation: "acquireCurrentSurface",
      surface,
      blockedReason: surface === null ? "no_published_live_surface" : null,
      resultCode: surface === null ? "no_published_live_surface" : null,
      resultDetail: surface === null ? "no published live workspace surface" : "",
    };
  } catch (error) {
    const failure = bridgeFailureFromError(error, "no_published_live_surface");
    return {
      operation: "acquireCurrentSurface",
      surface: null,
      blockedReason: failure.blockedReason,
      resultCode: failure.resultCode,
      resultDetail: failure.resultDetail,
    };
  }
}

export function acquireWorkspaceSurface(): WorkspaceSurfaceInfo | null {
  return acquireWorkspaceSurfaceResult().surface;
}

export function importWorkspaceSurfaceTextureResult(
  device: GPUDevice,
  surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision">,
): WorkspaceTextureImportResult {
  const bridge = resolveWorkspaceGpuBridge();
  if (bridge === null) {
    return {
      operation: "importTexture",
      texture: null,
      blockedReason: "gpu_bridge_missing",
      resultCode: "gpu_bridge_missing",
      resultDetail: "workspace gpu bridge missing",
    };
  }
  try {
    return {
      operation: "importTexture",
      texture: bridge.importTexture(device, surface.surfaceId, surface.revision),
      blockedReason: null,
      resultCode: null,
      resultDetail: "",
    };
  } catch (error) {
    const failure = bridgeFailureFromError(error, "renderer_import_rejected");
    return {
      operation: "importTexture",
      texture: null,
      blockedReason: failure.blockedReason,
      resultCode: failure.resultCode,
      resultDetail: failure.resultDetail,
    };
  }
}

export function importWorkspaceSurfaceTexture(
  device: GPUDevice,
  surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision">,
): GPUTexture | null {
  return importWorkspaceSurfaceTextureResult(device, surface).texture;
}

export function releaseWorkspaceSurface(
  surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision"> | null | undefined,
): WorkspaceSurfaceReleaseResult {
  if (surface === null || surface === undefined) {
    return {
      operation: "releaseSurface",
      released: true,
      blockedReason: null,
      resultCode: null,
      resultDetail: "",
    };
  }
  const bridge = resolveWorkspaceGpuBridge();
  if (bridge === null) {
    return {
      operation: "releaseSurface",
      released: false,
      blockedReason: "gpu_bridge_missing",
      resultCode: "gpu_bridge_missing",
      resultDetail: "workspace gpu bridge missing",
    };
  }
  try {
    return releaseResultFromBridgeValue(
      "releaseSurface",
      bridge.releaseSurface(surface.surfaceId, surface.revision),
    );
  } catch (error) {
    const failure = bridgeFailureFromError(error, "release_failure");
    return {
      operation: "releaseSurface",
      released: false,
      blockedReason: failure.blockedReason,
      resultCode: failure.resultCode,
      resultDetail: failure.resultDetail,
    };
  }
}

export function releaseWorkspaceRendererTexture(
  surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision"> | null | undefined,
): WorkspaceSurfaceReleaseResult {
  if (surface === null || surface === undefined) {
    return {
      operation: "releaseRendererTexture",
      released: true,
      blockedReason: null,
      resultCode: null,
      resultDetail: "",
    };
  }
  const bridge = resolveWorkspaceGpuBridge();
  if (bridge === null) {
    return {
      operation: "releaseRendererTexture",
      released: false,
      blockedReason: "gpu_bridge_missing",
      resultCode: "gpu_bridge_missing",
      resultDetail: "workspace gpu bridge missing",
    };
  }
  try {
    return releaseResultFromBridgeValue(
      "releaseRendererTexture",
      bridge.releaseRendererTexture(surface.surfaceId, surface.revision),
    );
  } catch (error) {
    const failure = bridgeFailureFromError(error, "release_failure");
    return {
      operation: "releaseRendererTexture",
      released: false,
      blockedReason: failure.blockedReason,
      resultCode: failure.resultCode,
      resultDetail: failure.resultDetail,
    };
  }
}
