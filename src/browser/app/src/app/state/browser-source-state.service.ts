import { computed, effect, inject, Injectable, signal, untracked } from "@angular/core";

import {
  compactDisplayText,
  labeledValueField,
  type LabeledValueField,
} from "../../app_shared";
import { type IntentMessage } from "../../host_api";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import { BrowserWorkflowRouteState } from "./browser-workflow-route.service";
import {
  buildSourceSettingsPatch,
  fileDialogRequestPayload,
  sourceBrowseRequestForDraft,
  sourceDraftEquals,
  sourceDraftFromSnapshot,
  sourceEditingSupported as sourceEditingSupportedForWorkflow,
  sourceKindOptions as sourceKindOptionsForWorkflow,
  type SourceDraft,
} from "./browser-shell.store.helpers";

export type SourceFieldVm = LabeledValueField;

@Injectable({ providedIn: "root" })
export class BrowserSourceState {
  private readonly runtime = inject(BrowserHostRuntimeState);
  private readonly workflowRoute = inject(BrowserWorkflowRouteState);

  readonly draft = signal<SourceDraft>(
    sourceDraftFromSnapshot(
      this.runtime.snapshot(),
      this.workflowRoute.selectedWorkflow(),
    ),
  );
  readonly dirty = signal(false);

  readonly selectedWorkflow = this.workflowRoute.selectedWorkflow;
  readonly sourceEditingSupported = computed(() =>
    sourceEditingSupportedForWorkflow(this.selectedWorkflow()),
  );
  readonly sourceKindOptions = computed(() =>
    sourceKindOptionsForWorkflow(this.selectedWorkflow(), this.draft().kind),
  );
  readonly browseRequest = computed(() => {
    if (!this.sourceEditingSupported()) {
      return null;
    }
    return sourceBrowseRequestForDraft(this.selectedWorkflow(), this.draft());
  });
  readonly canApplySource = computed(() =>
    this.sourceEditingSupported() && this.dirty(),
  );
  readonly canBrowseSource = computed(() => this.browseRequest() !== null);
  readonly canEditSourceLocator = computed(() => this.draft().kind !== "video_stream");
  readonly canEditSourceRecursive = computed(() =>
    this.draft().kind === "image_folder",
  );
  readonly sourceDetails = computed<SourceFieldVm[]>(() => {
    const source = this.runtime.snapshot().source;
    return [
      labeledValueField("Kind", source.kind),
      labeledValueField("Locator", compactDisplayText(source.locator), true),
      labeledValueField(
        "Capture",
        `${source.capture_width}x${source.capture_height} @ ${source.capture_fps} fps`,
      ),
      labeledValueField(
        "Crop",
        source.has_crop
          ? `${source.crop.x},${source.crop.y} ${source.crop.width}x${source.crop.height}`
          : "none",
      ),
    ];
  });
  readonly sourceControls = computed<SourceFieldVm[]>(() => {
    const source = this.draft();
    return [
      labeledValueField("Source Kind", source.kind),
      labeledValueField("Source Locator", compactDisplayText(source.locator), true),
      labeledValueField("Recursive", source.recursive ? "Yes" : "No"),
      labeledValueField("Device", source.deviceIndex),
      labeledValueField("Capture Width", source.captureWidth),
      labeledValueField("Capture Height", source.captureHeight),
      labeledValueField("Capture FPS", source.captureFps),
      labeledValueField("V4L2 Buffers", source.v4l2BufferCount),
    ];
  });
  readonly sourceNote = computed(() => {
    if (!this.sourceEditingSupported()) {
      return "source editor unavailable";
    }
    if (this.dirty()) {
      return "source changes ready";
    }
    return "source ready";
  });

  constructor() {
    effect(() => {
      this.runtime.snapshot();
      this.selectedWorkflow();
      untracked(() => {
        this.sync();
      });
    });
  }

  patch(updater: (current: SourceDraft) => SourceDraft): void {
    const nextDraft = updater(this.draft());
    this.draft.set(nextDraft);
    this.dirty.set(
      !sourceDraftEquals(
        nextDraft,
        sourceDraftFromSnapshot(this.runtime.snapshot(), this.selectedWorkflow()),
      ),
    );
  }

  sync(force = false): void {
    const nextDraft = sourceDraftFromSnapshot(
      this.runtime.snapshot(),
      this.selectedWorkflow(),
    );
    if (force || !this.dirty()) {
      this.draft.set(nextDraft);
      this.dirty.set(false);
      return;
    }
    this.dirty.set(!sourceDraftEquals(this.draft(), nextDraft));
  }

  apply(): IntentMessage | null {
    if (!this.canApplySource()) {
      return null;
    }
    const message = this.runtime.dispatch(this.selectedWorkflow(), "settings.update", {
      patch: buildSourceSettingsPatch(
        this.selectedWorkflow(),
        this.draft(),
        this.runtime.snapshot(),
      ),
    });
    this.dirty.set(false);
    return message;
  }

  browse(): IntentMessage | null {
    const request = this.browseRequest();
    if (request === null) {
      return null;
    }
    return this.runtime.dispatch(
      this.selectedWorkflow(),
      "file_dialog.request",
      fileDialogRequestPayload(request),
    );
  }
}
