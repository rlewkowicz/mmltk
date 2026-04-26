import type { BrowserRuntimeCapabilities } from "./host_api";

export const kUnknownBrowserRuntimeCapabilities: BrowserRuntimeCapabilities = {
  host_backend: "unknown",
  navigator_gpu: "unknown",
  workspace_surface_bridge: "unknown",
  workspace_surface_zero_copy: "unknown",
};

function workspaceSurfaceBridgeStatus(
  runtimeCapabilities: BrowserRuntimeCapabilities,
): BrowserRuntimeCapabilities["workspace_surface_bridge"] {
  return runtimeCapabilities.workspace_surface_bridge ?? "unknown";
}

export function runtimeSupportsWorkspaceSurfaceBridge(
  runtimeCapabilities: BrowserRuntimeCapabilities,
): boolean {
  return workspaceSurfaceBridgeStatus(runtimeCapabilities) === "available";
}

export function runtimeWorkspaceSurfaceBridgePending(
  runtimeCapabilities: BrowserRuntimeCapabilities,
): boolean {
  return (
    workspaceSurfaceBridgeStatus(runtimeCapabilities) === "unknown" &&
    runtimeCapabilities.host_backend === "unknown"
  );
}

export type RuntimeWorkspaceSurfaceBridgeViability =
  | "ready"
  | "pending"
  | "blocked";

export function runtimeWorkspaceSurfaceBridgeViability(
  runtimeCapabilities: BrowserRuntimeCapabilities,
): RuntimeWorkspaceSurfaceBridgeViability {
  const reportedStatus = workspaceSurfaceBridgeStatus(runtimeCapabilities);
  if (reportedStatus === "available") {
    return "ready";
  }
  if (reportedStatus === "unavailable") {
    return "blocked";
  }
  if (runtimeWorkspaceSurfaceBridgePending(runtimeCapabilities)) {
    return "pending";
  }
  return "blocked";
}
