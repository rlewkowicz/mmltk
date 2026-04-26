import { ChangeDetectionStrategy, Component, computed, inject } from "@angular/core";

import { jobOutputText } from "../../app_shared";
import { BrowserHostRuntimeState } from "../state/browser-host-runtime.service";

@Component({
  selector: "app-right-panel-job-section",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <section class="panel-section">
      <h2>Job</h2>
      <dl class="detail-grid">
        @for (entry of jobFields(); track entry.label) {
          <div>
            <dt>{{ entry.label }}</dt>
            <dd>{{ entry.value }}</dd>
          </div>
        }
      </dl>
      <pre class="console-block">{{ jobOutput() }}</pre>
    </section>
  `,
})
export class RightPanelJobSectionComponent {
  private readonly runtime = inject(BrowserHostRuntimeState);

  protected readonly jobFields = computed(() => {
    const job = this.runtime.snapshot().job;
    return [
      { label: "Status", value: job.running ? "running" : "idle" },
      { label: "Label", value: job.label.trim().length > 0 ? job.label : "-" },
      { label: "Summary", value: job.summary.trim().length > 0 ? job.summary : "-" },
      { label: "Error", value: job.error.trim().length > 0 ? job.error : "-" },
    ];
  });
  protected readonly jobOutput = computed(() => {
    return jobOutputText(this.runtime.snapshot().job);
  });
}
