import {
  ChangeDetectionStrategy,
  Component,
  DestroyRef,
  inject,
} from '@angular/core';
import { ActivatedRoute } from '@angular/router';
import { coerceWorkflow } from './browser-shell.store';
import { BrowserShellChromeState } from './state/browser-shell-chrome.service';
import { WorkflowMainComponent } from './workflow-main.component';
import { WorkflowSidebarComponent } from './workflow-sidebar.component';

@Component({
  selector: 'app-workflow-shell',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [WorkflowMainComponent, WorkflowSidebarComponent],
  template: `
    <div class="shell-grid shell-grid--workflow">
      <app-workflow-main />
      <app-workflow-sidebar />
    </div>
  `,
})
export class WorkflowShellComponent {
  private readonly route = inject(ActivatedRoute);
  private readonly destroyRef = inject(DestroyRef);
  private readonly shell = inject(BrowserShellChromeState);

  constructor() {
    const subscription = this.route.data.subscribe((data) => {
      this.shell.setRouteWorkflow(coerceWorkflow(data['workflow']));
    });
    this.destroyRef.onDestroy(() => {
      subscription.unsubscribe();
    });
  }
}
