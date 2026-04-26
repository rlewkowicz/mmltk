import type {
  BrowserHostWorkspaceGpuBridge,
  WorkspaceSurfaceInfo,
} from "../../host_api";
import {
  kWorkspaceSurfaceId,
  ownWindowPropertyValue,
  workspaceSurfaceInfoFromValue,
} from "../../app_shared";

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
    typeof bridge.releaseSurface === "function"
    ? bridge
    : null;
}

export function workspaceGpuBridgeAvailable(): boolean {
  return resolveWorkspaceGpuBridge() !== null;
}

export function acquireWorkspaceSurface(): WorkspaceSurfaceInfo | null {
  const bridge = resolveWorkspaceGpuBridge();
  if (bridge === null) {
    return null;
  }
  try {
    return workspaceSurfaceInfoFromValue(
      bridge.acquireCurrentSurface(kWorkspaceSurfaceId),
    );
  } catch {
    return null;
  }
}

export function importWorkspaceSurfaceTexture(
  device: GPUDevice,
  surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision">,
): GPUTexture | null {
  const bridge = resolveWorkspaceGpuBridge();
  if (bridge === null) {
    return null;
  }
  try {
    return bridge.importTexture(device, surface.surfaceId, surface.revision);
  } catch {
    return null;
  }
}

export function releaseWorkspaceSurface(
  surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision"> | null | undefined,
): void {
  if (surface === null || surface === undefined) {
    return;
  }
  const bridge = resolveWorkspaceGpuBridge();
  if (bridge === null) {
    return;
  }
  try {
    bridge.releaseSurface(surface.surfaceId, surface.revision);
  } catch {
  }
}
