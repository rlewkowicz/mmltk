import { ChangeDetectionStrategy, Component, inject } from '@angular/core';
import { RouterOutlet } from '@angular/router';
import { BrowserShellChromeState } from './state/browser-shell-chrome.service';
import { SettingsModalComponent } from './settings-modal.component';
import { TopbarComponent } from './topbar.component';
import { WorkflowNavComponent } from './workflow-nav.component';

@Component({
  selector: 'app-root',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [RouterOutlet, SettingsModalComponent, TopbarComponent, WorkflowNavComponent],
  template: `
    <div class="shell">
      @if (!store.contractHashMatched()) {
        <main class="contract-error">
          <h1>Browser UI contract mismatch</h1>
          <p>{{ store.contractMismatchDetail() }}</p>
        </main>
      } @else {
        <app-topbar />
        <app-workflow-nav />
        <router-outlet />
        <app-settings-modal />
      }
    </div>
  `,
})
export class App {
  protected readonly store = inject(BrowserShellChromeState);
}
