import { computed, effect, inject, Injectable, signal, untracked } from "@angular/core";

import {
  capabilitySummaryText,
  combinedCapabilityStatus,
  type CapabilityStatusViewState,
} from "../../app_shared";
import {
  annotateShellControlsState,
  annotateToolPaletteState,
} from "../../browser_shell_state";
import type { IntentMessage } from "../../host_api";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import type { BrowserIntentDispatcher } from "./browser-intent-dispatch";
import {
  dispatchIntentAction,
  dispatchIntentToggle,
  dispatchParsedIntentNumeric,
} from "./browser-intent-dispatch";
import {
  annotateBrowseRequestForField,
  annotateDraftEquals,
  annotateDraftFromSnapshot,
  annotateModelInputOptions as annotateModelInputOptionsList,
  buildAnnotateSettingsPatch,
  compileModeOptions as compileModeOptionsList,
  draftWithField,
  fileDialogRequestPayload,
  hasSelectOptionValue,
  type AnnotateBrowseField,
  type AnnotateDraft,
} from "./browser-shell.store.helpers";

@Injectable({ providedIn: "root" })
export class BrowserAnnotateWorkflowState {
  private readonly runtime = inject(BrowserHostRuntimeState);
  private readonly dispatchIntent: BrowserIntentDispatcher = (
    workflow,
    intent,
    payload,
  ) => this.runtime.dispatch(workflow, intent, payload);

  readonly draft = signal<AnnotateDraft>(annotateDraftFromSnapshot(this.runtime.snapshot()));
  readonly dirty = signal(false);
  readonly compileModeOptions = compileModeOptionsList();
  readonly annotateControls = computed(() =>
    annotateShellControlsState(this.runtime.snapshot()),
  );
  readonly annotateToolPalette = computed(() =>
    annotateToolPaletteState(this.runtime.snapshot()),
  );
  readonly annotateModelInputOptions = computed(() =>
    annotateModelInputOptionsList(this.draft().modelInput),
  );
  readonly annotateControlsSummary = computed(() =>
    capabilitySummaryText(
      this.annotateControls().statusItems,
      "annotate shell controls pending",
    ),
  );
  readonly annotateToolPaletteSummary = computed(() =>
    capabilitySummaryText(
      this.annotateToolPalette().statusItems,
      "annotate tool palette pending",
    ),
  );
  readonly controlsStatus = computed<CapabilityStatusViewState>(() => ({
    key: "workflow",
    label: "Annotate",
    status: combinedCapabilityStatus(this.annotateControls().statusItems, "ready"),
    summary: "annotate controls published",
    detail: capabilitySummaryText(
      this.annotateControls().statusItems,
      "annotate controls pending",
    ),
  }));
  readonly detailsStatus = computed(() => ({
    key: "annotate_details",
    label: "Annotate Details",
    status: "blocked" as const,
    summary: "no separate annotate detail surface",
    detail:
      "Annotate setup, save, and brush controls are exposed in Workflow Controls and the workspace.",
  }));

  constructor() {
    effect(() => {
      this.runtime.snapshot();
      untracked(() => {
        this.sync();
      });
    });
  }

  patch(updater: (current: AnnotateDraft) => AnnotateDraft): void {
    const nextDraft = updater(this.draft());
    this.draft.set(nextDraft);
    this.dirty.set(
      !annotateDraftEquals(
        nextDraft,
        annotateDraftFromSnapshot(this.runtime.snapshot()),
      ),
    );
  }

  sync(force = false): void {
    const nextDraft = annotateDraftFromSnapshot(this.runtime.snapshot());
    if (force || !this.dirty()) {
      this.draft.set(nextDraft);
      this.dirty.set(false);
      return;
    }
    this.dirty.set(!annotateDraftEquals(this.draft(), nextDraft));
  }

  updateTextField(
    field:
      | "weightsPath"
      | "onnxPath"
      | "tensorrtPath"
      | "outputDir"
      | "split"
      | "backend"
      | "maxDetsPerImage"
      | "deviceId"
      | "threshold",
    value: string,
  ): void {
    this.patch((current) => draftWithField(current, field, value));
  }

  updateModelInput(modelInput: string): void {
    if (!hasSelectOptionValue(this.annotateModelInputOptions(), modelInput)) {
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

  updateBooleanField(field: "allowFp16" | "fullFrame", value: boolean): void {
    this.patch((current) => draftWithField(current, field, value));
  }

  apply(): IntentMessage | null {
    if (!this.dirty()) {
      return null;
    }
    const message = this.runtime.dispatch("annotate", "settings.update", {
      patch: buildAnnotateSettingsPatch(this.draft(), this.runtime.snapshot()),
    });
    this.dirty.set(false);
    return message;
  }

  browseField(field: AnnotateBrowseField): IntentMessage {
    this.apply();
    const request = annotateBrowseRequestForField(field, this.draft());
    return this.runtime.dispatch(
      "annotate",
      "file_dialog.request",
      fileDialogRequestPayload(request),
    );
  }

  reloadSetupFrame(): IntentMessage | null {
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().setupFrame.reload,
    );
  }

  previousSetupFrame(): IntentMessage | null {
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().setupFrame.previous,
    );
  }

  nextSetupFrame(): IntentMessage | null {
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().setupFrame.next,
    );
  }

  startLive(): IntentMessage | null {
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().liveAnnotate.start,
    );
  }

  stopLive(): IntentMessage | null {
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().liveAnnotate.stop,
    );
  }

  requestSaveNow(): IntentMessage | null {
    return dispatchIntentAction(
      this.dispatchIntent,
      this.annotateControls().save.saveNow,
    );
  }

  setHoldSave(active: boolean): IntentMessage | null {
    return dispatchIntentToggle(
      this.dispatchIntent,
      this.annotateControls().save.holdSave,
      active,
    );
  }

  setBrushRadius(radius: string | number): IntentMessage | null {
    return dispatchParsedIntentNumeric(
      this.dispatchIntent,
      this.annotateControls().brush.radius,
      radius,
    );
  }
}
