import { computed, inject, Injectable, signal } from "@angular/core";

import { workflowNavItems, type WorkflowNavItem } from "../../app_shared";
import type { Workflow } from "../../host_api";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import { BrowserSourceState } from "./browser-source-state.service";
import { BrowserWorkflowRouteState } from "./browser-workflow-route.service";

@Injectable({ providedIn: "root" })
export class BrowserShellChromeState {
  private readonly runtime = inject(BrowserHostRuntimeState);
  private readonly workflowRoute = inject(BrowserWorkflowRouteState);
  private readonly source = inject(BrowserSourceState);
  private pendingViewIntentWorkflow: Workflow | null = null;

  readonly workflows: readonly WorkflowNavItem[] = workflowNavItems;
  readonly snapshot = this.runtime.snapshot;
  readonly transportStatus = this.runtime.transportStatus;
  readonly contractHashMatched = this.runtime.contractHashMatched;
  readonly contractMismatchDetail = this.runtime.contractMismatchDetail;
  readonly selectedWorkflow = this.workflowRoute.selectedWorkflow;
  readonly settingsOpen = signal(false);
  readonly lastIntentLabel = computed(() => {
    const intent = this.runtime.lastIntent();
    return intent === null ? "none" : `${intent.intent} -> ${intent.workflow}`;
  });
  readonly footerWorkflow = computed(() =>
    this.snapshot().active_workflow === this.selectedWorkflow()
      ? `workflow: ${this.selectedWorkflow()}`
      : `workflow: ${this.selectedWorkflow()} (host ${this.snapshot().active_workflow})`,
  );

  activateWorkflow(workflow: Workflow): void {
    this.setRouteWorkflow(workflow);
    if (this.snapshot().active_workflow === workflow) {
      this.pendingViewIntentWorkflow = null;
      return;
    }
    if (this.pendingViewIntentWorkflow !== workflow) {
      this.runtime.dispatch(workflow, "settings.update", {
        patch: {
          current_view: workflow,
        },
      });
      this.pendingViewIntentWorkflow = workflow;
    }
  }

  setRouteWorkflow(workflow: Workflow): void {
    this.workflowRoute.routeWorkflow.set(workflow);
    this.source.sync(true);
  }

  openSettings(): void {
    this.settingsOpen.set(true);
  }

  closeSettings(): void {
    this.settingsOpen.set(false);
  }
}
