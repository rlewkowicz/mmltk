import { computed, inject, Injectable } from "@angular/core";

import {
  annotateSidebarState,
  annotateSelectedSummary,
  booleanFromValue,
  capabilitySummaryText,
  compactDisplayText,
  integerDisplayText,
  isRecord,
  jobOutputText,
  primaryActionLabelText,
  statusItemFromContract,
  workflowDetailsPendingStatusItem,
  workflowNavItems,
  type LabeledValueField,
  type CapabilityStatusViewState,
  type WorkflowNavItem,
  workflowSectionRecord,
  workflowFromValue,
  workflowStateRoot,
} from "../../app_shared";
import {
  annotateShellControlsState,
  annotateWorkspaceSceneState,
  annotateToolPaletteState,
  isLivePredictPrimaryActionActive,
  livePreviewControlsState,
  liveStatusState,
  type AnnotateToolId,
} from "../../browser_shell_state";
import type {
  BrowserHostTransport,
  IntentMessage,
  StateSnapshot,
  Workflow,
} from "../../host_api";
import { createIntent } from "../../transport";
import {
  dispatchIntentAction,
  type BrowserIntentDispatcher,
  dispatchParsedIntentNumeric,
  dispatchIntentToggle,
} from "./browser-intent-dispatch";
import {
  type AnnotateDraft,
  type ExportDraft,
  fileDialogRequestPayload,
  type FileDialogRequestLike,
  type PredictDraft,
  type ShellDensity,
  type ShellSettingsDraft,
  type SourceDraft,
  type TrainDraft,
  type ValidateDraft,
} from "./browser-shell.store.helpers";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import { BrowserWorkflowDraftsState } from "./browser-workflow-drafts.service";
import { BrowserWorkspaceStateService } from "./browser-workspace.service";

export type { TransportStatusVm } from "./browser-host-runtime.service";

const kViewportCommitDelayMs = 140;
const kWorkflowOrder: ReadonlyArray<Workflow> = [
  "train",
  "validate",
  "predict",
  "annotate",
  "export",
  "live",
];

export type FieldVm = LabeledValueField;

export interface WorkspaceVm {
  title: string;
}

export interface WorkspaceDiagnosticsVm {
  framePathLabel: string;
  overlayPathLabel: string;
  detail: string;
  statusItems: CapabilityStatusViewState[];
}

export interface WorkspaceCanvasChromeVm {
  workflowLabel: string;
  statusLabel: string;
  fallbackLabel: string | null;
  viewportNote: string | null;
}

export function coerceWorkflow(
  value: unknown,
  fallback: Workflow = "annotate",
): Workflow {
  return workflowFromValue(value, fallback);
}

function field(label: string, value: string, wide = false): FieldVm {
  return { label, value, wide };
}

@Injectable({ providedIn: "root" })
export class BrowserShellStore {
  private readonly runtimeState = inject(BrowserHostRuntimeState);
  private readonly workflowDrafts = inject(BrowserWorkflowDraftsState);
  private readonly workspaceViewport = inject(BrowserWorkspaceStateService);
  private readonly dispatchIntent: BrowserIntentDispatcher = (
    workflow,
    intent,
    payload,
  ) => this.dispatch(workflow, intent, payload);

  readonly availableWorkflows = kWorkflowOrder;
  readonly workflows: readonly WorkflowNavItem[] = workflowNavItems;

  readonly transport = this.runtimeState.transport;
  readonly snapshot = this.runtimeState.snapshot;
  readonly bridgeState = this.runtimeState.bridgeState;
  readonly routeWorkflow = this.workflowDrafts.routeWorkflow;
  readonly workspaceState = this.workspaceViewport.workspaceState;
  readonly sourceDraft = this.workflowDrafts.sourceDraft;
  readonly sourceDraftDirty = this.workflowDrafts.sourceDraftDirty;
  readonly trainDraft = this.workflowDrafts.trainDraft;
  readonly trainDraftDirty = this.workflowDrafts.trainDraftDirty;
  readonly validateDraft = this.workflowDrafts.validateDraft;
  readonly validateDraftDirty = this.workflowDrafts.validateDraftDirty;
  readonly predictDraft = this.workflowDrafts.predictDraft;
  readonly predictDraftDirty = this.workflowDrafts.predictDraftDirty;
  readonly annotateDraft = this.workflowDrafts.annotateDraft;
  readonly annotateDraftDirty = this.workflowDrafts.annotateDraftDirty;
  readonly exportDraft = this.workflowDrafts.exportDraft;
  readonly exportDraftDirty = this.workflowDrafts.exportDraftDirty;
  readonly shellSettingsDraft = this.workflowDrafts.shellSettingsDraft;
  readonly shellSettingsDirty = this.workflowDrafts.shellSettingsDirty;
  readonly lastIntent = this.runtimeState.lastIntent;
  readonly rendererStatus = this.workspaceViewport.rendererStatus;

