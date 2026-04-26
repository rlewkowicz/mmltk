import {
  booleanFromValue,
  integerFromValue,
  isRecord,
  numberFromValue,
  stringFromValue,
  workflowLeafRecord,
  workflowRecordKey,
  workflowSectionRecord,
  workflowStateRoot,
  workflowSourceRecord,
} from "../../app_shared";
import type {
  FileDialogMode,
  SourceKind,
  StateSnapshot,
  Workflow,
} from "../../host_api";
import { sourceKindNameFromValue } from "../../workflow_utils";

type JsonRecord = Record<string, unknown>;

export type ShellDensity = "compact" | "balanced" | "comfortable";

export interface SourceDraft {
  kind: SourceKind;
  locator: string;
  recursive: boolean;
  deviceIndex: string;
  captureWidth: string;
  captureHeight: string;
  captureFps: string;
  v4l2BufferCount: string;
}

export interface ShellSettingsDraft {
  darkMode: boolean;
  uiScale: string;
  fontSize: string;
  secondaryFontSize: string;
  monoFontSize: string;
  propertyLabelWidth: string;
  cropEdgeHitHalfWidth: string;
  cropCornerHitSize: string;
  cropHandleRadius: string;
  density: ShellDensity;
}

export interface SelectOption<T extends string> {
  value: T;
  label: string;
}

export function draftWithField<T extends object, K extends keyof T>(
  current: T,
  field: K,
  value: T[K],
): T {
  return {
    ...current,
    [field]: value,
  };
}

export function hasSelectOptionValue<T extends string>(
  options: ReadonlyArray<SelectOption<T>>,
  value: string,
): value is T {
  return options.some((option) => option.value === value);
}

export interface FileDialogFilter {
  name: string;
  patterns: string[];
}

export interface SourceBrowseRequest {
  dialogId: string;
  mode: FileDialogMode;
  title: string;
  defaultPath: string;
  filters: FileDialogFilter[];
}

export interface FileDialogRequestLike {
  dialogId: string;
  mode: FileDialogMode;
  title: string;
  defaultPath: string;
  filters: ReadonlyArray<{
    name: string;
    patterns: ReadonlyArray<string>;
  }>;
}

export function fileDialogRequestPayload(request: FileDialogRequestLike): {
  dialog_id: string;
  mode: FileDialogMode;
  title: string;
  default_path: string;
  filters: FileDialogFilter[];
} {
  return {
    dialog_id: request.dialogId,
    mode: request.mode,
    title: request.title,
    default_path: request.defaultPath,
    filters: request.filters.map((filter) => ({
      name: filter.name,
      patterns: filter.patterns.slice(),
    })),
  };
}

const kImageFilePatterns = ["*.png", "*.jpg", "*.jpeg", "*.bmp", "*.webp"];
const kPredictSourceKinds: ReadonlyArray<SourceKind> = [
  "compiled_dataset",
  "single_image",
  "image_folder",
  "video_stream",
];
const kAnnotateSourceKinds: ReadonlyArray<SourceKind> = [
  "single_image",
  "image_folder",
  "video_stream",
];
const kShellDensityOptions: ReadonlyArray<SelectOption<ShellDensity>> = [
  { value: "compact", label: "Compact" },
  { value: "balanced", label: "Balanced" },
  { value: "comfortable", label: "Comfortable" },
];

function sourceKindPatchValue(kind: SourceKind): number {
  switch (kind) {
    case "compiled_dataset":
      return 0;
    case "single_image":
      return 1;
    case "image_folder":
      return 2;
    case "video_stream":
      return 3;
  }
}

function sourceLocatorFromRecord(
  workflowSource: JsonRecord | null,
  kind: SourceKind,
  fallback: string,
): string {
  if (workflowSource === null) {
    return fallback;
  }
  if (kind === "compiled_dataset") {
    return stringFromValue(workflowSource.compiled_path, fallback);
  }
  if (kind === "single_image") {
    return stringFromValue(workflowSource.single_image_path, fallback);
  }
  if (kind === "image_folder") {
    return stringFromValue(workflowSource.image_directory, fallback);
  }
  return fallback;
}

function densityFromValue(value: unknown): ShellDensity {
  if (value === "compact" || value === 0 || value === "0") {
    return "compact";
  }
  if (value === "comfortable" || value === 2 || value === "2") {
    return "comfortable";
  }
  return "balanced";
}

function densityPatchValue(density: ShellDensity): number {
  switch (density) {
    case "compact":
      return 0;
    case "balanced":
      return 1;
    case "comfortable":
      return 2;
  }
}

function formatIntegerString(value: unknown, fallback: number): string {
  return String(integerFromValue(value, fallback));
}

function formatFixedString(value: unknown, digits: number, fallback: number): string {
  return numberFromValue(value, fallback).toFixed(digits);
}

function clampIntegerInput(value: string, fallback: number, minValue: number): number {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    return fallback;
  }
  return Math.max(minValue, Math.round(parsed));
}

function clampUiScaleInput(value: string, fallback: number): number {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    return fallback;
  }
  return Math.min(1.75, Math.max(0.85, Number(parsed.toFixed(2))));
}

function clampFloatInput(
  value: string,
  fallback: number,
  minValue: number,
  maxValue: number,
  digits = 2,
): number {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    return fallback;
  }
  return Math.min(maxValue, Math.max(minValue, Number(parsed.toFixed(digits))));
}

export function sourceKindOptions(
  workflow: Workflow,
  currentKind?: SourceKind,
): ReadonlyArray<SelectOption<SourceKind>> {
  const baseKinds =
    workflow === "annotate"
      ? kAnnotateSourceKinds
      : workflow === "predict" || workflow === "live"
        ? kPredictSourceKinds
        : [];
  const kinds =
    currentKind !== undefined && !baseKinds.includes(currentKind)
      ? ([currentKind, ...baseKinds] as ReadonlyArray<SourceKind>)
      : baseKinds;
  return kinds.map((kind) => ({
    value: kind,
    label:
      kind === "compiled_dataset"
        ? "Compiled Dataset"
        : kind === "single_image"
          ? "Single Image"
          : kind === "image_folder"
            ? "Image Folder"
            : "Video Stream",
  }));
}

export function sourceEditingSupported(workflow: Workflow): boolean {
  return sourceKindOptions(workflow).length > 0;
}

export function sourceDraftFromSnapshot(
  snapshot: StateSnapshot,
  workflow: Workflow,
): SourceDraft {
  const workflowSource = workflowSourceRecord(snapshot, workflow);
  const source = snapshot.source;
  const kind = sourceKindNameFromValue(workflowSource?.kind ?? source.kind);
  return {
    kind,
    locator: sourceLocatorFromRecord(workflowSource, kind, source.locator),
    recursive: booleanFromValue(workflowSource?.recursive, source.recursive),
    deviceIndex: formatIntegerString(workflowSource?.device_index, source.device_index),
    captureWidth: formatIntegerString(workflowSource?.capture_width, source.capture_width),
    captureHeight: formatIntegerString(workflowSource?.capture_height, source.capture_height),
    captureFps: formatIntegerString(workflowSource?.capture_fps, source.capture_fps),
    v4l2BufferCount: formatIntegerString(
      workflowSource?.v4l2_buffer_count,
      source.v4l2_buffer_count,
    ),
  };
}

