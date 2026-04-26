import { computed, inject, Injectable } from "@angular/core";

import { primaryActionLabelText } from "../../app_shared";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import { BrowserLiveWorkflowState } from "./browser-live-workflow.service";
import { BrowserSourceState } from "./browser-source-state.service";
import { BrowserTrainWorkflowState } from "./browser-train-workflow.service";
import { BrowserWorkflowDraftSharedState } from "./browser-workflow-draft-shared.service";
import { BrowserWorkflowRouteState } from "./browser-workflow-route.service";

@Injectable({ providedIn: "root" })
export class BrowserWorkflowPrimaryActionState {
  private readonly runtime = inject(BrowserHostRuntimeState);
  private readonly workflowRoute = inject(BrowserWorkflowRouteState);
  private readonly shared = inject(BrowserWorkflowDraftSharedState);
  private readonly source = inject(BrowserSourceState);
  private readonly train = inject(BrowserTrainWorkflowState);
  private readonly live = inject(BrowserLiveWorkflowState);

  readonly selectedWorkflow = this.workflowRoute.selectedWorkflow;
  readonly primaryActionLabel = computed(() =>
    primaryActionLabelText(
      this.selectedWorkflow(),
      this.train.draft().executionTarget,
      this.source.draft().kind,
      this.live.livePredictPrimaryActionActive(),
    ),
  );

  runPrimaryAction(): void {
    const workflow = this.selectedWorkflow();
    this.shared.applyWorkflowSettingsIfDirty();
    if (workflow === "train") {
      this.train.runPrimaryAction();
      return;
    }
    if (workflow === "annotate") {
      this.runtime.dispatch("annotate", "annotate.save", {});
      return;
    }
    if (workflow === "validate") {
      this.runtime.dispatch("validate", "validate.start", {});
      return;
    }
    if (workflow === "export") {
      this.runtime.dispatch("export", "export.start", {});
      return;
    }
    this.runtime.dispatch(
      workflow === "live" ? "live" : "predict",
      this.live.livePredictPrimaryActionActive() ? "predict.stop" : "predict.start",
      {},
    );
  }
}
