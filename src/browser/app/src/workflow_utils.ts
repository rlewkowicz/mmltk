import type { SourceKind, Workflow } from "./host_api";

export type WorkflowSettingsKey = "predict" | "annotate";

export function workflowSettingsKey(workflow: Workflow): WorkflowSettingsKey | null {
  if (workflow === "predict" || workflow === "live") {
    return "predict";
  }
  if (workflow === "annotate") {
    return "annotate";
  }
  return null;
}

export function normalizeWorkflow(value: unknown): Workflow | null {
  if (
    value === "train" ||
    value === "validate" ||
    value === "predict" ||
    value === "annotate" ||
    value === "export" ||
    value === "live"
  ) {
    return value;
  }
  return null;
}

export function sourceKindNameFromValue(value: unknown): SourceKind {
  if (value === "compiled_dataset" || value === 0 || value === "0") {
    return "compiled_dataset";
  }
  if (value === "single_image" || value === 1 || value === "1") {
    return "single_image";
  }
  if (value === "image_folder" || value === 2 || value === "2") {
    return "image_folder";
  }
  if (value === "video_stream" || value === 3 || value === "3") {
    return "video_stream";
  }
  return "single_image";
}

export function remoteFamilyFlagsFromValue(value: unknown): boolean[] {
  const fallback = [true, true, true, true, true];
  if (!Array.isArray(value)) {
    return fallback;
  }
  return fallback.map((defaultValue, index) =>
    typeof value[index] === "boolean" ? value[index] : defaultValue,
  );
}
