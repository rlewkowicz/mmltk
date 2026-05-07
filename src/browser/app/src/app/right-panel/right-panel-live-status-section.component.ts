import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { BrowserLiveWorkflowState } from "../state/browser-live-workflow.service";

@Component({
  selector: "app-right-panel-live-status-section",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <section class="panel-section">
      <h2>Live Status</h2>
      <dl class="detail-grid">
        @for (entry of store.liveStatusFields(); track entry.label) {
          <div>
            <dt>{{ entry.label }}</dt>
            <dd>{{ entry.value }}</dd>
          </div>
        }
        <div>
          <dt>Mode</dt>
          <dd>{{ liveModeLabel() }}</dd>
        </div>
        <div>
          <dt>Transition</dt>
          <dd>{{ liveTransitionLabel() }}</dd>
        </div>
        <div>
          <dt>Display</dt>
          <dd>{{ livePreviewLabel() }}</dd>
        </div>
      </dl>
      <p class="section-copy">
        {{ store.liveStatus().errorText || "No live errors published." }}
      </p>
      <dl class="detail-grid">
        @for (item of store.liveStatus().statusItems; track item.key) {
          <div>
            <dt>{{ item.label }}</dt>
            <dd>{{ item.summary }}</dd>
          </div>
        }
      </dl>
      <p class="section-copy">{{ store.liveStatusSummary() }}</p>
    </section>
  `,
})
export class RightPanelLiveStatusSectionComponent {
  protected readonly store = inject(BrowserLiveWorkflowState);

  protected liveModeLabel(): string {
    const activeMode = this.store.liveStatus().runtime.activeMode;
    switch (activeMode) {
      case "annotate":
        return "live annotate";
      case "predict":
        return "live predict";
      default:
        return "idle";
    }
  }

  protected liveTransitionLabel(): string {
    const runtime = this.store.liveStatus().runtime;
    if (runtime.activeMode === "annotate" && runtime.startupState !== "idle") {
      return runtime.startupState;
    }
    if (runtime.starting) {
      return "starting";
    }
    if (runtime.stopping) {
      return "stopping";
    }
    if (runtime.showIdleStartError) {
      return "idle-start failed";
    }
    return this.store.liveStatus().showRunningSection ? "steady" : "idle";
  }

  protected livePreviewLabel(): string {
    const preview = this.store.liveStatus().preview;
    if (preview.cropOverlayMode) {
      return "full frame";
    }
    if (preview.fitToCapture) {
      return "fit to capture";
    }
    if (preview.displayStartupState !== "idle") {
      return preview.displayStartupState;
    }
    return preview.hasFrame ? "cropped preview" : "no preview";
  }
}
