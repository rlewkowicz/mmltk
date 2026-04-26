import { computed, inject, Injectable, signal } from "@angular/core";

import type { Workflow } from "../../host_api";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";

@Injectable({ providedIn: "root" })
export class BrowserWorkflowRouteState {
  private readonly runtime = inject(BrowserHostRuntimeState);

  readonly routeWorkflow = signal<Workflow>(this.runtime.snapshot().active_workflow);
  readonly selectedWorkflow = computed(() => this.routeWorkflow());
}