export function sourceDraftEquals(
  left: SourceDraft,
  right: SourceDraft,
): boolean {
  return (
    left.kind === right.kind &&
    left.locator === right.locator &&
    left.recursive === right.recursive &&
    left.deviceIndex === right.deviceIndex &&
    left.captureWidth === right.captureWidth &&
    left.captureHeight === right.captureHeight &&
    left.captureFps === right.captureFps &&
    left.v4l2BufferCount === right.v4l2BufferCount
  );
}

export function buildSourceSettingsPatch(
  workflow: Workflow,
  draft: SourceDraft,
  snapshot: StateSnapshot,
): JsonRecord {
  const workflowKey = workflowRecordKey(workflow);
  const fallback = sourceDraftFromSnapshot(snapshot, workflow);
  const locator = draft.locator.trim();
  const sourcePatch: JsonRecord = {
    kind: sourceKindPatchValue(draft.kind),
    recursive: draft.recursive,
    device_index: clampIntegerInput(draft.deviceIndex, Number(fallback.deviceIndex), 0),
    capture_width: clampIntegerInput(draft.captureWidth, Number(fallback.captureWidth), 1),
    capture_height: clampIntegerInput(
      draft.captureHeight,
      Number(fallback.captureHeight),
      1,
    ),
    capture_fps: clampIntegerInput(draft.captureFps, Number(fallback.captureFps), 1),
    v4l2_buffer_count: clampIntegerInput(
      draft.v4l2BufferCount,
      Number(fallback.v4l2BufferCount),
      1,
    ),
    compiled_path: "",
    single_image_path: "",
    image_directory: "",
  };

  if (draft.kind === "compiled_dataset") {
    sourcePatch.compiled_path = locator;
  } else if (draft.kind === "single_image") {
    sourcePatch.single_image_path = locator;
  } else if (draft.kind === "image_folder") {
    sourcePatch.image_directory = locator;
  }

  return {
    workflows: {
      [workflowKey]: {
        source: sourcePatch,
      },
    },
  };
}

export function sourceBrowseRequestForDraft(
  workflow: Workflow,
  draft: SourceDraft,
): SourceBrowseRequest | null {
  const workflowKey = workflowRecordKey(workflow);
  const defaultPath = draft.locator.trim();
  const supportsSourceDialog =
    workflowKey === "predict" || workflowKey === "annotate";
  if (draft.kind === "single_image" && supportsSourceDialog) {
    return {
      dialogId: `${workflowKey}.source.single_image`,
      mode: "open_file",
      title: workflowKey === "annotate" ? "Select annotation image" : "Select source image",
      defaultPath,
      filters: [
        {
          name: "Images",
          patterns: kImageFilePatterns.slice(),
        },
      ],
    };
  }
  if (draft.kind === "image_folder" && supportsSourceDialog) {
    return {
      dialogId: `${workflowKey}.source.image_folder`,
      mode: "open_folder",
      title: workflowKey === "annotate" ? "Select annotation folder" : "Select source folder",
      defaultPath,
      filters: [],
    };
  }
  return null;
}

export function shellDensityOptions(): ReadonlyArray<SelectOption<ShellDensity>> {
  return kShellDensityOptions;
}

export function defaultShellSettingsDraft(): ShellSettingsDraft {
  return {
    darkMode: false,
    uiScale: "1.00",
    fontSize: "16.0",
    secondaryFontSize: "14.0",
    monoFontSize: "14.0",
    propertyLabelWidth: "156",
    cropEdgeHitHalfWidth: "8.0",
    cropCornerHitSize: "20.0",
    cropHandleRadius: "6.0",
    density: "balanced",
  };
}

export function shellSettingsDraftFromSnapshot(
  snapshot: StateSnapshot,
): ShellSettingsDraft {
  const settings = isRecord(snapshot.settings_state) ? snapshot.settings_state : {};
  return {
    darkMode: booleanFromValue(settings.dark_mode, false),
    uiScale: formatFixedString(settings.ui_scale, 2, 1),
    fontSize: formatFixedString(settings.font_size, 1, 16),
    secondaryFontSize: formatFixedString(settings.secondary_font_size, 1, 14),
    monoFontSize: formatFixedString(settings.mono_font_size, 1, 14),
    propertyLabelWidth: formatFixedString(settings.property_label_width, 0, 156),
    cropEdgeHitHalfWidth: formatFixedString(settings.crop_edge_hit_half_width, 1, 8),
    cropCornerHitSize: formatFixedString(settings.crop_corner_hit_size, 1, 20),
    cropHandleRadius: formatFixedString(settings.crop_handle_radius, 1, 6),
    density: densityFromValue(settings.density),
  };
}

export function shellSettingsDraftEquals(
  left: ShellSettingsDraft,
  right: ShellSettingsDraft,
): boolean {
  return (
    left.uiScale === right.uiScale &&
    left.fontSize === right.fontSize &&
    left.secondaryFontSize === right.secondaryFontSize &&
    left.monoFontSize === right.monoFontSize &&
    left.propertyLabelWidth === right.propertyLabelWidth &&
    left.cropEdgeHitHalfWidth === right.cropEdgeHitHalfWidth &&
    left.cropCornerHitSize === right.cropCornerHitSize &&
    left.cropHandleRadius === right.cropHandleRadius &&
    left.density === right.density &&
    left.darkMode === right.darkMode
  );
}

export function buildShellSettingsPatch(
  draft: ShellSettingsDraft,
  snapshot: StateSnapshot,
): JsonRecord {
  const fallback = shellSettingsDraftFromSnapshot(snapshot);
  return {
    ui: {
      dark_mode: draft.darkMode,
      ui_scale: clampUiScaleInput(draft.uiScale, Number(fallback.uiScale)),
      font_size: clampFloatInput(draft.fontSize, Number(fallback.fontSize), 13, 28, 1),
      secondary_font_size: clampFloatInput(
        draft.secondaryFontSize,
        Number(fallback.secondaryFontSize),
        12,
        24,
        1,
      ),
      mono_font_size: clampFloatInput(
        draft.monoFontSize,
        Number(fallback.monoFontSize),
        12,
        24,
        1,
      ),
      property_label_width: clampFloatInput(
        draft.propertyLabelWidth,
        Number(fallback.propertyLabelWidth),
        110,
        260,
        0,
      ),
      crop_edge_hit_half_width: clampFloatInput(
        draft.cropEdgeHitHalfWidth,
        Number(fallback.cropEdgeHitHalfWidth),
        4,
        16,
        1,
      ),
      crop_corner_hit_size: clampFloatInput(
        draft.cropCornerHitSize,
        Number(fallback.cropCornerHitSize),
        12,
        32,
        1,
      ),
      crop_handle_radius: clampFloatInput(
        draft.cropHandleRadius,
        Number(fallback.cropHandleRadius),
        3,
        12,
        1,
      ),
      density: densityPatchValue(draft.density),
    },
  };
}

export type CompileMode = "none" | "selective" | "full";
export type ModelInput = "weights" | "onnx" | "tensorrt" | "none";
export type TrainTarget = "local" | "remote";
export type TrainInputMode = "weights" | "resume";
export type TrainOptimizer = "adamw" | "muon";
export type RemoteGpuFamilyKey = "a100" | "b200" | "h100" | "h200" | "l_series";

