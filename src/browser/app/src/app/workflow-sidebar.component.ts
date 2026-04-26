import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { AnnotationSummarySectionComponent } from "./left-panel/annotation-summary-section.component";
import { AnnotateWorkflowControlsComponent } from "./left-panel/annotate-workflow-controls.component";
import { SourceSectionComponent } from "./left-panel/source-section.component";
import { FileDialogStatusComponent } from "./file-dialog-status.component";
import { RightPanelAnnotateSidebarSectionComponent } from "./right-panel/right-panel-annotate-sidebar-section.component";
import { RightPanelJobSectionComponent } from "./right-panel/right-panel-job-section.component";
import { RightPanelLiveStatusSectionComponent } from "./right-panel/right-panel-live-status-section.component";
import { RightPanelRuntimeDiagnosticsSectionComponent } from "./right-panel/right-panel-runtime-diagnostics-section.component";
import { RightPanelTrainRuntimeSectionComponent } from "./right-panel/right-panel-train-runtime-section.component";
import { BrowserWorkflowRouteState } from "./state/browser-workflow-route.service";

@Component({
  selector: "app-workflow-sidebar",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [
    AnnotationSummarySectionComponent,
    AnnotateWorkflowControlsComponent,
    FileDialogStatusComponent,
    RightPanelAnnotateSidebarSectionComponent,
    RightPanelJobSectionComponent,
    RightPanelLiveStatusSectionComponent,
    RightPanelRuntimeDiagnosticsSectionComponent,
    RightPanelTrainRuntimeSectionComponent,
    SourceSectionComponent,
  ],
  template: `
    <aside class="workflow-sidebar" [attr.data-workflow]="workflow.selectedWorkflow()">
      <app-file-dialog-status />

      @switch (workflow.selectedWorkflow()) {
        @case ("train") {
          <app-right-panel-train-runtime-section />
          <app-right-panel-job-section />
        }
        @case ("validate") {
          <app-right-panel-job-section />
        }
        @case ("predict") {
          <app-right-panel-job-section />
        }
        @case ("live") {
          <app-right-panel-live-status-section />
          <app-right-panel-job-section />
          <app-right-panel-runtime-diagnostics-section />
        }
        @case ("annotate") {
          <app-left-panel-source-section />
          <section class="panel-section">
            <div class="section-header">
              <h2>Annotate</h2>
            </div>
            <app-left-panel-annotate-workflow-controls />
          </section>
          <app-left-panel-annotation-summary-section />
          <app-right-panel-annotate-sidebar-section />
          <app-right-panel-runtime-diagnostics-section />
        }
        @case ("export") {
          <app-right-panel-job-section />
        }
      }
    </aside>
  `,
})
export class WorkflowSidebarComponent {
  protected readonly workflow = inject(BrowserWorkflowRouteState);
}
