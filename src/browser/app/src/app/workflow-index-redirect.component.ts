import { ChangeDetectionStrategy, Component, OnInit, inject } from '@angular/core';
import { Router } from '@angular/router';
import { BrowserShellChromeState } from './state/browser-shell-chrome.service';

@Component({
  selector: 'app-workflow-index-redirect',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: '',
})
export class WorkflowIndexRedirectComponent implements OnInit {
  private readonly router = inject(Router);
  private readonly store = inject(BrowserShellChromeState);

  ngOnInit(): void {
    void this.router.navigate([this.store.snapshot().active_workflow], {
      replaceUrl: true,
    });
  }
}