export interface TrainDraft {
  selectedPreset: string;
  trainCompiledPath: string;
  valCompiledPath: string;
  testCompiledPath: string;
  weightsPath: string;
  resumePath: string;
  outputDir: string;
  inputMode: TrainInputMode;
  executionTarget: TrainTarget;
  cpuAffinity: string;
  batchSize: string;
  valBatchSize: string;
  epochs: string;
  gradAccumSteps: string;
  evalMaxDets: string;
  printFreq: string;
  prefetchFactor: string;
  workers: string;
  lanes: string;
  compileMode: CompileMode;
  optimizer: TrainOptimizer;
  momentum: string;
  learningRate: string;
  lrEncoder: string;
  weightDecay: string;
  lrComponentDecay: string;
  encoderLayerDecay: string;
  warmupEpochs: string;
  warmupMomentum: string;
  lrMinFactor: string;
  clipMaxNorm: string;
  lrDrop: string;
  lrScheduler: string;
  amp: boolean;
  ema: boolean;
  freezeEncoder: boolean;
  progressBar: boolean;
  localDeviceIds: string;
  remoteFamilies: Record<RemoteGpuFamilyKey, boolean>;
  remoteContainerImage: string;
  remoteLaunchTemplate: string;
}

export interface ValidateDraft {
  compiledPath: string;
  sourceDir: string;
  onnxPath: string;
  tensorrtPath: string;
  saveEnginePath: string;
  reportJsonPath: string;
  split: string;
  evalOrder: string;
  resolution: string;
  limitImages: string;
  alignmentImages: string;
  evalMaxDets: string;
  batchSize: string;
  prefetchFactor: string;
  cpuAffinity: string;
  deviceId: string;
  workers: string;
  allowFp16: boolean;
  writeReportJson: boolean;
  recompile: boolean;
  profile: boolean;
}

export interface PredictDraft {
  selectedPreset: string;
  weightsPath: string;
  onnxPath: string;
  tensorrtPath: string;
  outputPath: string;
  backend: string;
  modelInput: ModelInput;
  batchSize: string;
  maxDetsPerImage: string;
  liveSplitCount: string;
  cpuAffinity: string;
  deviceId: string;
  workers: string;
  lanes: string;
  threshold: string;
  allowFp16: boolean;
  progressBar: boolean;
  compileMode: CompileMode;
}

export interface AnnotateDraft {
  weightsPath: string;
  onnxPath: string;
  tensorrtPath: string;
  outputDir: string;
  split: string;
  backend: string;
  modelInput: ModelInput;
  maxDetsPerImage: string;
  deviceId: string;
  threshold: string;
  allowFp16: boolean;
  fullFrame: boolean;
  compileMode: CompileMode;
}

export interface ExportDraft {
  weightsPath: string;
  onnxPath: string;
  outputPath: string;
  deviceId: string;
  opsetVersion: string;
  allowFp16: boolean;
  buildTensorRt: boolean;
  simplify: boolean;
}

export type TrainBrowseField =
  | "trainCompiledPath"
  | "valCompiledPath"
  | "testCompiledPath"
  | "weightsPath"
  | "resumePath"
  | "outputDir";

export type ValidateBrowseField =
  | "compiledPath"
  | "sourceDir"
  | "onnxPath"
  | "tensorrtPath"
  | "saveEnginePath"
  | "reportJsonPath";

export type PredictBrowseField =
  | "weightsPath"
  | "onnxPath"
  | "tensorrtPath"
  | "outputPath";

export type AnnotateBrowseField =
  | "weightsPath"
  | "onnxPath"
  | "tensorrtPath"
  | "outputDir";

export type ExportBrowseField = "weightsPath" | "onnxPath" | "outputPath";

const kCompileModeOptions: ReadonlyArray<SelectOption<CompileMode>> = [
  { value: "none", label: "None" },
  { value: "selective", label: "Selective" },
  { value: "full", label: "Full" },
];

const kPredictModelInputOptions: ReadonlyArray<SelectOption<ModelInput>> = [
  { value: "weights", label: "Weights" },
  { value: "onnx", label: "ONNX" },
  { value: "tensorrt", label: "TensorRT" },
];

const kAnnotateModelInputOptions: ReadonlyArray<SelectOption<ModelInput>> = [
  ...kPredictModelInputOptions,
  { value: "none", label: "None" },
];

const kTrainTargetOptions: ReadonlyArray<SelectOption<TrainTarget>> = [
  { value: "local", label: "Local" },
  { value: "remote", label: "Remote" },
];

const kTrainInputModeOptions: ReadonlyArray<SelectOption<TrainInputMode>> = [
  { value: "weights", label: "Weights" },
  { value: "resume", label: "Resume" },
];

const kTrainOptimizerOptions: ReadonlyArray<SelectOption<TrainOptimizer>> = [
  { value: "adamw", label: "AdamW" },
  { value: "muon", label: "Muon" },
];

const kRemoteGpuFamilyOptions: ReadonlyArray<SelectOption<RemoteGpuFamilyKey> & { index: number }> = [
  { value: "a100", label: "A100", index: 0 },
  { value: "b200", label: "B200", index: 1 },
  { value: "h100", label: "H100", index: 2 },
  { value: "h200", label: "H200", index: 3 },
  { value: "l_series", label: "L Series", index: 4 },
];

const kCompiledDatasetPatterns = ["*.mmltk", "*.bin"];
const kWeightsFilePatterns = ["*.pt", "*.pth", "*.ckpt", "*.safetensors"];
const kOnnxFilePatterns = ["*.onnx"];
const kTensorRtFilePatterns = ["*.engine", "*.trt"];
const kJsonFilePatterns = ["*.json"];

function compileModeFromValue(value: unknown): CompileMode {
  if (value === "none" || value === 0 || value === "0") {
    return "none";
  }
  if (value === "full" || value === 2 || value === "2") {
    return "full";
  }
  return "selective";
}

function compileModePatchValue(mode: CompileMode): number {
  switch (mode) {
    case "none":
      return 0;
    case "selective":
      return 1;
    case "full":
      return 2;
  }
}

function modelInputFromValue(value: unknown): ModelInput {
  if (value === "onnx" || value === 1 || value === "1") {
    return "onnx";
  }
  if (value === "tensorrt" || value === 2 || value === "2") {
    return "tensorrt";
  }
  if (value === "none" || value === 3 || value === "3") {
    return "none";
  }
  return "weights";
}

function modelInputPatchValue(modelInput: ModelInput): number {
  switch (modelInput) {
    case "weights":
      return 0;
    case "onnx":
      return 1;
    case "tensorrt":
      return 2;
    case "none":
      return 3;
  }
}

function trainTargetFromValue(value: unknown): TrainTarget {
  return value === "remote" || value === 1 || value === "1" ? "remote" : "local";
}

function trainTargetPatchValue(target: TrainTarget): number {
  return target === "remote" ? 1 : 0;
}

function trainInputModeFromValue(value: unknown): TrainInputMode {
  return value === "resume" || value === 1 || value === "1" ? "resume" : "weights";
}

function trainInputModePatchValue(inputMode: TrainInputMode): number {
  return inputMode === "resume" ? 1 : 0;
}

