import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { BrowserLiveWorkflowState } from "./state/browser-live-workflow.service";

@Component({
  selector: "app-live-preview-controls",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <section class="panel-section">
      <div class="section-header">
        <h2>Live Preview</h2>
      </div>
      <div class="form-grid">
        <label class="field">
          <span>{{ live.livePreviewControls().fitToCapture.label }}</span>
          <input
            #fitToCapture
            type="checkbox"
            [checked]="live.livePreviewControls().fitToCapture.value"
            [disabled]="!live.livePreviewControls().fitToCapture.enabled"
            (change)="live.setPreviewFitToCapture(fitToCapture.checked)"
          />
        </label>
        <label class="field">
          <span>{{ live.livePreviewControls().fullFrameDisplay.label }}</span>
          <input
            #fullFrameDisplay
            type="checkbox"
            [checked]="live.livePreviewControls().fullFrameDisplay.value"
            [disabled]="!live.livePreviewControls().fullFrameDisplay.enabled"
            (change)="live.setPreviewFullFrameDisplay(fullFrameDisplay.checked)"
          />
        </label>
      </div>
      <p class="section-copy">{{ live.livePreviewControlsSummary() }}</p>
    </section>
  `,
})
export class LivePreviewControlsComponent {
  protected readonly live = inject(BrowserLiveWorkflowState);
}
