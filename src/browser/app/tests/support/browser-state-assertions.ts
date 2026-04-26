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