function trainOptimizerFromValue(value: unknown): TrainOptimizer {
  return value === "muon" || value === 1 || value === "1" ? "muon" : "adamw";
}

function trainOptimizerPatchValue(optimizer: TrainOptimizer): number {
  return optimizer === "muon" ? 1 : 0;
}

function formatThresholdString(value: unknown, fallback: number): string {
  return numberFromValue(value, fallback).toFixed(2);
}

function clampThresholdInput(value: string, fallback: number): number {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    return fallback;
  }
  return Math.min(1, Math.max(0, Number(parsed.toFixed(4))));
}

function selectedPresetFromSnapshot(snapshot: StateSnapshot): string {
  return stringFromValue(workflowStateRoot(snapshot)?.selected_preset, "rf-detr-seg-medium");
}

function joinIntegerList(values: ReadonlyArray<number>): string {
  return values.join(", ");
}

function parseRemoteFamilies(value: unknown): Record<RemoteGpuFamilyKey, boolean> {
  const entries = Array.isArray(value) ? value : [];
  const remoteFamilies = {
    a100: false,
    b200: false,
    h100: false,
    h200: false,
    l_series: false,
  };
  for (const option of kRemoteGpuFamilyOptions) {
    remoteFamilies[option.value] = option.index < entries.length ? entries[option.index] === true : false;
  }
  return remoteFamilies;
}

function remoteFamiliesArray(
  remoteFamilies: Record<RemoteGpuFamilyKey, boolean>,
): boolean[] {
  return kRemoteGpuFamilyOptions.map((option) => remoteFamilies[option.value] === true);
}

export function parseIntegerListInput(
  value: string,
  fallback: ReadonlyArray<number> = [],
): number[] {
  const parsed = value
    .split(",")
    .map((entry) => entry.trim())
    .filter((entry) => entry.length > 0)
    .map((entry) => Number(entry))
    .filter((entry) => Number.isFinite(entry) && entry >= 0)
    .map((entry) => Math.round(entry));
  const unique = Array.from(new Set(parsed));
  return unique.length > 0 ? unique : Array.from(new Set(fallback.filter((entry) => entry >= 0)));
}

function appendCurrentOption<T extends string>(
  options: ReadonlyArray<SelectOption<T>>,
  currentValue: T,
  label: string,
): ReadonlyArray<SelectOption<T>> {
  return options.some((option) => option.value === currentValue)
    ? options
    : [{ value: currentValue, label }, ...options];
}

export function compileModeOptions(
  currentValue?: CompileMode,
): ReadonlyArray<SelectOption<CompileMode>> {
  return currentValue === undefined
    ? kCompileModeOptions
    : appendCurrentOption(kCompileModeOptions, currentValue, currentValue);
}

export function predictModelInputOptions(
  currentValue?: ModelInput,
): ReadonlyArray<SelectOption<ModelInput>> {
  return currentValue === undefined
    ? kPredictModelInputOptions
    : appendCurrentOption(kPredictModelInputOptions, currentValue, currentValue);
}

export function annotateModelInputOptions(
  currentValue?: ModelInput,
): ReadonlyArray<SelectOption<ModelInput>> {
  return currentValue === undefined
    ? kAnnotateModelInputOptions
    : appendCurrentOption(kAnnotateModelInputOptions, currentValue, currentValue);
}

export function trainTargetOptions(): ReadonlyArray<SelectOption<TrainTarget>> {
  return kTrainTargetOptions;
}

export function trainInputModeOptions(): ReadonlyArray<SelectOption<TrainInputMode>> {
  return kTrainInputModeOptions;
}

export function trainOptimizerOptions(): ReadonlyArray<SelectOption<TrainOptimizer>> {
  return kTrainOptimizerOptions;
}

export function trainRemoteFamilyOptions():
  ReadonlyArray<SelectOption<RemoteGpuFamilyKey> & { index: number }> {
  return kRemoteGpuFamilyOptions;
}

export function trainDraftFromSnapshot(snapshot: StateSnapshot): TrainDraft {
  const root = workflowStateRoot(snapshot);
  const datasetPaths = workflowSectionRecord(snapshot, "train", "dataset_paths");
  const artifacts = workflowSectionRecord(snapshot, "train", "model_artifacts");
  const execution = workflowSectionRecord(snapshot, "train", "execution");
  const training = workflowSectionRecord(snapshot, "train", "training");
  return {
    selectedPreset: selectedPresetFromSnapshot(snapshot),
    trainCompiledPath: stringFromValue(datasetPaths?.train_compiled_path),
    valCompiledPath: stringFromValue(datasetPaths?.val_compiled_path),
    testCompiledPath: stringFromValue(datasetPaths?.test_compiled_path),
    weightsPath: stringFromValue(artifacts?.weights_path),
    resumePath: stringFromValue(training?.resume_path),
    outputDir: stringFromValue(training?.output_dir),
    inputMode: trainInputModeFromValue(training?.input_mode),
    executionTarget: trainTargetFromValue(training?.execution_target),
    cpuAffinity: stringFromValue(execution?.cpu_affinity),
    batchSize: formatIntegerString(training?.batch_size, 2),
    valBatchSize: formatIntegerString(training?.val_batch_size, 0),
    epochs: formatIntegerString(training?.epochs, 12),
    gradAccumSteps: formatIntegerString(training?.grad_accum_steps, 1),
    evalMaxDets: formatIntegerString(training?.eval_max_dets, 500),
    printFreq: formatIntegerString(training?.print_freq, 100),
    prefetchFactor: formatIntegerString(training?.prefetch_factor, 2),
    workers: formatIntegerString(execution?.workers, 8),
    lanes: formatIntegerString(execution?.lanes, 0),
    compileMode: compileModeFromValue(execution?.compile_mode),
    optimizer: trainOptimizerFromValue(training?.optimizer),
    momentum: formatFixedString(training?.momentum, 4, 0.95),
    learningRate: formatFixedString(training?.lr, 6, 0.0001),
    lrEncoder: formatFixedString(training?.lr_encoder, 6, 0.00015),
    weightDecay: formatFixedString(training?.weight_decay, 6, 0.0001),
    lrComponentDecay: formatFixedString(training?.lr_component_decay, 4, 0.7),
    encoderLayerDecay: formatFixedString(training?.encoder_layer_decay, 4, 0.8),
    warmupEpochs: formatFixedString(training?.warmup_epochs, 4, 0),
    warmupMomentum: formatFixedString(training?.warmup_momentum, 4, 0),
    lrMinFactor: formatFixedString(training?.lr_min_factor, 4, 0),
    clipMaxNorm: formatFixedString(training?.clip_max_norm, 4, 0.1),
    lrDrop: formatIntegerString(training?.lr_drop, 100),
    lrScheduler: stringFromValue(training?.lr_scheduler, "step"),
    amp: booleanFromValue(training?.amp, true),
    ema: booleanFromValue(training?.use_ema, false),
    freezeEncoder: booleanFromValue(training?.freeze_encoder, false),
    progressBar: booleanFromValue(execution?.progress_bar, false),
    localDeviceIds: joinIntegerList(
      Array.isArray(training?.local_device_ids)
        ? training.local_device_ids
            .map((entry) => Number(entry))
            .filter((entry) => Number.isFinite(entry) && entry >= 0)
            .map((entry) => Math.round(entry))
        : [0],
    ),
    remoteFamilies: parseRemoteFamilies(training?.remote_family_enabled),
    remoteContainerImage: stringFromValue(training?.remote_container_image),
    remoteLaunchTemplate: stringFromValue(training?.remote_launch_template),
  };
}

