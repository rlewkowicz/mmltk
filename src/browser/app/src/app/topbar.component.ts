import { ChangeDetectionStrategy, Component, inject } from '@angular/core';
import { BrowserShellChromeState } from './state/browser-shell-chrome.service';

@Component({
  selector: 'app-topbar',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <header class="topbar">
      <div class="brand-block">
        <p class="eyebrow">Browser desktop shell</p>
        <h1>mmltk</h1>
      </div>
      <div class="topbar-meta">
        <div class="pill">
          <span>Protocol</span>
          <strong>{{ store.snapshot().protocol_version }}</strong>
        </div>
        <div class="pill">
          <span>Revision</span>
          <strong>{{ store.snapshot().state_revision }}</strong>
        </div>
        <div class="pill accent">
          <span>Host</span>
          <strong>{{ store.transportStatus().label }}</strong>
        </div>
      </div>
    </header>
  `,
})
export class TopbarComponent {
  protected readonly store = inject(BrowserShellChromeState);
}
