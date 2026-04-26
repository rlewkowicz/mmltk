import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { PredictRuntimeFieldsComponent } from "../predict-runtime-fields.component";
import { BrowserWorkflowPanelsState } from "../state/browser-workflow-panels.service";
import { RightPanelWorkflowTrainDetailsComponent } from "./right-panel-workflow-train-details.component";
import { RightPanelWorkflowValidateDetailsComponent } from "./right-panel-workflow-validate-details.component";

@Component({
  selector: "app-right-panel-workflow-details-section",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [
    PredictRuntimeFieldsComponent,
    RightPanelWorkflowTrainDetailsComponent,
    RightPanelWorkflowValidateDetailsComponent,
  ],
  template: `
    <section class="panel-section">
      <div class="section-header">
        <h2>Workflow Details</h2>
        <button
          type="button"
          [disabled]="!panels.canApplyWorkflowSettings()"
          (click)="panels.applyWorkflowSettings()"
        >
          Apply Details
        </button>
      </div>
      @switch (panels.selectedWorkflow()) {
        @case ("train") {
          <app-right-panel-workflow-train-details />
        }
        @case ("validate") {
          <app-right-panel-workflow-validate-details />
        }
        @case ("predict") {
          <app-predict-runtime-fields [liveMode]="false" />
        }
        @case ("live") {
          <app-predict-runtime-fields [liveMode]="true" />
        }
      }
      <dl class="detail-grid">
        @for (item of panels.workflowDetailsStatus(); track item.key) {
          <div>
            <dt>{{ item.label }}</dt>
            <dd>{{ item.summary }}</dd>
          </div>
        }
      </dl>
      <p class="section-copy">{{ panels.workflowDetailsNote() }}</p>
    </section>
  `,
})
export class RightPanelWorkflowDetailsSectionComponent {
  protected readonly panels = inject(BrowserWorkflowPanelsState);
}