export function trainDraftEquals(left: TrainDraft, right: TrainDraft): boolean {
  return (
    left.selectedPreset === right.selectedPreset &&
    left.trainCompiledPath === right.trainCompiledPath &&
    left.valCompiledPath === right.valCompiledPath &&
    left.testCompiledPath === right.testCompiledPath &&
    left.weightsPath === right.weightsPath &&
    left.resumePath === right.resumePath &&
    left.outputDir === right.outputDir &&
    left.inputMode === right.inputMode &&
    left.executionTarget === right.executionTarget &&
    left.cpuAffinity === right.cpuAffinity &&
    left.batchSize === right.batchSize &&
    left.valBatchSize === right.valBatchSize &&
    left.epochs === right.epochs &&
    left.gradAccumSteps === right.gradAccumSteps &&
    left.evalMaxDets === right.evalMaxDets &&
    left.printFreq === right.printFreq &&
    left.prefetchFactor === right.prefetchFactor &&
    left.workers === right.workers &&
    left.lanes === right.lanes &&
    left.compileMode === right.compileMode &&
    left.optimizer === right.optimizer &&
    left.momentum === right.momentum &&
    left.learningRate === right.learningRate &&
    left.lrEncoder === right.lrEncoder &&
    left.weightDecay === right.weightDecay &&
    left.lrComponentDecay === right.lrComponentDecay &&
    left.encoderLayerDecay === right.encoderLayerDecay &&
    left.warmupEpochs === right.warmupEpochs &&
    left.warmupMomentum === right.warmupMomentum &&
    left.lrMinFactor === right.lrMinFactor &&
    left.clipMaxNorm === right.clipMaxNorm &&
    left.lrDrop === right.lrDrop &&
    left.lrScheduler === right.lrScheduler &&
    left.amp === right.amp &&
    left.ema === right.ema &&
    left.freezeEncoder === right.freezeEncoder &&
    left.progressBar === right.progressBar &&
    left.localDeviceIds === right.localDeviceIds &&
    kRemoteGpuFamilyOptions.every(
      (option) => left.remoteFamilies[option.value] === right.remoteFamilies[option.value],
    ) &&
    left.remoteContainerImage === right.remoteContainerImage &&
    left.remoteLaunchTemplate === right.remoteLaunchTemplate
  );
}

export function buildTrainSettingsPatch(
  draft: TrainDraft,
  snapshot: StateSnapshot,
): JsonRecord {
  const fallback = trainDraftFromSnapshot(snapshot);
  return {
    selected_preset: draft.selectedPreset.trim() || fallback.selectedPreset,
    workflows: {
      train: {
        dataset_paths: {
          train_compiled_path: draft.trainCompiledPath.trim(),
          val_compiled_path: draft.valCompiledPath.trim(),
          test_compiled_path: draft.testCompiledPath.trim(),
        },
        model_artifacts: {
          weights_path: draft.weightsPath.trim(),
        },
        execution: {
          cpu_affinity: draft.cpuAffinity.trim(),
          workers: clampIntegerInput(draft.workers, Number(fallback.workers), 0),
          lanes: clampIntegerInput(draft.lanes, Number(fallback.lanes), 0),
          progress_bar: draft.progressBar,
          compile_mode: compileModePatchValue(draft.compileMode),
        },
        training: {
          output_dir: draft.outputDir.trim(),
          resume_path: draft.resumePath.trim(),
          input_mode: trainInputModePatchValue(draft.inputMode),
          execution_target: trainTargetPatchValue(draft.executionTarget),
          batch_size: clampIntegerInput(draft.batchSize, Number(fallback.batchSize), 1),
          val_batch_size: clampIntegerInput(
            draft.valBatchSize,
            Number(fallback.valBatchSize),
            0,
          ),
          epochs: clampIntegerInput(draft.epochs, Number(fallback.epochs), 1),
          grad_accum_steps: clampIntegerInput(
            draft.gradAccumSteps,
            Number(fallback.gradAccumSteps),
            1,
          ),
          eval_max_dets: clampIntegerInput(
            draft.evalMaxDets,
            Number(fallback.evalMaxDets),
            1,
          ),
          print_freq: clampIntegerInput(draft.printFreq, Number(fallback.printFreq), 1),
          prefetch_factor: clampIntegerInput(
            draft.prefetchFactor,
            Number(fallback.prefetchFactor),
            0,
          ),
          optimizer: trainOptimizerPatchValue(draft.optimizer),
          momentum: clampFloatInput(draft.momentum, Number(fallback.momentum), 0, 1, 6),
          lr: clampFloatInput(draft.learningRate, Number(fallback.learningRate), 0, 1, 8),
          lr_encoder: clampFloatInput(draft.lrEncoder, Number(fallback.lrEncoder), 0, 1, 8),
          weight_decay: clampFloatInput(
            draft.weightDecay,
            Number(fallback.weightDecay),
            0,
            1,
            8,
          ),
          lr_component_decay: clampFloatInput(
            draft.lrComponentDecay,
            Number(fallback.lrComponentDecay),
            0,
            1,
            6,
          ),
          encoder_layer_decay: clampFloatInput(
            draft.encoderLayerDecay,
            Number(fallback.encoderLayerDecay),
            0,
            1,
            6,
          ),
          warmup_epochs: clampFloatInput(
            draft.warmupEpochs,
            Number(fallback.warmupEpochs),
            0,
            10000,
            6,
          ),
          warmup_momentum: clampFloatInput(
            draft.warmupMomentum,
            Number(fallback.warmupMomentum),
            0,
            1,
            6,
          ),
          lr_min_factor: clampFloatInput(
            draft.lrMinFactor,
            Number(fallback.lrMinFactor),
            0,
            1,
            6,
          ),
          clip_max_norm: clampFloatInput(
            draft.clipMaxNorm,
            Number(fallback.clipMaxNorm),
            0,
            1000000,
            6,
          ),
          lr_drop: clampIntegerInput(draft.lrDrop, Number(fallback.lrDrop), 0),
          lr_scheduler: draft.lrScheduler.trim(),
          amp: draft.amp,
          use_ema: draft.ema,
          freeze_encoder: draft.freezeEncoder,
          local_device_ids: parseIntegerListInput(draft.localDeviceIds),
          remote_family_enabled: remoteFamiliesArray(draft.remoteFamilies),
          remote_container_image: draft.remoteContainerImage.trim(),
          remote_launch_template: draft.remoteLaunchTemplate.trim(),
        },
      },
    },
  };
}

