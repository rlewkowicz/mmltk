import { computed, inject, Injectable } from "@angular/core";

import type { IntentMessage, Workflow } from "../../host_api";
import { BrowserAnnotateWorkflowState } from "./browser-annotate-workflow.service";
import { BrowserExportWorkflowState } from "./browser-export-workflow.service";
import { BrowserPredictWorkflowState } from "./browser-predict-workflow.service";
import { BrowserTrainWorkflowState } from "./browser-train-workflow.service";
import { BrowserValidateWorkflowState } from "./browser-validate-workflow.service";
import { BrowserWorkflowRouteState } from "./browser-workflow-route.service";

@Injectable({ providedIn: "root" })
export class BrowserWorkflowDraftSharedState {
  private readonly workflowRoute = inject(BrowserWorkflowRouteState);
  private readonly train = inject(BrowserTrainWorkflowState);
  private readonly validate = inject(BrowserValidateWorkflowState);
  private readonly predict = inject(BrowserPredictWorkflowState);
  private readonly annotate = inject(BrowserAnnotateWorkflowState);
  private readonly exportWorkflow = inject(BrowserExportWorkflowState);

  readonly routeWorkflow = this.workflowRoute.routeWorkflow;
  readonly selectedWorkflow = this.workflowRoute.selectedWorkflow;
  readonly canApplyWorkflowSettings = computed(() =>
    this.workflowDraftDirty(this.selectedWorkflow()),
  );

  workflowDraftDirty(workflow: Workflow): boolean {
    switch (workflow) {
      case "train":
        return this.train.dirty();
      case "validate":
        return this.validate.dirty();
      case "predict":
      case "live":
        return this.predict.dirty();
      case "annotate":
        return this.annotate.dirty();
      case "export":
        return this.exportWorkflow.dirty();
    }
  }

  applyWorkflowSettings(): IntentMessage | null {
    switch (this.selectedWorkflow()) {
      case "train":
        return this.train.apply();
      case "validate":
        return this.validate.apply();
      case "predict":
      case "live":
        return this.predict.apply();
      case "annotate":
        return this.annotate.apply();
      case "export":
        return this.exportWorkflow.apply();
    }
    return null;
  }

  applyWorkflowSettingsIfDirty(): IntentMessage | null {
    return this.workflowDraftDirty(this.selectedWorkflow())
      ? this.applyWorkflowSettings()
      : null;
  }
}
