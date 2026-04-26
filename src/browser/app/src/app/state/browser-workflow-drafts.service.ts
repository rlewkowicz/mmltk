import { inject, Injectable } from "@angular/core";

import { BrowserAnnotateWorkflowState } from "./browser-annotate-workflow.service";
import { BrowserExportWorkflowState } from "./browser-export-workflow.service";
import { BrowserLiveWorkflowState } from "./browser-live-workflow.service";
import { BrowserPredictWorkflowState } from "./browser-predict-workflow.service";
import { BrowserShellSettingsState } from "./browser-shell-settings.service";
import { BrowserSourceState } from "./browser-source-state.service";
import {
  type DraftFieldVm,
  BrowserTrainWorkflowState,
  type TrainRemoteQueryVm,
} from "./browser-train-workflow.service";
import { BrowserValidateWorkflowState } from "./browser-validate-workflow.service";
import { BrowserWorkflowDraftSharedState } from "./browser-workflow-draft-shared.service";

@Injectable({ providedIn: "root" })
export class BrowserWorkflowDraftsState {
  private readonly shared = inject(BrowserWorkflowDraftSharedState);
  private readonly source = inject(BrowserSourceState);
  private readonly train = inject(BrowserTrainWorkflowState);
  private readonly validate = inject(BrowserValidateWorkflowState);
  private readonly predict = inject(BrowserPredictWorkflowState);
  private readonly annotate = inject(BrowserAnnotateWorkflowState);
  private readonly exportWorkflow = inject(BrowserExportWorkflowState);
  private readonly live = inject(BrowserLiveWorkflowState);
  private readonly shellSettings = inject(BrowserShellSettingsState);

  readonly routeWorkflow = this.shared.routeWorkflow;
  readonly selectedWorkflow = this.shared.selectedWorkflow;

  readonly sourceDraft = this.source.draft;
  readonly sourceDraftDirty = this.source.dirty;
  readonly trainDraft = this.train.draft;
  readonly trainDraftDirty = this.train.dirty;
  readonly validateDraft = this.validate.draft;
  readonly validateDraftDirty = this.validate.dirty;
  readonly predictDraft = this.predict.draft;
  readonly predictDraftDirty = this.predict.dirty;
  readonly annotateDraft = this.annotate.draft;
  readonly annotateDraftDirty = this.annotate.dirty;
  readonly exportDraft = this.exportWorkflow.draft;
  readonly exportDraftDirty = this.exportWorkflow.dirty;
  readonly shellSettingsDraft = this.shellSettings.draft;
  readonly shellSettingsDirty = this.shellSettings.dirty;

  readonly sourceEditingSupported = this.source.sourceEditingSupported;
  readonly sourceKindOptions = this.source.sourceKindOptions;
  readonly sourceBrowseRequest = this.source.browseRequest;
  readonly canApplySource = this.source.canApplySource;
  readonly canBrowseSource = this.source.canBrowseSource;
  readonly canEditSourceLocator = this.source.canEditSourceLocator;
  readonly canEditSourceRecursive = this.source.canEditSourceRecursive;
  readonly sourceDetails = this.source.sourceDetails;
  readonly sourceControls = this.source.sourceControls;
  readonly sourceNote = this.source.sourceNote;

  readonly trainTargetOptions = this.train.trainTargetOptions;
  readonly trainInputModeOptions = this.train.trainInputModeOptions;
  readonly trainOptimizerOptions = this.train.trainOptimizerOptions;
  readonly trainRemoteFamilyOptions = this.train.trainRemoteFamilyOptions;
  readonly compileModeOptions = this.train.compileModeOptions;
  readonly predictModelInputOptions = this.predict.predictModelInputOptions;
  readonly annotateModelInputOptions = this.annotate.annotateModelInputOptions;
  readonly annotateControlsSummary = this.annotate.annotateControlsSummary;
  readonly annotateWorkflowControlStatus = this.annotate.controlsStatus;
  readonly annotateWorkflowDetailsStatusItem = this.annotate.detailsStatus;
  readonly validateWorkflowControlStatus = this.validate.controlsStatus;
  readonly validateWorkflowDetailsStatusItem = this.validate.detailsStatus;
  readonly predictWorkflowControlStatus = this.predict.controlsStatus;
  readonly predictWorkflowDetailsStatusItem = this.predict.detailsStatus;
  readonly exportWorkflowControlStatus = this.exportWorkflow.controlsStatus;
  readonly exportWorkflowDetailsStatusItem = this.exportWorkflow.detailsStatus;
  readonly livePreviewControlsSummary = this.live.livePreviewControlsSummary;
  readonly liveStatusSummary = this.live.liveStatusSummary;
  readonly liveStatusFields = this.live.liveStatusFields;
  readonly liveWorkflowControlStatus = this.live.controlsStatus;
  readonly liveWorkflowDetailsStatusItem = this.live.detailsStatus;