export function trainBrowseRequestForField(
  field: TrainBrowseField,
  draft: TrainDraft,
): SourceBrowseRequest {
  switch (field) {
    case "trainCompiledPath":
      return {
        dialogId: "train.dataset.train_compiled_path",
        mode: "open_file",
        title: "Choose train compiled dataset",
        defaultPath: draft.trainCompiledPath.trim(),
        filters: [{ name: "Compiled datasets", patterns: kCompiledDatasetPatterns.slice() }],
      };
    case "valCompiledPath":
      return {
        dialogId: "train.dataset.val_compiled_path",
        mode: "open_file",
        title: "Choose validation compiled dataset",
        defaultPath: draft.valCompiledPath.trim(),
        filters: [{ name: "Compiled datasets", patterns: kCompiledDatasetPatterns.slice() }],
      };
    case "testCompiledPath":
      return {
        dialogId: "train.dataset.test_compiled_path",
        mode: "open_file",
        title: "Choose test compiled dataset",
        defaultPath: draft.testCompiledPath.trim(),
        filters: [{ name: "Compiled datasets", patterns: kCompiledDatasetPatterns.slice() }],
      };
    case "weightsPath":
      return {
        dialogId: "train.model.weights",
        mode: "open_file",
        title: "Choose training weights",
        defaultPath: draft.weightsPath.trim(),
        filters: [{ name: "Weights", patterns: kWeightsFilePatterns.slice() }],
      };
    case "resumePath":
      return {
        dialogId: "train.training.resume_path",
        mode: "open_file",
        title: "Choose checkpoint to resume",
        defaultPath: draft.resumePath.trim(),
        filters: [{ name: "Checkpoints", patterns: kWeightsFilePatterns.slice() }],
      };
    case "outputDir":
      return {
        dialogId: "train.training.output_dir",
        mode: "open_folder",
        title: "Choose training output directory",
        defaultPath: draft.outputDir.trim(),
        filters: [],
      };
  }
}

export function validateDraftFromSnapshot(snapshot: StateSnapshot): ValidateDraft {
  const datasetPaths = workflowSectionRecord(snapshot, "validate", "dataset_paths");
  const artifacts = workflowSectionRecord(snapshot, "validate", "model_artifacts");
  const execution = workflowSectionRecord(snapshot, "validate", "execution");
  const validation = workflowSectionRecord(snapshot, "validate", "validation");
  return {
    compiledPath: stringFromValue(datasetPaths?.compiled_path),
    sourceDir: stringFromValue(datasetPaths?.source_dir),
    onnxPath: stringFromValue(artifacts?.onnx_path),
    tensorrtPath: stringFromValue(artifacts?.tensorrt_path),
    saveEnginePath: stringFromValue(validation?.save_engine_path),
    reportJsonPath: stringFromValue(validation?.report_json_path),
    split: stringFromValue(validation?.split, "val"),
    evalOrder: stringFromValue(validation?.eval_order),
    resolution: formatIntegerString(validation?.resolution, 960),
    limitImages: formatIntegerString(validation?.limit_images, 0),
    alignmentImages: formatIntegerString(validation?.alignment_images, 0),
    evalMaxDets: formatIntegerString(validation?.eval_max_dets, 500),
    batchSize: formatIntegerString(validation?.batch_size, 1),
    prefetchFactor: formatIntegerString(validation?.prefetch_factor, 1),
    cpuAffinity: stringFromValue(execution?.cpu_affinity),
    deviceId: formatIntegerString(execution?.device_id, 0),
    workers: formatIntegerString(execution?.workers, 0),
    allowFp16: booleanFromValue(execution?.allow_fp16, true),
    writeReportJson: booleanFromValue(validation?.write_report_json, true),
    recompile: booleanFromValue(validation?.recompile, false),
    profile: booleanFromValue(validation?.profile, false),
  };
}

export function validateDraftEquals(
  left: ValidateDraft,
  right: ValidateDraft,
): boolean {
  return (
    left.compiledPath === right.compiledPath &&
    left.sourceDir === right.sourceDir &&
    left.onnxPath === right.onnxPath &&
    left.tensorrtPath === right.tensorrtPath &&
    left.saveEnginePath === right.saveEnginePath &&
    left.reportJsonPath === right.reportJsonPath &&
    left.split === right.split &&
    left.evalOrder === right.evalOrder &&
    left.resolution === right.resolution &&
    left.limitImages === right.limitImages &&
    left.alignmentImages === right.alignmentImages &&
    left.evalMaxDets === right.evalMaxDets &&
    left.batchSize === right.batchSize &&
    left.prefetchFactor === right.prefetchFactor &&
    left.cpuAffinity === right.cpuAffinity &&
    left.deviceId === right.deviceId &&
    left.workers === right.workers &&
    left.allowFp16 === right.allowFp16 &&
    left.writeReportJson === right.writeReportJson &&
    left.recompile === right.recompile &&
    left.profile === right.profile
  );
}

export function buildValidateSettingsPatch(
  draft: ValidateDraft,
  snapshot: StateSnapshot,
): JsonRecord {
  const fallback = validateDraftFromSnapshot(snapshot);
  return {
    workflows: {
      validate: {
        dataset_paths: {
          compiled_path: draft.compiledPath.trim(),
          source_dir: draft.sourceDir.trim(),
        },
        model_artifacts: {
          onnx_path: draft.onnxPath.trim(),
          tensorrt_path: draft.tensorrtPath.trim(),
        },
        execution: {
          cpu_affinity: draft.cpuAffinity.trim(),
          device_id: clampIntegerInput(draft.deviceId, Number(fallback.deviceId), 0),
          workers: clampIntegerInput(draft.workers, Number(fallback.workers), 0),
          allow_fp16: draft.allowFp16,
        },
        validation: {
          save_engine_path: draft.saveEnginePath.trim(),
          report_json_path: draft.reportJsonPath.trim(),
          split: draft.split.trim(),
          eval_order: draft.evalOrder.trim(),
          resolution: clampIntegerInput(draft.resolution, Number(fallback.resolution), 1),
          limit_images: clampIntegerInput(draft.limitImages, Number(fallback.limitImages), 0),
          alignment_images: clampIntegerInput(
            draft.alignmentImages,
            Number(fallback.alignmentImages),
            0,
          ),
          eval_max_dets: clampIntegerInput(
            draft.evalMaxDets,
            Number(fallback.evalMaxDets),
            1,
          ),
          batch_size: clampIntegerInput(draft.batchSize, Number(fallback.batchSize), 1),
          prefetch_factor: clampIntegerInput(
            draft.prefetchFactor,
            Number(fallback.prefetchFactor),
            0,
          ),
          recompile: draft.recompile,
          profile: draft.profile,
          write_report_json: draft.writeReportJson,
        },
      },
    },
  };
}

