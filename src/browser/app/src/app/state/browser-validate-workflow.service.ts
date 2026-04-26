import { computed, effect, inject, Injectable, signal, untracked } from "@angular/core";

import type { IntentMessage } from "../../host_api";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import {
  buildValidateSettingsPatch,
  draftWithField,
  fileDialogRequestPayload,
  validateBrowseRequestForField,
  validateDraftEquals,
  validateDraftFromSnapshot,
  type ValidateBrowseField,
  type ValidateDraft,
} from "./browser-shell.store.helpers";

@Injectable({ providedIn: "root" })
export class BrowserValidateWorkflowState {
  private readonly runtime = inject(BrowserHostRuntimeState);

  readonly draft = signal<ValidateDraft>(validateDraftFromSnapshot(this.runtime.snapshot()));
  readonly dirty = signal(false);
  readonly controlsStatus = computed(() => ({
    key: "workflow",
    label: "Validate",
    status: "ready" as const,
    summary: "validate controls ready",
    detail: "validate.start uses the staged validation draft.",
  }));
  readonly detailsStatus = computed(() => ({
    key: "validate_details",
    label: "Validate Details",
    status: "ready" as const,
    summary: "validate detail editor ready",
    detail:
      "Report paths, output paths, and execution limits are available in Workflow Details.",
  }));

  constructor() {
    effect(() => {
      this.runtime.snapshot();
      untracked(() => {
        this.sync();
      });
    });
  }

  patch(updater: (current: ValidateDraft) => ValidateDraft): void {
    const nextDraft = updater(this.draft());
    this.draft.set(nextDraft);
    this.dirty.set(
      !validateDraftEquals(
        nextDraft,
        validateDraftFromSnapshot(this.runtime.snapshot()),
      ),
    );
  }

  sync(force = false): void {
    const nextDraft = validateDraftFromSnapshot(this.runtime.snapshot());
    if (force || !this.dirty()) {
      this.draft.set(nextDraft);
      this.dirty.set(false);
      return;
    }
    this.dirty.set(!validateDraftEquals(this.draft(), nextDraft));
  }

  updateTextField(
    field:
      | "compiledPath"
      | "sourceDir"
      | "onnxPath"
      | "tensorrtPath"
      | "saveEnginePath"
      | "reportJsonPath"
      | "split"
      | "evalOrder"
      | "resolution"
      | "limitImages"
      | "alignmentImages"
      | "evalMaxDets"
      | "batchSize"
      | "prefetchFactor"
      | "cpuAffinity"
      | "deviceId"
      | "workers",
    value: string,
  ): void {
    this.patch((current) => draftWithField(current, field, value));
  }

  updateBooleanField(
    field: "allowFp16" | "writeReportJson" | "recompile" | "profile",
    value: boolean,
  ): void {
    this.patch((current) => draftWithField(current, field, value));
  }

  apply(): IntentMessage | null {
    if (!this.dirty()) {
      return null;
    }
    const message = this.runtime.dispatch("validate", "settings.update", {
      patch: buildValidateSettingsPatch(this.draft(), this.runtime.snapshot()),
    });
    this.dirty.set(false);
    return message;
  }

  browseField(field: ValidateBrowseField): IntentMessage {
    this.apply();
    const request = validateBrowseRequestForField(field, this.draft());
    return this.runtime.dispatch(
      "validate",
      "file_dialog.request",
      fileDialogRequestPayload(request),
    );
  }
}
