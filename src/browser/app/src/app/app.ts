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
      <app-topbar />
      <app-workflow-nav />
      <router-outlet />
      <app-settings-modal />
      <footer class="statusbar">
        <span id="active-workflow">{{ store.footerWorkflow() }}</span>
        <span id="last-intent">last intent: <strong>{{ store.lastIntentLabel() }}</strong></span>
      </footer>
    </div>
  `,
})
export class App {
  protected readonly store = inject(BrowserShellChromeState);
}