export function validateBrowseRequestForField(
  field: ValidateBrowseField,
  draft: ValidateDraft,
): SourceBrowseRequest {
  switch (field) {
    case "compiledPath":
      return {
        dialogId: "validate.dataset.compiled_path",
        mode: "open_file",
        title: "Choose validation compiled dataset",
        defaultPath: draft.compiledPath.trim(),
        filters: [{ name: "Compiled datasets", patterns: kCompiledDatasetPatterns.slice() }],
      };
    case "sourceDir":
      return {
        dialogId: "validate.dataset.source_dir",
        mode: "open_folder",
        title: "Choose validation source root",
        defaultPath: draft.sourceDir.trim(),
        filters: [],
      };
    case "onnxPath":
      return {
        dialogId: "validate.model.onnx",
        mode: "open_file",
        title: "Choose validation ONNX",
        defaultPath: draft.onnxPath.trim(),
        filters: [{ name: "ONNX", patterns: kOnnxFilePatterns.slice() }],
      };
    case "tensorrtPath":
      return {
        dialogId: "validate.model.tensorrt",
        mode: "open_file",
        title: "Choose validation TensorRT engine",
        defaultPath: draft.tensorrtPath.trim(),
        filters: [{ name: "TensorRT", patterns: kTensorRtFilePatterns.slice() }],
      };
    case "saveEnginePath":
      return {
        dialogId: "validate.output.save_engine_path",
        mode: "save_file",
        title: "Choose validation engine output",
        defaultPath: draft.saveEnginePath.trim(),
        filters: [{ name: "TensorRT", patterns: kTensorRtFilePatterns.slice() }],
      };
    case "reportJsonPath":
      return {
        dialogId: "validate.report_json_path",
        mode: "save_file",
        title: "Choose validation report path",
        defaultPath: draft.reportJsonPath.trim(),
        filters: [{ name: "JSON", patterns: kJsonFilePatterns.slice() }],
      };
  }
}

export function predictDraftFromSnapshot(
  snapshot: StateSnapshot,
  workflow: Workflow = "predict",
): PredictDraft {
  const workflowKey = workflowRecordKey(workflow);
  const artifacts = workflowSectionRecord(snapshot, workflow, "model_artifacts");
  const execution = workflowSectionRecord(snapshot, workflow, "execution");
  const predict = workflowLeafRecord(snapshot, workflow);
  return {
    selectedPreset: selectedPresetFromSnapshot(snapshot),
    weightsPath: stringFromValue(artifacts?.weights_path),
    onnxPath: stringFromValue(artifacts?.onnx_path),
    tensorrtPath: stringFromValue(artifacts?.tensorrt_path),
    outputPath: stringFromValue(predict?.output_path),
    backend: stringFromValue(predict?.backend, "auto"),
    modelInput: modelInputFromValue(predict?.model_input),
    batchSize: formatIntegerString(predict?.batch_size, 1),
    maxDetsPerImage: formatIntegerString(predict?.max_dets_per_image, 500),
    liveSplitCount: formatIntegerString(predict?.live_split_count, 1),
    cpuAffinity: stringFromValue(execution?.cpu_affinity),
    deviceId: formatIntegerString(execution?.device_id, 0),
    workers: formatIntegerString(execution?.workers, 0),
    lanes: formatIntegerString(execution?.lanes, 0),
    threshold: formatThresholdString(predict?.threshold, 0.25),
    allowFp16: booleanFromValue(execution?.allow_fp16, true),
    progressBar: booleanFromValue(execution?.progress_bar, false),
    compileMode: compileModeFromValue(execution?.compile_mode),
  };
}

export function predictDraftEquals(left: PredictDraft, right: PredictDraft): boolean {
  return (
    left.selectedPreset === right.selectedPreset &&
    left.weightsPath === right.weightsPath &&
    left.onnxPath === right.onnxPath &&
    left.tensorrtPath === right.tensorrtPath &&
    left.outputPath === right.outputPath &&
    left.backend === right.backend &&
    left.modelInput === right.modelInput &&
    left.batchSize === right.batchSize &&
    left.maxDetsPerImage === right.maxDetsPerImage &&
    left.liveSplitCount === right.liveSplitCount &&
    left.cpuAffinity === right.cpuAffinity &&
    left.deviceId === right.deviceId &&
    left.workers === right.workers &&
    left.lanes === right.lanes &&
    left.threshold === right.threshold &&
    left.allowFp16 === right.allowFp16 &&
    left.progressBar === right.progressBar &&
    left.compileMode === right.compileMode
  );
}

export function buildPredictSettingsPatch(
  workflow: Workflow,
  draft: PredictDraft,
  snapshot: StateSnapshot,
): JsonRecord {
  const workflowKey = workflowRecordKey(workflow);
  const fallback = predictDraftFromSnapshot(snapshot, workflow);
  return {
    selected_preset: draft.selectedPreset.trim() || fallback.selectedPreset,
    workflows: {
      [workflowKey]: {
        model_artifacts: {
          weights_path: draft.weightsPath.trim(),
          onnx_path: draft.onnxPath.trim(),
          tensorrt_path: draft.tensorrtPath.trim(),
        },
        execution: {
          cpu_affinity: draft.cpuAffinity.trim(),
          device_id: clampIntegerInput(draft.deviceId, Number(fallback.deviceId), 0),
          workers: clampIntegerInput(draft.workers, Number(fallback.workers), 0),
          lanes: clampIntegerInput(draft.lanes, Number(fallback.lanes), 0),
          allow_fp16: draft.allowFp16,
          progress_bar: draft.progressBar,
          compile_mode: compileModePatchValue(draft.compileMode),
        },
        predict: {
          output_path: draft.outputPath.trim(),
          backend: draft.backend.trim(),
          model_input: modelInputPatchValue(draft.modelInput),
          batch_size: clampIntegerInput(draft.batchSize, Number(fallback.batchSize), 1),
          max_dets_per_image: clampIntegerInput(
            draft.maxDetsPerImage,
            Number(fallback.maxDetsPerImage),
            1,
          ),
          live_split_count: clampIntegerInput(
            draft.liveSplitCount,
            Number(fallback.liveSplitCount),
            1,
          ),
          threshold: clampThresholdInput(draft.threshold, Number(fallback.threshold)),
        },
      },
    },
  };
}

export function predictBrowseRequestForField(
  workflow: Workflow,
  field: PredictBrowseField,
  draft: PredictDraft,
): SourceBrowseRequest {
  const workflowKey = workflowRecordKey(workflow);
  switch (field) {
    case "weightsPath":
      return {
        dialogId: `${workflowKey}.model.weights`,
        mode: "open_file",
        title: "Choose model weights",
        defaultPath: draft.weightsPath.trim(),
        filters: [{ name: "Weights", patterns: kWeightsFilePatterns.slice() }],
      };
    case "onnxPath":
      return {
        dialogId: `${workflowKey}.model.onnx`,
        mode: "open_file",
        title: "Choose ONNX model",
        defaultPath: draft.onnxPath.trim(),
        filters: [{ name: "ONNX", patterns: kOnnxFilePatterns.slice() }],
      };
    case "tensorrtPath":
      return {
        dialogId: `${workflowKey}.model.tensorrt`,
        mode: "open_file",
        title: "Choose TensorRT engine",
        defaultPath: draft.tensorrtPath.trim(),
        filters: [{ name: "TensorRT", patterns: kTensorRtFilePatterns.slice() }],
      };
    case "outputPath":
      return {
        dialogId: `${workflowKey}.output_path`,
        mode: "save_file",
        title: workflow === "live" ? "Choose live output path" : "Choose prediction output path",
        defaultPath: draft.outputPath.trim(),
        filters: [{ name: "JSON", patterns: kJsonFilePatterns.slice() }],
      };
  }
}

