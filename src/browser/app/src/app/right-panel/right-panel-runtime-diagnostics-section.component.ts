import { ChangeDetectionStrategy, Component, computed, inject } from "@angular/core";

import { BrowserHostRuntimeState } from "../state/browser-host-runtime.service";
import { BrowserWorkspaceStateService } from "../state/browser-workspace.service";

@Component({
  selector: "app-right-panel-runtime-diagnostics-section",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <section class="panel-section">
      <div class="section-header">
        <h2>Runtime Diagnostics</h2>
      </div>
      <dl class="detail-grid">
        <div>
          <dt>Bridge</dt>
          <dd>{{ diagnostics().bridge }}</dd>
        </div>
        <div>
          <dt>Composition</dt>
          <dd>{{ diagnostics().composition }}</dd>
        </div>
        <div>
          <dt>Workspace Path</dt>
          <dd>{{ diagnostics().workspacePath }}</dd>
        </div>
        <div>
          <dt>Presentation</dt>
          <dd>{{ diagnostics().presentation }}</dd>
        </div>
        <div>
          <dt>Overlay</dt>
          <dd>{{ diagnostics().overlay }}</dd>
        </div>
      </dl>
      <p class="section-copy">{{ diagnostics().transportDetail }}</p>
      <p class="section-copy">{{ compositionLayoutDetail() }}</p>

      <h2>Workspace Surface</h2>
      <p class="section-copy">
        {{ surfaceMetadataCopy() }}
      </p>
      <div class="slot-list">
        @if (workspaceSurface() !== null) {
          <div class="slot-card">
            <strong>{{ workspaceSurface()?.surfaceId }}</strong>
            <p class="section-copy">
              {{ workspaceSurface()?.width }}x{{ workspaceSurface()?.height }} ·
              {{ workspaceSurface()?.textureFormat }}
            </p>
            <p class="section-copy">
              revision {{ workspaceSurface()?.revision }} · opaque={{ workspaceSurface()?.opaque }} ·
              upright={{ workspaceSurface()?.upright }}
            </p>
          </div>
        } @else {
          <p class="section-copy">
            No current workspace surface revision is published yet.
          </p>
        }
      </div>
    </section>
  `,
})
export class RightPanelRuntimeDiagnosticsSectionComponent {
  protected readonly runtime = inject(BrowserHostRuntimeState);
  protected readonly workspace = inject(BrowserWorkspaceStateService);
  protected readonly diagnostics = computed(() => {
    const workspaceDiagnostics = this.workspace.workspaceDiagnostics();
    const nativePresentedLive =
      workspaceDiagnostics.framePathLabel === "native-presented live workspace";
    const transportStatus = this.runtime.transportStatus();
    return {
      bridge: transportStatus.runtimeLabel,
      composition: `${workspaceDiagnostics.framePathLabel} · ${this.workspace.rendererStatus()}`,
      workspacePath: workspaceDiagnostics.framePathLabel,
      presentation: nativePresentedLive
        ? "CEF native front-slot present"
        : workspaceDiagnostics.framePathLabel,
      overlay: workspaceDiagnostics.overlayPathLabel,
      transportDetail: nativePresentedLive
        ? (transportStatus.statusItems.find((item) => item.key === "transport")?.detail ??
          transportStatus.detail)
        : transportStatus.detail,
    };
  });
  protected readonly surfaceMetadataCopy = computed(
    () =>
      "Host runtime telemetry now reports the published workspace surface revision directly; legacy imported-frame metadata is no longer part of the browser runtime path.",
  );
  protected readonly workspaceSurface = computed(
    () => this.runtime.snapshot().workspace_surface ?? null,
  );
  protected readonly compositionLayoutDetail = computed(() => {
    const details = [
      this.workspace.workspaceDiagnostics().detail,
      this.workspace.workspaceViewportDetail(),
    ].filter((detail): detail is string => typeof detail === "string" && detail.trim().length > 0);

    return details.filter((detail, index, items) => items.indexOf(detail) === index).join(" ");
  });
}
