import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { BrowserTrainWorkflowState } from "../state/browser-train-workflow.service";

@Component({
  selector: "app-right-panel-train-runtime-section",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <section class="panel-section">
      <h2>Train Runtime</h2>
      <dl class="detail-grid">
        @for (entry of store.trainRuntimeFields(); track entry.label) {
          <div>
            <dt>{{ entry.label }}</dt>
            <dd>{{ entry.value }}</dd>
          </div>
        }
      </dl>
      <p class="section-copy">{{ store.trainRuntimeNote() }}</p>
    </section>
  `,
})
export class RightPanelTrainRuntimeSectionComponent {
  protected readonly store = inject(BrowserTrainWorkflowState);
}