export function annotateDraftFromSnapshot(snapshot: StateSnapshot): AnnotateDraft {
  const artifacts = workflowSectionRecord(snapshot, "annotate", "model_artifacts");
  const execution = workflowSectionRecord(snapshot, "annotate", "execution");
  const annotate = workflowLeafRecord(snapshot, "annotate");
  return {
    weightsPath: stringFromValue(artifacts?.weights_path),
    onnxPath: stringFromValue(artifacts?.onnx_path),
    tensorrtPath: stringFromValue(artifacts?.tensorrt_path),
    outputDir: stringFromValue(annotate?.output_dir),
    split: stringFromValue(annotate?.split, "train"),
    backend: stringFromValue(annotate?.backend, "auto"),
    modelInput: modelInputFromValue(annotate?.model_input),
    maxDetsPerImage: formatIntegerString(annotate?.max_dets_per_image, 300),
    deviceId: formatIntegerString(execution?.device_id, 0),
    threshold: formatThresholdString(annotate?.threshold, 0.25),
    allowFp16: booleanFromValue(execution?.allow_fp16, true),
    fullFrame: booleanFromValue(annotate?.full_frame, false),
    compileMode: compileModeFromValue(execution?.compile_mode),
  };
}

export function annotateDraftEquals(left: AnnotateDraft, right: AnnotateDraft): boolean {
  return (
    left.weightsPath === right.weightsPath &&
    left.onnxPath === right.onnxPath &&
    left.tensorrtPath === right.tensorrtPath &&
    left.outputDir === right.outputDir &&
    left.split === right.split &&
    left.backend === right.backend &&
    left.modelInput === right.modelInput &&
    left.maxDetsPerImage === right.maxDetsPerImage &&
    left.deviceId === right.deviceId &&
    left.threshold === right.threshold &&
    left.allowFp16 === right.allowFp16 &&
    left.fullFrame === right.fullFrame &&
    left.compileMode === right.compileMode
  );
}

export function buildAnnotateSettingsPatch(
  draft: AnnotateDraft,
  snapshot: StateSnapshot,
): JsonRecord {
  const fallback = annotateDraftFromSnapshot(snapshot);
  return {
    workflows: {
      annotate: {
        model_artifacts: {
          weights_path: draft.weightsPath.trim(),
          onnx_path: draft.onnxPath.trim(),
          tensorrt_path: draft.tensorrtPath.trim(),
        },
        execution: {
          device_id: clampIntegerInput(draft.deviceId, Number(fallback.deviceId), 0),
          allow_fp16: draft.allowFp16,
          compile_mode: compileModePatchValue(draft.compileMode),
        },
        annotate: {
          output_dir: draft.outputDir.trim(),
          split: draft.split.trim(),
          backend: draft.backend.trim(),
          model_input: modelInputPatchValue(draft.modelInput),
          max_dets_per_image: clampIntegerInput(
            draft.maxDetsPerImage,
            Number(fallback.maxDetsPerImage),
            1,
          ),
          threshold: clampThresholdInput(draft.threshold, Number(fallback.threshold)),
          full_frame: draft.fullFrame,
        },
      },
    },
  };
}

export function annotateBrowseRequestForField(
  field: AnnotateBrowseField,
  draft: AnnotateDraft,
): SourceBrowseRequest {
  switch (field) {
    case "weightsPath":
      return {
        dialogId: "annotate.model.weights",
        mode: "open_file",
        title: "Choose annotate model weights",
        defaultPath: draft.weightsPath.trim(),
        filters: [{ name: "Weights", patterns: kWeightsFilePatterns.slice() }],
      };
    case "onnxPath":
      return {
        dialogId: "annotate.model.onnx",
        mode: "open_file",
        title: "Choose annotate ONNX model",
        defaultPath: draft.onnxPath.trim(),
        filters: [{ name: "ONNX", patterns: kOnnxFilePatterns.slice() }],
      };
    case "tensorrtPath":
      return {
        dialogId: "annotate.model.tensorrt",
        mode: "open_file",
        title: "Choose annotate TensorRT engine",
        defaultPath: draft.tensorrtPath.trim(),
        filters: [{ name: "TensorRT", patterns: kTensorRtFilePatterns.slice() }],
      };
    case "outputDir":
      return {
        dialogId: "annotate.output_dir",
        mode: "open_folder",
        title: "Choose annotation output directory",
        defaultPath: draft.outputDir.trim(),
        filters: [],
      };
  }
}

export function exportDraftFromSnapshot(snapshot: StateSnapshot): ExportDraft {
  const artifacts = workflowSectionRecord(snapshot, "export", "model_artifacts");
  const execution = workflowSectionRecord(snapshot, "export", "execution");
  const exportRecord = workflowLeafRecord(snapshot, "export");
  return {
    weightsPath: stringFromValue(artifacts?.weights_path),
    onnxPath: stringFromValue(artifacts?.onnx_path),
    outputPath: stringFromValue(exportRecord?.output_path),
    deviceId: formatIntegerString(execution?.device_id, 0),
    opsetVersion: formatIntegerString(exportRecord?.opset_version, 19),
    allowFp16: booleanFromValue(execution?.allow_fp16, true),
    buildTensorRt: booleanFromValue(exportRecord?.build_tensorrt, true),
    simplify: booleanFromValue(exportRecord?.simplify, false),
  };
}

export function exportDraftEquals(left: ExportDraft, right: ExportDraft): boolean {
  return (
    left.weightsPath === right.weightsPath &&
    left.onnxPath === right.onnxPath &&
    left.outputPath === right.outputPath &&
    left.deviceId === right.deviceId &&
    left.opsetVersion === right.opsetVersion &&
    left.allowFp16 === right.allowFp16 &&
    left.buildTensorRt === right.buildTensorRt &&
    left.simplify === right.simplify
  );
}

export function buildExportSettingsPatch(
  draft: ExportDraft,
  snapshot: StateSnapshot,
): JsonRecord {
  const fallback = exportDraftFromSnapshot(snapshot);
  return {
    workflows: {
      export: {
        model_artifacts: {
          weights_path: draft.weightsPath.trim(),
          onnx_path: draft.onnxPath.trim(),
        },
        execution: {
          device_id: clampIntegerInput(draft.deviceId, Number(fallback.deviceId), 0),
          allow_fp16: draft.allowFp16,
        },
        export: {
          output_path: draft.outputPath.trim(),
          opset_version: clampIntegerInput(
            draft.opsetVersion,
            Number(fallback.opsetVersion),
            1,
          ),
          build_tensorrt: draft.buildTensorRt,
          simplify: draft.simplify,
        },
      },
    },
  };
}

export function exportBrowseRequestForField(
  field: ExportBrowseField,
  draft: ExportDraft,
): SourceBrowseRequest {
  switch (field) {
    case "weightsPath":
      return {
        dialogId: "export.model.weights",
        mode: "open_file",
        title: "Choose export model weights",
        defaultPath: draft.weightsPath.trim(),
        filters: [{ name: "Weights", patterns: kWeightsFilePatterns.slice() }],
      };
    case "onnxPath":
      return {
        dialogId: "export.model.onnx",
        mode: "open_file",
        title: "Choose export ONNX model",
        defaultPath: draft.onnxPath.trim(),
        filters: [{ name: "ONNX", patterns: kOnnxFilePatterns.slice() }],
      };
    case "outputPath":
      return {
        dialogId: "export.output_path",
        mode: "save_file",
        title: "Choose export output path",
        defaultPath: draft.outputPath.trim(),
        filters: [{ name: "TensorRT", patterns: kTensorRtFilePatterns.slice() }],
      };
  }
}
