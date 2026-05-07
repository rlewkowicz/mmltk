import { ChangeDetectionStrategy, Component, inject } from '@angular/core';
import type { CaptureRegion } from '../host_api';
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
        </div>
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
        text-transform: none;
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
  protected readonly workspace = inject(BrowserWorkspaceStateService);

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

  private formatRegion(region: CaptureRegion): string {
    return `${region.x},${region.y} · ${region.width}x${region.height}`;
  }
}
