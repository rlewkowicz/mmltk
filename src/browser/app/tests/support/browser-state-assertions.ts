import * as assert from "node:assert/strict";

type ViewportCommitPayload = {
  center_x?: number;
  center_y?: number;
  zoom?: number;
  workspace_surface?: unknown;
};

export function assertSemanticViewportCommitPayload(payload: unknown): void {
  const viewportPayload = payload as ViewportCommitPayload | undefined;
  assert.equal(typeof viewportPayload?.center_x, "number");
  assert.equal(typeof viewportPayload?.center_y, "number");
  assert.equal(typeof viewportPayload?.zoom, "number");
  assert.equal("workspace_surface" in (viewportPayload ?? {}), false);
}

export function assertPrimaryChromeOmitsRawDiagnostics(values: readonly string[]): void {
  const forbidden = [
    "Workspace Path",
    "Overlay",
    "Render",
    "Canvas",
    "bridge pending",
    "webgpu adapter unavailable",
    "workspace surface bridge pending",
    "workspace zero-copy pending",
    "webgpu canvas overlay",
    "overlay pending",
  ].map((diagnostic) => diagnostic.toLowerCase());
  for (const value of values) {
    const normalizedValue = value.toLowerCase();
    for (const diagnostic of forbidden) {
      assert.equal(
        normalizedValue.includes(diagnostic),
        false,
        `primary workspace chrome leaked ${diagnostic}: ${value}`,
      );
    }
  }
}
