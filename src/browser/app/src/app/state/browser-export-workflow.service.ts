import { computed, effect, inject, Injectable, signal, untracked } from "@angular/core";

import type { IntentMessage } from "../../host_api";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import {
  buildExportSettingsPatch,
  draftWithField,
  exportBrowseRequestForField,
  exportDraftEquals,
  exportDraftFromSnapshot,
  fileDialogRequestPayload,
  type ExportBrowseField,
  type ExportDraft,
} from "./browser-shell.store.helpers";

@Injectable({ providedIn: "root" })
export class BrowserExportWorkflowState {
  private readonly runtime = inject(BrowserHostRuntimeState);

  readonly draft = signal<ExportDraft>(exportDraftFromSnapshot(this.runtime.snapshot()));
  readonly dirty = signal(false);
  readonly controlsStatus = computed(() => ({
    key: "workflow",
    label: "Export",
    status: "ready" as const,
    summary: "export controls ready",
    detail: "export.start uses the staged export draft.",
  }));
  readonly detailsStatus = computed(() => ({
    key: "export_details",
    label: "Export Details",
    status: "blocked" as const,
    summary: "no separate export detail surface",
    detail: "Export inputs are published in Workflow Controls.",
  }));

  constructor() {
    effect(() => {
      this.runtime.snapshot();
      untracked(() => {
        this.sync();
      });
    });
  }

  patch(updater: (current: ExportDraft) => ExportDraft): void {
    const nextDraft = updater(this.draft());
    this.draft.set(nextDraft);
    this.dirty.set(
      !exportDraftEquals(nextDraft, exportDraftFromSnapshot(this.runtime.snapshot())),
    );
  }

  sync(force = false): void {
    const nextDraft = exportDraftFromSnapshot(this.runtime.snapshot());
    if (force || !this.dirty()) {
      this.draft.set(nextDraft);
      this.dirty.set(false);
      return;
    }
    this.dirty.set(!exportDraftEquals(this.draft(), nextDraft));
  }

  updateTextField(
    field: "weightsPath" | "onnxPath" | "outputPath" | "deviceId" | "opsetVersion",
    value: string,
  ): void {
    this.patch((current) => draftWithField(current, field, value));
  }

  updateBooleanField(
    field: "allowFp16" | "buildTensorRt" | "simplify",
    value: boolean,
  ): void {
    this.patch((current) => draftWithField(current, field, value));
  }

  apply(): IntentMessage | null {
    if (!this.dirty()) {
      return null;
    }
    const message = this.runtime.dispatch("export", "settings.update", {
      patch: buildExportSettingsPatch(this.draft(), this.runtime.snapshot()),
    });
    this.dirty.set(false);
    return message;
  }

  browseField(field: ExportBrowseField): IntentMessage {
    this.apply();
    const request = exportBrowseRequestForField(field, this.draft());
    return this.runtime.dispatch(
      "export",
      "file_dialog.request",
      fileDialogRequestPayload(request),
    );
  }
}