  readonly transportMode = this.runtimeState.transportMode;
  readonly transportStatus = this.runtimeState.transportStatus;
  readonly transportLabel = computed(() => this.transportStatus().label);
  readonly workflow = computed(() => this.snapshot().active_workflow);
  readonly selectedWorkflow = this.workflowDrafts.selectedWorkflow;
  readonly protocolVersion = computed(() => this.snapshot().protocol_version);
  readonly stateRevision = computed(() => this.snapshot().state_revision);
  readonly viewportEnabled = this.workspaceViewport.viewportEnabled;
  readonly job = computed(() => this.snapshot().job);
  readonly annotateSidebar = computed(() => annotateSidebarState(this.snapshot()));
  readonly annotateControls = computed(() => annotateShellControlsState(this.snapshot()));
  readonly annotateToolPalette = computed(() =>
    annotateToolPaletteState(this.snapshot()),
  );
  readonly liveStatus = computed(() => liveStatusState(this.snapshot()));
  readonly livePreviewControls = computed(() =>
    livePreviewControlsState(this.snapshot()),
  );
  readonly annotateWorkspaceScene = computed(() =>
    this.selectedWorkflow() === "annotate"
      ? annotateWorkspaceSceneState(this.snapshot())
      : null,
  );
  readonly selectedRegion = this.workspaceViewport.selectedRegion;
  readonly workspaceGeometry = this.workspaceViewport.workspaceGeometry;
  readonly workspaceClip = this.workspaceViewport.workspaceClip;
  readonly sourceEditingSupported = this.workflowDrafts.sourceEditingSupported;
  readonly sourceKindOptions = this.workflowDrafts.sourceKindOptions;
  readonly sourceBrowseRequest = this.workflowDrafts.sourceBrowseRequest;
  readonly canApplySource = this.workflowDrafts.canApplySource;
  readonly canBrowseSource = this.workflowDrafts.canBrowseSource;
  readonly canEditSourceLocator = this.workflowDrafts.canEditSourceLocator;
  readonly canEditSourceRecursive = this.workflowDrafts.canEditSourceRecursive;
  readonly trainTargetOptions = this.workflowDrafts.trainTargetOptions;
  readonly trainInputModeOptions = this.workflowDrafts.trainInputModeOptions;
  readonly trainOptimizerOptions = this.workflowDrafts.trainOptimizerOptions;
  readonly trainRemoteFamilyOptions = this.workflowDrafts.trainRemoteFamilyOptions;
  readonly compileModeOptions = this.workflowDrafts.compileModeOptions;
  readonly predictModelInputOptions = this.workflowDrafts.predictModelInputOptions;
  readonly annotateModelInputOptions = this.workflowDrafts.annotateModelInputOptions;
  readonly trainLocalGpuDraftState = this.workflowDrafts.trainLocalGpuDraftState;
  readonly trainLocalGpuDetail = this.workflowDrafts.trainLocalGpuDetail;
  readonly trainRemoteQuery = this.workflowDrafts.trainRemoteQuery;
  readonly trainRemoteStatusSummary = this.workflowDrafts.trainRemoteStatusSummary;
  readonly trainWorkflowControlStatus = this.workflowDrafts.trainWorkflowControlStatus;
  readonly trainWorkflowDetailsStatusItem =
    this.workflowDrafts.trainWorkflowDetailsStatusItem;
  readonly canApplyWorkflowSettings = this.workflowDrafts.canApplyWorkflowSettings;
  readonly shellDensityOptions = this.workflowDrafts.shellDensityOptions;
  readonly canApplyShellSettings = this.workflowDrafts.canApplyShellSettings;
  readonly trainRuntimeNote = this.workflowDrafts.trainRuntimeNote;
  readonly sourceDetails = this.workflowDrafts.sourceDetails;
  readonly sourceControls = this.workflowDrafts.sourceControls;
  readonly sourceNote = this.workflowDrafts.sourceNote;
  readonly annotateControlsSummary = this.workflowDrafts.annotateControlsSummary;
  readonly validateWorkflowControlStatus =
    this.workflowDrafts.validateWorkflowControlStatus;
  readonly validateWorkflowDetailsStatusItem =
    this.workflowDrafts.validateWorkflowDetailsStatusItem;
  readonly predictWorkflowControlStatus =
    this.workflowDrafts.predictWorkflowControlStatus;
  readonly predictWorkflowDetailsStatusItem =
    this.workflowDrafts.predictWorkflowDetailsStatusItem;
  readonly annotateWorkflowControlStatus =
    this.workflowDrafts.annotateWorkflowControlStatus;
  readonly annotateWorkflowDetailsStatusItem =
    this.workflowDrafts.annotateWorkflowDetailsStatusItem;
  readonly exportWorkflowControlStatus =
    this.workflowDrafts.exportWorkflowControlStatus;
  readonly exportWorkflowDetailsStatusItem =
    this.workflowDrafts.exportWorkflowDetailsStatusItem;
  readonly livePreviewControlsSummary =
    this.workflowDrafts.livePreviewControlsSummary;
  readonly liveStatusSummary = this.workflowDrafts.liveStatusSummary;
  readonly liveStatusFields = this.workflowDrafts.liveStatusFields;
  readonly liveWorkflowControlStatus = this.workflowDrafts.liveWorkflowControlStatus;
  readonly liveWorkflowDetailsStatusItem =
    this.workflowDrafts.liveWorkflowDetailsStatusItem;
  readonly canCommitCrop = this.workspaceViewport.canCommitCrop;
  readonly canFitWorkspace = this.workspaceViewport.canFitWorkspace;
  readonly canFocusSelection = this.workspaceViewport.canFocusSelection;
  readonly canFocusCrop = this.workspaceViewport.canFocusCrop;
  readonly lastIntentLabel = computed(() => {
    const intent = this.lastIntent();
    return intent === null ? "none" : `${intent.intent} -> ${intent.workflow}`;
  });
  readonly annotationDetails = computed(() => {
    const annotation = this.snapshot().annotation;
    return [
      field("Document", integerDisplayText(annotation.document_generation)),
      field("Session", integerDisplayText(annotation.session_revision)),
      field("Instances", integerDisplayText(annotation.instance_count)),
      field(
        "Selected",
        annotation.selected_instance === null
          ? "none"
          : integerDisplayText(annotation.selected_instance),
      ),
    ];
  });
  readonly workflowControlStatus = computed<CapabilityStatusViewState[]>(() =>
    this.buildWorkflowControlsStatus(this.selectedWorkflow()),
  );
  readonly workflowControlsNote = computed(() =>
    capabilitySummaryText(
      this.workflowControlStatus(),
      "workflow controls pending",
    ),
  );
  readonly workflowDetailsStatus = computed<CapabilityStatusViewState[]>(() =>
    this.buildWorkflowDetailsStatus(this.selectedWorkflow()),
  );
  readonly workflowDetailsNote = computed(() =>
    capabilitySummaryText(
      this.workflowDetailsStatus(),
      "workflow details pending",
    ),
  );
  readonly jobFields = computed(() => {
    const job = this.snapshot().job;
    return [
      field("Status", job.running ? "running" : "idle"),
      field("Label", compactDisplayText(job.label)),
      field("Summary", compactDisplayText(job.summary), true),
      field("Error", compactDisplayText(job.error)),
    ];
  });
  readonly jobOutput = computed(() => {
    return jobOutputText(this.snapshot().job);
  });
  readonly shellSettingsNote = computed(() =>
    this.shellSettingsDirty()
      ? "UI settings pending"
      : "UI settings ready",
  );
  readonly workflowStateJson = computed(() =>
    JSON.stringify(this.snapshot().workflow_state, null, 2),
  );
  readonly workspaceDiagnostics = this.workspaceViewport.workspaceDiagnostics;
  readonly workspace = this.workspaceViewport.workspace;
  readonly workspaceCanvasChrome = this.workspaceViewport.workspaceCanvasChrome;
  readonly trainRuntimeFields = this.workflowDrafts.trainRuntimeFields;
  readonly annotateToolPaletteSummary = computed(() =>
    capabilitySummaryText(
      this.annotateToolPalette().statusItems,
      "annotate tool palette pending",
    ),
  );
  readonly annotateSelectedSummary = computed(() => {
    return annotateSelectedSummary(this.annotateSidebar());
  });
  readonly annotateSidebarNote = computed(() => {
    const sidebar = this.annotateSidebar();
    if (sidebar?.assistError) {
      return sidebar.assistError;
    }
    if (sidebar?.assistSummary) {
      return sidebar.assistSummary;
    }
    return this.annotateToolPaletteSummary();
  });
  readonly intentStatus = computed<CapabilityStatusViewState[]>(() => {
    const diagnostics = this.workspaceDiagnostics();
    return diagnostics.statusItems;
  });
  readonly primaryActionLabel = computed(() =>
    primaryActionLabelText(
      this.selectedWorkflow(),
      this.trainDraft().executionTarget,
      this.sourceDraft().kind,
      this.livePredictPrimaryActionActive(),
    ),
  );
  readonly intentSummary = computed(() => {
    return capabilitySummaryText(
      this.intentStatus(),
      "intent surface pending",
    );
  });
  readonly footerWorkflow = computed(() =>
    this.workflow() === this.selectedWorkflow()
      ? `workflow: ${this.selectedWorkflow()}`
      : `workflow: ${this.selectedWorkflow()} (host ${this.workflow()})`,
  );
  readonly workspaceRenderInput = this.workspaceViewport.workspaceRenderInput;

