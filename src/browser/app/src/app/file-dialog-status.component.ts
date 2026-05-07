import { ChangeDetectionStrategy, Component, computed, inject } from "@angular/core";

import { compactDisplayText, isRecord } from "../app_shared";
import { BrowserHostRuntimeState } from "./state/browser-host-runtime.service";

@Component({
  selector: "app-file-dialog-status",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  styles: [
    `
      :host {
        display: contents;
      }
    `,
  ],
  template: `
    @if (visible()) {
      <section class="panel-section panel-section--compact">
        <h2>File Dialog</h2>
        <dl class="detail-grid detail-grid--dense">
          <div>
            <dt>Status</dt>
            <dd>{{ active() ? "active" : "idle" }}</dd>
          </div>
          <div>
            <dt>Title</dt>
            <dd>{{ title() }}</dd>
          </div>
        </dl>
        @if (error().length > 0) {
          <p class="status-copy" data-tone="error">{{ error() }}</p>
        }
      </section>
    }
  `,
})
export class FileDialogStatusComponent {
  private readonly runtime = inject(BrowserHostRuntimeState);
  private readonly dialog = computed(() => {
    const root = this.runtime.snapshot().workflow_state;
    return isRecord(root.file_dialog) ? root.file_dialog : {};
  });

  protected readonly active = computed(() => this.dialog().active === true);
  protected readonly title = computed(() =>
    compactDisplayText(this.dialog().title ?? this.dialog().dialog_id, "-"),
  );
  protected readonly error = computed(() => compactDisplayText(this.dialog().error, ""));
  protected readonly visible = computed(() => this.active() || this.error().length > 0);
}
