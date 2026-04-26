import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { RightPanelAnnotateSidebarSectionComponent } from "./right-panel/right-panel-annotate-sidebar-section.component";
import { RightPanelJobSectionComponent } from "./right-panel/right-panel-job-section.component";
import { RightPanelLiveStatusSectionComponent } from "./right-panel/right-panel-live-status-section.component";
import { RightPanelRuntimeDiagnosticsSectionComponent } from "./right-panel/right-panel-runtime-diagnostics-section.component";
import { RightPanelShellSettingsSectionComponent } from "./right-panel/right-panel-shell-settings-section.component";
import { RightPanelTrainRuntimeSectionComponent } from "./right-panel/right-panel-train-runtime-section.component";
import { RightPanelWorkflowDetailsSectionComponent } from "./right-panel/right-panel-workflow-details-section.component";
import { RightPanelWorkflowStateSectionComponent } from "./right-panel/right-panel-workflow-state-section.component";
import { BrowserWorkflowRouteState } from "./state/browser-workflow-route.service";

@Component({
  selector: "app-right-panel",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [
    RightPanelAnnotateSidebarSectionComponent,
    RightPanelJobSectionComponent,
    RightPanelLiveStatusSectionComponent,
    RightPanelRuntimeDiagnosticsSectionComponent,
    RightPanelShellSettingsSectionComponent,
    RightPanelTrainRuntimeSectionComponent,
    RightPanelWorkflowDetailsSectionComponent,
    RightPanelWorkflowStateSectionComponent,
  ],
  template: `
    <aside class="panel panel-right">
      <app-right-panel-job-section />

      @if (workflow.selectedWorkflow() === "train") {
        <app-right-panel-train-runtime-section />
      }

      @if (
        workflow.selectedWorkflow() === "live" ||
        workflow.selectedWorkflow() === "predict"
      ) {
        <app-right-panel-live-status-section />
      }

      <app-right-panel-shell-settings-section />
      <app-right-panel-workflow-details-section />

      @if (workflow.selectedWorkflow() === "annotate") {
        <app-right-panel-annotate-sidebar-section />
      }

      <app-right-panel-runtime-diagnostics-section />
      <app-right-panel-workflow-state-section />
    </aside>
  `,
})
export class RightPanelComponent {
  protected readonly workflow = inject(BrowserWorkflowRouteState);
}