  connectTransport(transport: BrowserHostTransport): void {
    this.runtimeState.connectTransport(transport);
  }

  activateWorkflow(workflow: Workflow): void {
    this.routeWorkflow.set(workflow);
    this.workflowDrafts.syncSourceDraft(true);
    if (this.workflow() !== workflow) {
      this.dispatch(workflow, "settings.update", {
        patch: {
          current_view: workflow,
        },
      });
    }
  }

  selectWorkflow(workflow: Workflow): IntentMessage {
    this.routeWorkflow.set(workflow);
    this.workflowDrafts.syncSourceDraft(true);
    return this.dispatch(workflow, "settings.update", {
      patch: {
        current_view: workflow,
      },
    });
  }

  selectAnnotateTool(tool: AnnotateToolId): IntentMessage | null {
    if (this.selectedWorkflow() !== "annotate") {
      return null;
    }
    return this.dispatch("annotate", "tool.select", { tool });
  }

  dispatch(
    workflow: Workflow,
    intent: string,
    payload: Record<string, unknown>,
  ): IntentMessage {
    return this.runtimeState.dispatch(workflow, intent, payload);
  }

  updateSourceKind(kind: string): void {
    const allowedKinds = this.sourceKindOptions().map(
      (option: { value: SourceDraft["kind"] }) => option.value,
    );
    if (!allowedKinds.includes(kind as SourceDraft["kind"])) {
      return;
    }
    this.patchSourceDraft((current) => ({
      ...current,
      kind: kind as SourceDraft["kind"],
      locator:
        kind === "video_stream"
          ? `device:${current.deviceIndex.trim().length > 0 ? current.deviceIndex.trim() : "0"}`
          : current.locator,
      recursive: kind === "image_folder" ? current.recursive : false,
    }));
  }

