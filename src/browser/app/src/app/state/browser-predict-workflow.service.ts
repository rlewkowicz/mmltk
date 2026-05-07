import { computed, effect, inject, Injectable, signal, untracked } from "@angular/core";

import type { IntentMessage } from "../../host_api";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import { BrowserWorkflowRouteState } from "./browser-workflow-route.service";
import {
  buildPredictSettingsPatch,
  compileModeOptions as compileModeOptionsList,
  draftWithField,
  fileDialogRequestPayload,
  hasSelectOptionValue,
  predictBrowseRequestForField,
  predictDraftEquals,
  predictDraftFromSnapshot,
  predictModelInputOptions as predictModelInputOptionsList,
  type PredictBrowseField,
  type PredictDraft,
} from "./browser-shell.store.helpers";

@Injectable({ providedIn: "root" })
export class BrowserPredictWorkflowState {
  private readonly runtime = inject(BrowserHostRuntimeState);
  private readonly workflowRoute = inject(BrowserWorkflowRouteState);

  readonly draft = signal<PredictDraft>(
    predictDraftFromSnapshot(this.runtime.snapshot(), "predict"),
  );
  readonly dirty = signal(false);
  readonly compileModeOptions = compileModeOptionsList();
  readonly selectedWorkflow = this.workflowRoute.selectedWorkflow;
  readonly predictModelInputOptions = computed(() =>
    predictModelInputOptionsList(this.draft().modelInput),
  );
  readonly controlsStatus = computed(() => ({
    key: "workflow",
    label: "Predict",
    status: "ready" as const,
    summary: "predict controls ready",
    detail: "predict.start uses the staged predict draft.",
  }));
  readonly detailsStatus = computed(() => ({
    key: "predict_details",
    label: "Predict Details",
    status: "ready" as const,
    summary: "predict detail editor ready",
    detail: "Predict uses the shared inference draft published in the host snapshot.",
  }));

  constructor() {
    effect(() => {
      this.runtime.snapshot();
      untracked(() => {
        this.sync();
      });
    });
  }

  patch(updater: (current: PredictDraft) => PredictDraft): void {
    const nextDraft = updater(this.draft());
    this.draft.set(nextDraft);
    this.dirty.set(
      !predictDraftEquals(
        nextDraft,
        predictDraftFromSnapshot(this.runtime.snapshot(), "predict"),
      ),
    );
  }

  sync(force = false): void {
    const nextDraft = predictDraftFromSnapshot(this.runtime.snapshot(), "predict");
    if (force || !this.dirty()) {
      this.draft.set(nextDraft);
      this.dirty.set(false);
      return;
    }
    this.dirty.set(!predictDraftEquals(this.draft(), nextDraft));
  }

  updateTextField(
    field:
      | "weightsPath"
      | "onnxPath"
      | "tensorrtPath"
      | "outputPath"
      | "backend"
      | "batchSize"
      | "maxDetsPerImage"
      | "liveSplitCount"
      | "cpuAffinity"
      | "deviceId"
      | "workers"
      | "lanes"
      | "threshold",
    value: string,
  ): void {
    this.patch((current) => draftWithField(current, field, value));
  }

  updateModelInput(modelInput: string): void {
    if (!hasSelectOptionValue(this.predictModelInputOptions(), modelInput)) {
      return;
    }
    this.patch((current) => draftWithField(current, "modelInput", modelInput));
  }

  updateCompileMode(mode: string): void {
    if (!hasSelectOptionValue(this.compileModeOptions, mode)) {
      return;
    }
    this.patch((current) => draftWithField(current, "compileMode", mode));
  }

  updateBooleanField(field: "allowFp16" | "progressBar", value: boolean): void {
    this.patch((current) => draftWithField(current, field, value));
  }

  apply(): IntentMessage | null {
    if (!this.dirty()) {
      return null;
    }
    const workflow = this.selectedWorkflow() === "live" ? "live" : "predict";
    const message = this.runtime.dispatch(workflow, "settings.update", {
      patch: buildPredictSettingsPatch(workflow, this.draft(), this.runtime.snapshot()),
    });
    this.dirty.set(false);
    return message;
  }

  browseField(field: PredictBrowseField): IntentMessage {
    this.apply();
    const workflow = this.selectedWorkflow() === "live" ? "live" : "predict";
    const request = predictBrowseRequestForField(workflow, field, this.draft());
    return this.runtime.dispatch(
      workflow,
      "file_dialog.request",
      fileDialogRequestPayload(request),
    );
  }
}
