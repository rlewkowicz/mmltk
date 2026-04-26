import { ChangeDetectionStrategy, Component, computed, inject } from "@angular/core";

import { BrowserHostRuntimeState } from "../state/browser-host-runtime.service";

@Component({
  selector: "app-right-panel-workflow-state-section",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <section class="panel-section">
      <h2>Workflow State</h2>
      <pre class="console-block">{{ workflowStateJson() }}</pre>
    </section>
  `,
})
export class RightPanelWorkflowStateSectionComponent {
  private readonly runtime = inject(BrowserHostRuntimeState);

  protected readonly workflowStateJson = computed(() =>
    JSON.stringify(this.runtime.snapshot().workflow_state, null, 2),
  );
}
