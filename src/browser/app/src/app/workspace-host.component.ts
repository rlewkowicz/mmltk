import { ChangeDetectionStrategy, Component, computed, inject } from '@angular/core';
import type { CaptureRegion } from '../host_api';
import { BrowserHostRuntimeState } from './state/browser-host-runtime.service';
import { BrowserWorkspaceStateService } from './state/browser-workspace.service';
import { WorkspaceCanvasComponent } from './workspace/workspace-canvas.component';

@Component({
  selector: 'app-workspace-host',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [WorkspaceCanvasComponent],
  template: `
    <main class="workspace">
      <div class="workspace-header">
        <div>
          <p class="eyebrow">Workspace surface</p>
          <h2 id="workspace-title">{{ workspace.workspace().title }}</h2>
        </div>
        <div class="workspace-status">
          <div class="pill">
            <span>Workspace Bridge</span>
            <strong id="gpu-status">{{ workspaceBridgeLabel() }}</strong>
          </div>
          <div class="pill">
            <span>Workspace Path</span>
            <strong id="shader-status">{{ workspace.workspaceDiagnostics().framePathLabel }}</strong>
          </div>
          <div class="pill">
            <span>Overlay</span>
            <strong>{{ workspace.workspaceDiagnostics().overlayPathLabel }}</strong>
          </div>
        </div>
      </div>

      <div
        class="workspace-frame"
        [class.workspace-frame--blocked]="workspace.workspaceInteractionBlocked()"
        [attr.aria-disabled]="workspace.workspaceInteractionBlocked() ? 'true' : null"
      >
        <mmltk-workspace-canvas />
        @if (workspace.workspaceHardError(); as hardError) {
          <section class="workspace-hard-error" role="alert" aria-live="assertive">
            <div class="workspace-hard-error__panel">
              <p class="workspace-hard-error__eyebrow">Workspace hard error</p>
              <h3 class="workspace-hard-error__title">{{ hardError.title }}</h3>
              <p class="workspace-hard-error__detail">{{ hardError.detail }}</p>
              <p class="workspace-hard-error__hint">{{ hardError.recoveryHint }}</p>
            </div>
          </section>
        }
      </div>

      <div class="workspace-footer">
        <div class="workspace-meta">
          <div class="pill">
            <span>Zoom</span>
            <strong>{{ zoomLabel() }}</strong>
          </div>
          <div class="pill">
            <span>Center</span>
            <strong>{{ centerLabel() }}</strong>
          </div>
          <div class="pill">
            <span>Clip</span>
            <strong>{{ clipLabel() }}</strong>
          </div>
          <div class="pill">
            <span>Viewport</span>
            <strong>{{ viewportLabel() }}</strong>
          </div>
          <div class="pill">
            <span>Overlay Bounds</span>
            <strong>{{ overlayBoundsLabel() }}</strong>
          </div>
          <div class="pill">
            <span>Bridge</span>
            <strong>{{ runtime.transportStatus().runtimeLabel }}</strong>
          </div>
        </div>
        <p id="workspace-note">{{ workspaceNote() }}</p>
      </div>
    </main>
  `,
  styles: [
    `
      .workspace-frame {
        position: relative;
      }

      .workspace-frame--blocked mmltk-workspace-canvas {
        filter: saturate(0.45) brightness(0.58);
      }

      .workspace-hard-error {
        position: absolute;
        inset: 0;
        z-index: 2;
        display: grid;
        place-items: center;
        padding: clamp(1rem, 3vw, 2rem);
        background:
          linear-gradient(180deg, rgba(7, 10, 14, 0.18), rgba(7, 10, 14, 0.78)),
          radial-gradient(circle at top, rgba(255, 188, 92, 0.16), transparent 52%);
        backdrop-filter: blur(6px);
        pointer-events: auto;
        cursor: not-allowed;
      }

      .workspace-hard-error__panel {
        width: min(100%, 32rem);
        padding: clamp(1rem, 2.6vw, 1.5rem);
        border: 1px solid rgba(255, 188, 92, 0.34);
        border-radius: 1rem;
        background: rgba(15, 19, 25, 0.9);
        box-shadow: 0 24px 64px rgba(0, 0, 0, 0.28);
      }

      .workspace-hard-error__eyebrow,
      .workspace-hard-error__detail,
      .workspace-hard-error__hint,
      .workspace-hard-error__title {
        margin: 0;
      }

      .workspace-hard-error__eyebrow {
        color: rgba(255, 205, 130, 0.92);
        font-size: 0.78rem;
        font-weight: 700;
        letter-spacing: 0.08em;
        text-transform: uppercase;
      }

      .workspace-hard-error__title {
        margin-top: 0.5rem;
      }

      .workspace-hard-error__detail {
        margin-top: 0.75rem;
      }

      .workspace-hard-error__hint {
        margin-top: 0.75rem;
        color: rgba(232, 238, 245, 0.78);
      }
    `,
  ],
})
export class WorkspaceHostComponent {
  protected readonly runtime = inject(BrowserHostRuntimeState);
  protected readonly workspace = inject(BrowserWorkspaceStateService);
  protected readonly workspaceBridgeLabel = computed(() => {
    const workspaceBridge = this.runtime.runtimeCapabilityStatus().find(
      (item) => item.key === 'workspace_surface_bridge',
    );
    if (workspaceBridge === undefined) {
      return this.workspace.workspace().gpuStatus;
    }
    switch (workspaceBridge.status) {
      case 'ready':
        return 'bridge ready';
      case 'blocked':
        return 'unavailable';
      default:
        return 'pending';
    }
  });
  protected readonly workspaceNote = computed(() => {
    const details = [
      this.runtime.runtimeCapabilityStatus().find((item) => item.key === 'workspace_surface_bridge')?.detail,
      this.runtime.transportStatus().statusItems.find((item) => item.key === 'workspace_surface_zero_copy')?.detail,
      this.workspace.workspaceViewportDetail(),
    ].filter((detail): detail is string => typeof detail === 'string' && detail.trim().length > 0);

    return details.filter((detail, index, items) => items.indexOf(detail) === index).join(' ');
  });

  protected zoomLabel(): string {
    const zoom = this.workspace.workspaceState().zoom;
    return `${(zoom * 100).toFixed(1)}%`;
  }

  protected centerLabel(): string {
    const workspace = this.workspace.workspaceState();
    return `${workspace.centerX.toFixed(1)}, ${workspace.centerY.toFixed(1)}`;
  }

  protected clipLabel(): string {
    const clip = this.workspace.workspaceClip();
    return clip === null ? 'none' : this.formatRegion(clip);
  }

  protected viewportLabel(): string {
    const report = this.workspace.workspaceCanvasLayout();
    return report === null ? 'pending' : this.formatSurfaceRect(report.viewportBoundsCss);
  }

  protected overlayBoundsLabel(): string {
    const report = this.workspace.workspaceCanvasLayout();
    if (report === null) {
      return 'pending';
    }
    return report.overlayBoundsCss === null
      ? 'none'
      : this.formatSurfaceRect(report.overlayBoundsCss);
  }

  private formatRegion(region: CaptureRegion): string {
    return `${region.x},${region.y} · ${region.width}x${region.height}`;
  }

  private formatSurfaceRect(region: {
    x: number;
    y: number;
    width: number;
    height: number;
  }): string {
    return `${region.x.toFixed(1)},${region.y.toFixed(1)} · ${region.width.toFixed(1)}x${region.height.toFixed(1)}`;
  }
}
