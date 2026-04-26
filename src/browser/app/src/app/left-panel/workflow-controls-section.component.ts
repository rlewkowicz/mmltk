import { ChangeDetectionStrategy, Component, inject } from '@angular/core';
import { PredictWorkflowControlsComponent } from '../predict-workflow-controls.component';
import { AnnotateWorkflowControlsComponent } from './annotate-workflow-controls.component';
import { ExportWorkflowControlsComponent } from './export-workflow-controls.component';
import { BrowserWorkflowPanelsState } from '../state/browser-workflow-panels.service';
import { TrainWorkflowControlsComponent } from './train-workflow-controls.component';
import { ValidateWorkflowControlsComponent } from './validate-workflow-controls.component';

@Component({
  selector: 'app-left-panel-workflow-controls-section',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [
    AnnotateWorkflowControlsComponent,
    ExportWorkflowControlsComponent,
    PredictWorkflowControlsComponent,
    TrainWorkflowControlsComponent,
    ValidateWorkflowControlsComponent,
  ],
  template: `
    <section class="panel-section">
      <div class="section-header">
        <h2>Workflow Controls</h2>
        <button
          type="button"
          [disabled]="!panels.canApplyWorkflowSettings()"
          (click)="panels.applyWorkflowSettings()"
        >
          Apply Workflow
        </button>
      </div>
      @switch (panels.selectedWorkflow()) {
        @case ('train') {
          <app-left-panel-train-workflow-controls />
        }
        @case ('validate') {
          <app-left-panel-validate-workflow-controls />
        }
        @case ('predict') {
          <app-predict-workflow-controls [liveMode]="false" />
        }
        @case ('live') {
          <app-predict-workflow-controls [liveMode]="true" />
        }
        @case ('annotate') {
          <app-left-panel-annotate-workflow-controls />
        }
        @case ('export') {
          <app-left-panel-export-workflow-controls />
        }
      }
      <dl class="detail-grid">
        @for (item of panels.workflowControlStatus(); track item.key) {
          <div>
            <dt>{{ item.label }}</dt>
            <dd>{{ item.summary }}</dd>
          </div>
        }
      </dl>
      <p class="section-copy">{{ panels.workflowControlsNote() }}</p>
    </section>
  `,
})
export class WorkflowControlsSectionComponent {
  protected readonly panels = inject(BrowserWorkflowPanelsState);
}
