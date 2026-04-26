import { computed, inject, Injectable } from "@angular/core";

import {
  capabilitySummaryText,
  workflowDetailsPendingStatusItem,
  type CapabilityStatusViewState,
} from "../../app_shared";
import type { Workflow } from "../../host_api";
import { BrowserAnnotateWorkflowState } from "./browser-annotate-workflow.service";
import { BrowserExportWorkflowState } from "./browser-export-workflow.service";
import { BrowserLiveWorkflowState } from "./browser-live-workflow.service";
import { BrowserPredictWorkflowState } from "./browser-predict-workflow.service";
import { BrowserTrainWorkflowState } from "./browser-train-workflow.service";
import { BrowserValidateWorkflowState } from "./browser-validate-workflow.service";
import { BrowserWorkflowDraftSharedState } from "./browser-workflow-draft-shared.service";
import { BrowserWorkflowRouteState } from "./browser-workflow-route.service";

@Injectable({ providedIn: "root" })
export class BrowserWorkflowPanelsState {
  private readonly route = inject(BrowserWorkflowRouteState);
  private readonly shared = inject(BrowserWorkflowDraftSharedState);
  private readonly train = inject(BrowserTrainWorkflowState);
  private readonly validate = inject(BrowserValidateWorkflowState);
  private readonly predict = inject(BrowserPredictWorkflowState);
  private readonly live = inject(BrowserLiveWorkflowState);
  private readonly annotate = inject(BrowserAnnotateWorkflowState);
  private readonly exportWorkflow = inject(BrowserExportWorkflowState);

  readonly selectedWorkflow = this.route.selectedWorkflow;
  readonly canApplyWorkflowSettings = this.shared.canApplyWorkflowSettings;
  readonly workflowControlStatus = computed<CapabilityStatusViewState[]>(() =>
    this.buildWorkflowControlsStatus(this.selectedWorkflow()),
  );
  readonly workflowControlsNote = computed(() =>
    capabilitySummaryText(this.workflowControlStatus(), "workflow controls pending"),
  );
  readonly workflowDetailsStatus = computed<CapabilityStatusViewState[]>(() =>
    this.buildWorkflowDetailsStatus(this.selectedWorkflow()),
  );
  readonly workflowDetailsNote = computed(() =>
    capabilitySummaryText(this.workflowDetailsStatus(), "workflow details pending"),
  );

  applyWorkflowSettings() {
    return this.shared.applyWorkflowSettings();
  }

  private buildWorkflowControlsStatus(
    workflow: Workflow,
  ): CapabilityStatusViewState[] {
    const items: CapabilityStatusViewState[] = [
      {
        key: "draft",
        label: "Draft",
        status: this.shared.workflowDraftDirty(workflow) ? "active" : "ready",
        summary: this.shared.workflowDraftDirty(workflow)
          ? "workflow draft staged"
          : "workflow draft synchronized",
        detail: this.shared.workflowDraftDirty(workflow)
          ? "Apply Workflow or the primary action to send settings.update."
          : "Workflow draft matches the current host snapshot.",
      },
    ];
    switch (workflow) {
      case "train":
        items.push(this.train.controlsStatus());
        break;
      case "validate":
        items.push(this.validate.controlsStatus());
        break;
      case "export":
        items.push(this.exportWorkflow.controlsStatus());
        break;
      case "annotate":
        items.push(this.annotate.controlsStatus());
        break;
      case "live":
        items.push(this.live.controlsStatus());
        break;
      default:
        items.push(this.predict.controlsStatus());
        break;
    }
    return items;
  }

  private buildWorkflowDetailsStatus(
    workflow: Workflow,
  ): CapabilityStatusViewState[] {
    switch (workflow) {
      case "train":
        return [this.train.detailsStatus()];
      case "validate":
        return [this.validate.detailsStatus()];
      case "predict":
        return [this.predict.detailsStatus()];
      case "live":
        return [this.live.detailsStatus()];
      case "annotate":
        return [this.annotate.detailsStatus()];
      case "export":
        return [this.exportWorkflow.detailsStatus()];
      default:
        return [workflowDetailsPendingStatusItem()];
    }
  }
}