  readonly trainLocalGpuDraftState = this.train.trainLocalGpuDraftState;
  readonly trainRemoteQuery = this.train.trainRemoteQuery;
  readonly trainLocalGpuDetail = this.train.trainLocalGpuDetail;
  readonly trainRemoteStatusSummary = this.train.trainRemoteStatusSummary;
  readonly trainWorkflowControlStatus = this.train.controlsStatus;
  readonly trainWorkflowDetailsStatusItem = this.train.detailsStatus;
  readonly canApplyWorkflowSettings = this.shared.canApplyWorkflowSettings;
  readonly shellDensityOptions = this.shellSettings.shellDensityOptions;
  readonly canApplyShellSettings = this.shellSettings.canApplyShellSettings;
  readonly trainRuntimeNote = this.train.trainRuntimeNote;
  readonly trainRuntimeFields = this.train.trainRuntimeFields;

  patchSourceDraft = this.source.patch.bind(this.source);
  syncSourceDraft = this.source.sync.bind(this.source);
  patchTrainDraft = this.train.patch.bind(this.train);
  syncTrainDraft = this.train.sync.bind(this.train);
  patchValidateDraft = this.validate.patch.bind(this.validate);
  syncValidateDraft = this.validate.sync.bind(this.validate);
  patchPredictDraft = this.predict.patch.bind(this.predict);
  syncPredictDraft = this.predict.sync.bind(this.predict);
  patchAnnotateDraft = this.annotate.patch.bind(this.annotate);
  syncAnnotateDraft = this.annotate.sync.bind(this.annotate);
  patchExportDraft = this.exportWorkflow.patch.bind(this.exportWorkflow);
  syncExportDraft = this.exportWorkflow.sync.bind(this.exportWorkflow);
  patchShellSettingsDraft = this.shellSettings.patch.bind(this.shellSettings);
  syncShellSettingsDraft = this.shellSettings.sync.bind(this.shellSettings);
  workflowDraftDirty = this.shared.workflowDraftDirty.bind(this.shared);
  applySource = this.source.apply.bind(this.source);
  browseSource = this.source.browse.bind(this.source);
  applyShellSettings = this.shellSettings.apply.bind(this.shellSettings);
  applyWorkflowSettings = this.shared.applyWorkflowSettings.bind(this.shared);
  applyWorkflowSettingsIfDirty =
    this.shared.applyWorkflowSettingsIfDirty.bind(this.shared);
  updateTrainTextField = this.train.updateTextField.bind(this.train);
  updateTrainExecutionTarget = this.train.updateExecutionTarget.bind(this.train);
  updateTrainInputMode = this.train.updateInputMode.bind(this.train);
  updateTrainOptimizer = this.train.updateOptimizer.bind(this.train);
  updateTrainCompileMode = this.train.updateCompileMode.bind(this.train);
  updateTrainProgressBar = this.train.updateProgressBar.bind(this.train);
  setLocalGpuDeviceSelected =
    this.train.setLocalGpuDeviceSelected.bind(this.train);
  browseTrainField = this.train.browseField.bind(this.train);
  refreshTrainLocalGpus =
    this.train.refreshLocalGpuInventory.bind(this.train);
  queryTrainRemoteOffers = this.train.queryRemoteOffers.bind(this.train);
  armTrainRemoteOffer = this.train.armRemoteOffer.bind(this.train);
  clearTrainRemoteOffer = this.train.clearRemoteOffer.bind(this.train);
  updateValidateTextField = this.validate.updateTextField.bind(this.validate);
  updateValidateBooleanField =
    this.validate.updateBooleanField.bind(this.validate);
  browseValidateField = this.validate.browseField.bind(this.validate);
  updatePredictTextField = this.predict.updateTextField.bind(this.predict);
  updatePredictModelInput = this.predict.updateModelInput.bind(this.predict);
  updatePredictCompileMode = this.predict.updateCompileMode.bind(this.predict);
  updatePredictBooleanField =
    this.predict.updateBooleanField.bind(this.predict);
  browsePredictField = this.predict.browseField.bind(this.predict);
  updateAnnotateTextField = this.annotate.updateTextField.bind(this.annotate);
  updateAnnotateModelInput = this.annotate.updateModelInput.bind(this.annotate);
  updateAnnotateCompileMode =
    this.annotate.updateCompileMode.bind(this.annotate);
  updateAnnotateBooleanField =
    this.annotate.updateBooleanField.bind(this.annotate);
  browseAnnotateField = this.annotate.browseField.bind(this.annotate);
  reloadAnnotateSetupFrame =
    this.annotate.reloadSetupFrame.bind(this.annotate);
  previousAnnotateSetupFrame =
    this.annotate.previousSetupFrame.bind(this.annotate);
  nextAnnotateSetupFrame = this.annotate.nextSetupFrame.bind(this.annotate);
  startAnnotateLive = this.annotate.startLive.bind(this.annotate);
  stopAnnotateLive = this.annotate.stopLive.bind(this.annotate);
  requestAnnotateSaveNow = this.annotate.requestSaveNow.bind(this.annotate);
  setAnnotateHoldSave = this.annotate.setHoldSave.bind(this.annotate);
  setAnnotateBrushRadius = this.annotate.setBrushRadius.bind(this.annotate);
  updateExportTextField =
    this.exportWorkflow.updateTextField.bind(this.exportWorkflow);
  updateExportBooleanField =
    this.exportWorkflow.updateBooleanField.bind(this.exportWorkflow);
  browseExportField = this.exportWorkflow.browseField.bind(this.exportWorkflow);
}

export type { DraftFieldVm, TrainRemoteQueryVm };