  updateSourceLocator(locator: string): void {
    this.patchSourceDraft((current) => ({
      ...current,
      locator,
    }));
  }

  updateSourceRecursive(recursive: boolean): void {
    this.patchSourceDraft((current) => ({
      ...current,
      recursive: current.kind === "image_folder" ? recursive : false,
    }));
  }

  updateSourceDeviceIndex(deviceIndex: string): void {
    this.patchSourceDraft((current) => ({
      ...current,
      deviceIndex,
      locator:
        current.kind === "video_stream"
          ? `device:${deviceIndex.trim().length > 0 ? deviceIndex.trim() : "0"}`
          : current.locator,
    }));
  }

  updateSourceCaptureWidth(captureWidth: string): void {
    this.patchSourceDraft((current) => ({
      ...current,
      captureWidth,
    }));
  }

  updateSourceCaptureHeight(captureHeight: string): void {
    this.patchSourceDraft((current) => ({
      ...current,
      captureHeight,
    }));
  }

  updateSourceCaptureFps(captureFps: string): void {
    this.patchSourceDraft((current) => ({
      ...current,
      captureFps,
    }));
  }

  updateSourceV4l2BufferCount(v4l2BufferCount: string): void {
    this.patchSourceDraft((current) => ({
      ...current,
      v4l2BufferCount,
    }));
  }

  applySource(): IntentMessage | null {
    return this.workflowDrafts.applySource();
  }

