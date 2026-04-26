import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { BrowserWorkflowPanelsState } from "./state/browser-workflow-panels.service";
import { BrowserWorkflowPrimaryActionState } from "./state/browser-workflow-primary-action.service";

@Component({
  selector: "app-workflow-actions-section",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <section class="panel-section workflow-actions">
      <div class="section-header">
        <h2>Actions</h2>
      </div>
      <div class="action-row">
        <button
          type="button"
          [disabled]="!panels.canApplyWorkflowSettings()"
          (click)="panels.applyWorkflowSettings()"
        >
          Apply Workflow
        </button>
        <button id="run-primary-action" type="button" (click)="primary.runPrimaryAction()">
          {{ primary.primaryActionLabel() }}
        </button>
      </div>
      <p class="section-copy">{{ panels.workflowControlsNote() }}</p>
    </section>
  `,
})
export class WorkflowActionsSectionComponent {
  protected readonly panels = inject(BrowserWorkflowPanelsState);
  protected readonly primary = inject(BrowserWorkflowPrimaryActionState);
}
