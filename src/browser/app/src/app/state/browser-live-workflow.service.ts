import { computed, inject, Injectable } from "@angular/core";

import {
  capabilitySummaryText,
  combinedCapabilityStatus,
  type CapabilityStatusViewState,
} from "../../app_shared";
import {
  isLivePredictPrimaryActionActive,
  livePreviewControlsState,
  liveStatusState,
  type BrowserIntentToggleViewState,
} from "../../browser_shell_state";
import type { IntentMessage } from "../../host_api";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";

@Injectable({ providedIn: "root" })
export class BrowserLiveWorkflowState {
  private readonly runtime = inject(BrowserHostRuntimeState);

  readonly liveStatus = computed(() => liveStatusState(this.runtime.snapshot()));
  readonly livePreviewControls = computed(() =>
    livePreviewControlsState(this.runtime.snapshot()),
  );
  readonly livePreviewControlsSummary = computed(() =>
    capabilitySummaryText(
      this.livePreviewControls().statusItems,
      "preview display controls pending",
    ),
  );
  readonly liveStatusSummary = computed(() =>
    capabilitySummaryText(this.liveStatus().statusItems, "live runtime pending"),
  );
  readonly liveStatusFields = computed(() => {
    const status = this.liveStatus();
    return [
      { label: "Controller", value: status.controllerLabel },
      { label: "Preview", value: status.previewLabel },
      { label: "Analyzer", value: status.analyzerLabel, wide: true },
      { label: "Workspace Path", value: status.workspacePathLabel, wide: true },
      { label: "Backend", value: status.backendLabel },
      { label: "Latency", value: status.latencyLabel },
    ];
  });
  readonly controlsStatus = computed<CapabilityStatusViewState>(() => ({
    key: "workflow",
    label: "Live",
    status: combinedCapabilityStatus(this.livePreviewControls().statusItems, "ready"),
    summary: "live controls ready",
    detail: capabilitySummaryText(
      this.livePreviewControls().statusItems,
      "live controls pending",
    ),
  }));
  readonly detailsStatus = computed<CapabilityStatusViewState>(() => ({
    key: "live_runtime",
    label: "Live Runtime",
    status: combinedCapabilityStatus(this.liveStatus().statusItems, "pending"),
    summary: this.liveStatus().hasSnapshotState
      ? "live runtime telemetry published"
      : "live runtime telemetry pending",
    detail: this.liveStatusSummary(),
  }));

  livePredictPrimaryActionActive(): boolean {
    return isLivePredictPrimaryActionActive(this.liveStatus());
  }

  setPreviewFitToCapture(enabled: boolean): IntentMessage | null {
    return this.dispatchToggleControl(this.livePreviewControls().fitToCapture, enabled);
  }

  setPreviewFullFrameDisplay(enabled: boolean): IntentMessage | null {
    return this.dispatchToggleControl(
      this.livePreviewControls().fullFrameDisplay,
      enabled,
    );
  }

  private dispatchToggleControl(
    control: BrowserIntentToggleViewState,
    value: boolean,
  ): IntentMessage | null {
    if (!control.enabled || control.intent === null) {
      return null;
    }
    const payload =
      control.payloadKey === null ? {} : { [control.payloadKey]: value };
    return this.runtime.dispatch(control.workflow, control.intent, payload);
  }
}
