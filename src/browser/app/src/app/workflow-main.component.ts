import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { ExportWorkflowControlsComponent } from "./left-panel/export-workflow-controls.component";
import { SourceSectionComponent } from "./left-panel/source-section.component";
import { TrainWorkflowControlsComponent } from "./left-panel/train-workflow-controls.component";
import { ValidateWorkflowControlsComponent } from "./left-panel/validate-workflow-controls.component";
import { ModelConfigSectionComponent } from "./model-config-section.component";
import { PredictRuntimeFieldsComponent } from "./predict-runtime-fields.component";
import { PredictWorkflowControlsComponent } from "./predict-workflow-controls.component";
import { RightPanelWorkflowTrainDetailsComponent } from "./right-panel/right-panel-workflow-train-details.component";
import { RightPanelWorkflowValidateDetailsComponent } from "./right-panel/right-panel-workflow-validate-details.component";
import { BrowserWorkflowRouteState } from "./state/browser-workflow-route.service";
import { WorkflowActionsSectionComponent } from "./workflow-actions-section.component";
import { WorkspaceHostComponent } from "./workspace-host.component";

@Component({
  selector: "app-workflow-main",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [
    ExportWorkflowControlsComponent,
    ModelConfigSectionComponent,
    PredictRuntimeFieldsComponent,
    PredictWorkflowControlsComponent,
    RightPanelWorkflowTrainDetailsComponent,
    RightPanelWorkflowValidateDetailsComponent,
    SourceSectionComponent,
    TrainWorkflowControlsComponent,
    ValidateWorkflowControlsComponent,
    WorkflowActionsSectionComponent,
    WorkspaceHostComponent,
  ],
  template: `
    <main class="workflow-main" [attr.data-workflow]="workflow.selectedWorkflow()">
      @if (workflow.selectedWorkflow() !== "annotate") {
        <app-model-config-section />
      }

      @switch (workflow.selectedWorkflow()) {
        @case ("train") {
          <div class="workflow-main-grid">
            <section class="panel-section">
              <div class="section-header">
                <h2>Train</h2>
              </div>
              <app-left-panel-train-workflow-controls />
            </section>
            <section class="panel-section">
              <div class="section-header">
                <h2>Execution Target</h2>
              </div>
              <app-right-panel-workflow-train-details />
            </section>
            <app-workflow-actions-section class="workflow-span" />
          </div>
        }
        @case ("validate") {
          <div class="workflow-main-grid">
            <section class="panel-section">
              <div class="section-header">
                <h2>Validate</h2>
              </div>
              <app-left-panel-validate-workflow-controls />
            </section>
            <section class="panel-section">
              <div class="section-header">
                <h2>Validation Details</h2>
              </div>
              <app-right-panel-workflow-validate-details />
            </section>
            <app-workflow-actions-section class="workflow-span" />
          </div>
        }
        @case ("predict") {
          <div class="workflow-main-grid">
            <app-left-panel-source-section />
            <section class="panel-section">
              <div class="section-header">
                <h2>Predict</h2>
              </div>
              <app-predict-workflow-controls [liveMode]="false" />
            </section>
            <section class="panel-section">
              <div class="section-header">
                <h2>Runtime</h2>
              </div>
              <app-predict-runtime-fields [liveMode]="false" />
            </section>
            <app-workflow-actions-section />
          </div>
        }
        @case ("live") {
          <div class="workflow-main-grid workflow-main-grid--live">
            <app-left-panel-source-section />
            <section class="panel-section">
              <div class="section-header">
                <h2>Live Predict</h2>
              </div>
              <app-predict-workflow-controls [liveMode]="true" />
            </section>
            <section class="panel-section">
              <div class="section-header">
                <h2>Runtime</h2>
              </div>
              <app-predict-runtime-fields [liveMode]="true" />
            </section>
            <app-workflow-actions-section />
            <app-workspace-host class="workflow-span" />
          </div>
        }
        @case ("annotate") {
          <app-workspace-host />
        }
        @case ("export") {
          <div class="workflow-main-grid">
            <section class="panel-section">
              <div class="section-header">
                <h2>Export</h2>
              </div>
              <app-left-panel-export-workflow-controls />
            </section>
            <app-workflow-actions-section />
          </div>
        }
      }
    </main>
  `,
})
export class WorkflowMainComponent {
  protected readonly workflow = inject(BrowserWorkflowRouteState);
}
