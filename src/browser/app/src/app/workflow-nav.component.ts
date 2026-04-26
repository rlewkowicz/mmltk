import { ChangeDetectionStrategy, Component, inject } from '@angular/core';
import { Router } from '@angular/router';
import { BrowserShellChromeState } from './state/browser-shell-chrome.service';
import type { Workflow } from '../host_api';

@Component({
  selector: 'app-workflow-nav',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <nav class="workflow-bar" aria-label="Workflow selection">
      @for (workflow of store.workflows; track workflow.id) {
        <button
          type="button"
          (pointerdown)="store.activateWorkflow(workflow.id)"
          (click)="selectWorkflow(workflow.id)"
          [attr.data-active]="store.selectedWorkflow() === workflow.id"
          [title]="workflow.description"
        >
          {{ workflow.label }}
        </button>
      }
      <button
        type="button"
        [attr.data-active]="store.settingsOpen()"
        (click)="store.openSettings()"
      >
        SETTINGS
      </button>
    </nav>
  `,
})
export class WorkflowNavComponent {
  protected readonly store = inject(BrowserShellChromeState);
  private readonly router = inject(Router);

  protected selectWorkflow(workflow: Workflow): void {
    this.store.activateWorkflow(workflow);
    void this.router.navigate(['/', workflow]);
  }
}