  browseSource = this.workflowDrafts.browseSource;
  updateTrainTextField = this.workflowDrafts.updateTrainTextField;
  updateTrainExecutionTarget = this.workflowDrafts.updateTrainExecutionTarget;
  updateTrainInputMode = this.workflowDrafts.updateTrainInputMode;
  updateTrainOptimizer = this.workflowDrafts.updateTrainOptimizer;
  updateTrainCompileMode = this.workflowDrafts.updateTrainCompileMode;
  updateTrainProgressBar = this.workflowDrafts.updateTrainProgressBar;
  setTrainLocalGpuDeviceSelected = this.workflowDrafts.setLocalGpuDeviceSelected;

  setTrainRemoteFamilyEnabled(
    family: keyof TrainDraft["remoteFamilies"],
    enabled: boolean,
  ): void {
    this.patchTrainDraft((current) => ({
      ...current,
      remoteFamilies: {
        ...current.remoteFamilies,
        [family]: enabled,
      },
    }));
  }

  browseTrainField = this.workflowDrafts.browseTrainField;
  refreshTrainLocalGpuInventory = this.workflowDrafts.refreshTrainLocalGpus;
  queryTrainRemoteOffers = this.workflowDrafts.queryTrainRemoteOffers;
  armTrainRemoteOffer = this.workflowDrafts.armTrainRemoteOffer;
  clearTrainRemoteOffer = this.workflowDrafts.clearTrainRemoteOffer;
  updateValidateTextField = this.workflowDrafts.updateValidateTextField;
  updateValidateBooleanField = this.workflowDrafts.updateValidateBooleanField;
  browseValidateField = this.workflowDrafts.browseValidateField;
  updatePredictTextField = this.workflowDrafts.updatePredictTextField;
  updatePredictModelInput = this.workflowDrafts.updatePredictModelInput;
  updatePredictCompileMode = this.workflowDrafts.updatePredictCompileMode;
  updatePredictBooleanField = this.workflowDrafts.updatePredictBooleanField;
  browsePredictField = this.workflowDrafts.browsePredictField;
  updateAnnotateTextField = this.workflowDrafts.updateAnnotateTextField;
  updateAnnotateModelInput = this.workflowDrafts.updateAnnotateModelInput;
  updateAnnotateCompileMode = this.workflowDrafts.updateAnnotateCompileMode;
  updateAnnotateBooleanField = this.workflowDrafts.updateAnnotateBooleanField;
  browseAnnotateField = this.workflowDrafts.browseAnnotateField;
  updateExportTextField = this.workflowDrafts.updateExportTextField;
  updateExportBooleanField = this.workflowDrafts.updateExportBooleanField;
  browseExportField = this.workflowDrafts.browseExportField;

  applyWorkflowSettings(): IntentMessage | null {
    return this.workflowDrafts.applyWorkflowSettings();
  }

  updateShellUiScale(uiScale: string): void {
    this.patchShellSettingsDraft((current) => ({
      ...current,
      uiScale,
    }));
  }

  updateShellDensity(density: string): void {
    const allowedDensities = this.shellDensityOptions.map((option) => option.value);
    if (!allowedDensities.includes(density as ShellDensity)) {
      return;
    }
    this.patchShellSettingsDraft((current) => ({
      ...current,
      density: density as ShellDensity,
    }));
  }

  applyShellSettings(): IntentMessage | null {
    return this.workflowDrafts.applyShellSettings();
  }

  reloadAnnotateSetupFrame(): IntentMessage | null {
    this.flushAnnotateActionDrafts();
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().setupFrame.reload,
    );
  }

  previousAnnotateSetupFrame(): IntentMessage | null {
    this.flushAnnotateActionDrafts();
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().setupFrame.previous,
    );
  }

  nextAnnotateSetupFrame(): IntentMessage | null {
    this.flushAnnotateActionDrafts();
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().setupFrame.next,
    );
  }

  startAnnotateLive(): IntentMessage | null {
    this.flushAnnotateActionDrafts();
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().liveAnnotate.start,
    );
  }

  stopAnnotateLive(): IntentMessage | null {
    this.flushAnnotateActionDrafts();
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().liveAnnotate.stop,
    );
  }

  requestAnnotateSaveNow(): IntentMessage | null {
    this.flushAnnotateActionDrafts();
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().save.saveNow,
    );
  }

  setAnnotateHoldSave(active: boolean): IntentMessage | null {
    return dispatchIntentToggle(
      this.dispatchIntent,
      this.annotateControls().save.holdSave,
      active,
    );
  }

  setAnnotateBrushRadius(radius: string | number): IntentMessage | null {
    return dispatchParsedIntentNumeric(
      this.dispatchIntent,
      this.annotateControls().brush.radius,
      radius,
    );
  }

  setLivePreviewFitToCapture(enabled: boolean): IntentMessage | null {
    return dispatchIntentToggle(
      this.dispatchIntent,
      this.livePreviewControls().fitToCapture,
      enabled,
    );
  }

  setLivePreviewFullFrameDisplay(enabled: boolean): IntentMessage | null {
    return dispatchIntentToggle(
      this.dispatchIntent,
      this.livePreviewControls().fullFrameDisplay,
      enabled,
    );
  }

  runPrimaryAction(): void {
    const workflow = this.selectedWorkflow();
    this.workflowDrafts.applySource();
    this.workflowDrafts.applyWorkflowSettingsIfDirty();
    if (workflow === "train") {
      this.runTrainPrimaryAction();
      return;
    }
    if (workflow === "annotate") {
      this.dispatch("annotate", "annotate.save", {});
      return;
    }
    if (workflow === "validate") {
      this.dispatch("validate", "validate.start", {});
      return;
    }
    if (workflow === "export") {
      this.dispatch("export", "export.start", {});
      return;
    }
    this.dispatch(
      workflow === "live" ? "live" : "predict",
      this.livePredictPrimaryActionActive() ? "predict.stop" : "predict.start",
      {},
    );
  }

  setRendererStatus(status: string): void {
    this.workspaceViewport.setRendererStatus(status);
  }

  setWorkspaceCanvasSize(canvasWidth: number, canvasHeight: number): void {
    this.workspaceViewport.setWorkspaceCanvasSize(canvasWidth, canvasHeight);
  }

  fitWorkspaceToCapture(commitViewport = false): void {
    this.workspaceViewport.fitWorkspaceToCapture(commitViewport);
  }

  commitWorkspaceCrop(): boolean {
    return this.workspaceViewport.commitWorkspaceCrop();
  }

  focusSelectedAnnotation(scheduleCommit = false): boolean {
    return this.workspaceViewport.focusSelectedAnnotation(scheduleCommit);
  }

  focusActiveClip(scheduleCommit = false): boolean {
    return this.workspaceViewport.focusActiveClip(scheduleCommit);
  }

  beginWorkspaceInteraction(
    ...args: Parameters<BrowserWorkspaceStateService["beginWorkspaceInteraction"]>
  ): boolean {
    return this.workspaceViewport.beginWorkspaceInteraction(...args);
  }

  moveWorkspaceInteraction(pointerId: number, canvasX: number, canvasY: number): boolean {
    return this.workspaceViewport.moveWorkspaceInteraction(
      pointerId,
      canvasX,
      canvasY,
    );
  }

  endWorkspaceInteraction(pointerId: number, shouldCommit: boolean): boolean {
    return this.workspaceViewport.endWorkspaceInteraction(pointerId, shouldCommit);
  }

  cancelWorkspaceInteraction(pointerId: number): boolean {
    return this.workspaceViewport.cancelWorkspaceInteraction(pointerId);
  }

  zoomWorkspaceAtCanvasPoint(canvasX: number, canvasY: number, deltaY: number): boolean {
    return this.workspaceViewport.zoomWorkspaceAtCanvasPoint(
      canvasX,
      canvasY,
      deltaY,
    );
  }

  commitViewport(): boolean {
    return this.workspaceViewport.commitViewport();
  }

  scheduleViewportCommit(delayMs = kViewportCommitDelayMs): void {
    this.workspaceViewport.scheduleViewportCommit(delayMs);
  }

  undoAnnotateSidebar(): IntentMessage | null {
    return this.dispatchAnnotateSidebar("undo");
  }

  redoAnnotateSidebar(): IntentMessage | null {
    return this.dispatchAnnotateSidebar("redo");
  }

  deleteSelectedAnnotateObject(): IntentMessage | null {
    return this.dispatchAnnotateSidebar("delete_selected");
  }

  selectAnnotateObject(objectIndex: number | null): IntentMessage | null {
    return this.dispatchAnnotateSidebar("select_object", {
      object_index: objectIndex,
    });
  }

  selectAnnotateCategory(categoryIndex: number): IntentMessage | null {
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject === null || selectedObject === undefined) {
      return null;
    }
    return this.dispatchAnnotateSidebar("update_selected", {
      enabled: selectedObject.enabled,
      category_index: categoryIndex,
    });
  }

  requestAnnotateAssist(): IntentMessage | null {
    const sidebar = this.annotateSidebar();
    if (sidebar === null || !sidebar.assistAvailable || sidebar.assistRunning) {
      return null;
    }
    return this.dispatchAnnotateSidebar("assist");
  }

  redrawSelectedAnnotateBox(): IntentMessage | null {
    const sidebar = this.annotateSidebar();
    if (sidebar === null || sidebar.canRedrawSelectedBox !== true) {
      return null;
    }
    return this.dispatchAnnotateSidebar("redraw_box");
  }

  updateAnnotateSplineActiveSegment(segmentIndex: number): IntentMessage | null {
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject?.spline === null || selectedObject?.spline === undefined) {
      return null;
    }
    return this.dispatchAnnotateSidebar("spline.update_active_segment", {
      spline_segment_index: Math.max(0, Math.round(segmentIndex)),
    });
  }

  insertAnnotateSplineKnot(): IntentMessage | null {
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject?.spline?.canInsertActiveSegmentKnot !== true) {
      return null;
    }
    return this.dispatchAnnotateSidebar("spline.insert_active_knot");
  }

  setAnnotateSplineHandleMode(handleMode: string): IntentMessage | null {
    if (handleMode !== "corner" && handleMode !== "smooth" && handleMode !== "mirrored") {
      return null;
    }
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject?.spline?.canEditActiveHandleMode !== true) {
      return null;
    }
    return this.dispatchAnnotateSidebar("spline.set_handle_mode", {
      spline_handle_mode: handleMode,
    });
  }

  closeAnnotateSpline(): IntentMessage | null {
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject?.spline?.canClose !== true) {
      return null;
    }
    return this.dispatchAnnotateSidebar("spline.close");
  }

  reopenAnnotateSpline(): IntentMessage | null {
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject?.spline?.canReopen !== true) {
      return null;
    }
    return this.dispatchAnnotateSidebar("spline.reopen");
  }

  deleteAnnotateSplineActiveKnot(): IntentMessage | null {
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject?.spline?.canDeleteActiveKnot !== true) {
      return null;
    }
    return this.dispatchAnnotateSidebar("spline.delete_active_knot");
  }

  updateAnnotateSkeletonActiveJoint(jointIndex: number): IntentMessage | null {
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject?.skeleton === null || selectedObject?.skeleton === undefined) {
      return null;
    }
    return this.dispatchAnnotateSidebar("skeleton.update_active_joint", {
      skeleton_joint_index: Math.max(0, Math.round(jointIndex)),
    });
  }

  skipAnnotateSkeletonJoint(): IntentMessage | null {
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject?.skeleton?.canSkipJoint !== true) {
      return null;
    }
    return this.dispatchAnnotateSidebar("skeleton.skip_joint");
  }

  hideAnnotateSkeletonJoint(): IntentMessage | null {
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject?.skeleton?.canHideActiveJoint !== true) {
      return null;
    }
    return this.dispatchAnnotateSidebar("skeleton.hide_joint");
  }

  showAnnotateSkeletonJoint(): IntentMessage | null {
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject?.skeleton?.canReactivateActiveJoint !== true) {
      return null;
    }
    return this.dispatchAnnotateSidebar("skeleton.show_joint");
  }

  reseedAnnotateSkeletonJoint(): IntentMessage | null {
    const selectedObject = this.annotateSidebar()?.selectedObject;
    if (selectedObject?.skeleton?.canReseedActiveJoint !== true) {
      return null;
    }
    return this.dispatchAnnotateSidebar("skeleton.reseed_joint");
  }

  isAnnotateCategorySelected(categoryIndex: number): boolean {
    const sidebar = this.annotateSidebar();
    if (sidebar === null) {
      return false;
    }
    if (sidebar.selectedObject !== null) {
      return sidebar.selectedObject.categoryIndex === categoryIndex;
    }
    return sidebar.preferredNewObjectCategoryIndex === categoryIndex;
  }

  private patchSourceDraft(
    updater: (current: SourceDraft) => SourceDraft,
  ): void {
    this.workflowDrafts.patchSourceDraft(updater);
  }

  private syncSourceDraft(force = false): void {
    this.workflowDrafts.syncSourceDraft(force);
  }

  private patchTrainDraft(
    updater: (current: TrainDraft) => TrainDraft,
  ): void {
    this.workflowDrafts.patchTrainDraft(updater);
  }

  private syncTrainDraft(force = false): void {
    this.workflowDrafts.syncTrainDraft(force);
  }

  private patchValidateDraft(
    updater: (current: ValidateDraft) => ValidateDraft,
  ): void {
    this.workflowDrafts.patchValidateDraft(updater);
  }

  private syncValidateDraft(force = false): void {
    this.workflowDrafts.syncValidateDraft(force);
  }

  private patchPredictDraft(
    updater: (current: PredictDraft) => PredictDraft,
  ): void {
    this.workflowDrafts.patchPredictDraft(updater);
  }

  private syncPredictDraft(force = false): void {
    this.workflowDrafts.syncPredictDraft(force);
  }

  private patchAnnotateDraft(
    updater: (current: AnnotateDraft) => AnnotateDraft,
  ): void {
    this.workflowDrafts.patchAnnotateDraft(updater);
  }

  private syncAnnotateDraft(force = false): void {
    this.workflowDrafts.syncAnnotateDraft(force);
  }

  private patchExportDraft(
    updater: (current: ExportDraft) => ExportDraft,
  ): void {
    this.workflowDrafts.patchExportDraft(updater);
  }

  private syncExportDraft(force = false): void {
    this.workflowDrafts.syncExportDraft(force);
  }

  private patchShellSettingsDraft(
    updater: (current: ShellSettingsDraft) => ShellSettingsDraft,
  ): void {
    this.workflowDrafts.patchShellSettingsDraft(updater);
  }

  private syncShellSettingsDraft(force = false): void {
    this.workflowDrafts.syncShellSettingsDraft(force);
  }

  private dispatchFileDialogRequest(
    workflow: Workflow,
    request: FileDialogRequestLike,
  ): IntentMessage {
    return this.dispatch(workflow, "file_dialog.request", fileDialogRequestPayload(request));
  }

  private workflowDraftDirty(workflow: Workflow): boolean {
    return this.workflowDrafts.workflowDraftDirty(workflow);
  }

  private applyWorkflowSettingsIfDirty(): IntentMessage | null {
    return this.workflowDrafts.applyWorkflowSettingsIfDirty();
  }

  private dispatchAnnotateSidebar(
    action: string,
    payload: Record<string, unknown> = {},
  ): IntentMessage | null {
    if (this.selectedWorkflow() !== "annotate") {
      return null;
    }
    return this.dispatch("annotate", "annotate.sidebar", {
      action,
      ...payload,
    });
  }

  private runTrainPrimaryAction(): void {
    const snapshot = this.snapshot();
    if (this.trainDraft().executionTarget === "remote") {
      const remoteSession = workflowSectionRecord(snapshot, "train", "remote_session");
      const running = booleanFromValue(remoteSession?.instance_running, false);
      this.dispatch("train", running ? "train.remote.stop" : "train.remote.start", {});
      return;
    }
    const root = workflowStateRoot(snapshot);
    const runtime = isRecord(root?.train_runtime) ? root.train_runtime : null;
    const running = booleanFromValue(runtime?.running, false);
    this.dispatch("train", running ? "train.stop" : "train.start", {});
  }

  private livePredictPrimaryActionActive(): boolean {
    return isLivePredictPrimaryActionActive(this.liveStatus());
  }

  private flushAnnotateActionDrafts(): void {
    this.workflowDrafts.applySource();
    this.workflowDrafts.applyWorkflowSettingsIfDirty();
  }

  private buildWorkflowControlsStatus(
    workflow: Workflow,
  ): CapabilityStatusViewState[] {
    const items: CapabilityStatusViewState[] = [];
    switch (workflow) {
      case "train":
        items.push(this.trainWorkflowControlStatus());
        break;
      case "validate":
        items.push(this.validateWorkflowControlStatus());
        break;
      case "export":
        items.push(this.exportWorkflowControlStatus());
        break;
      case "annotate":
        items.push(this.annotateWorkflowControlStatus());
        break;
      case "live":
        items.push(this.liveWorkflowControlStatus());
        break;
      default:
        items.push(this.predictWorkflowControlStatus());
        break;
    }
    return items;
  }

  private buildWorkflowDetailsStatus(
    workflow: Workflow,
  ): CapabilityStatusViewState[] {
    switch (workflow) {
      case "train": {
        return [this.trainWorkflowDetailsStatusItem()];
      }
      case "validate":
        return [this.validateWorkflowDetailsStatusItem()];
      case "predict":
        return [this.predictWorkflowDetailsStatusItem()];
      case "live":
        return [this.liveWorkflowDetailsStatusItem()];
      case "annotate":
        return [this.annotateWorkflowDetailsStatusItem()];
      case "export":
        return [this.exportWorkflowDetailsStatusItem()];
      default:
        return [workflowDetailsPendingStatusItem()];
    }
  }
}
