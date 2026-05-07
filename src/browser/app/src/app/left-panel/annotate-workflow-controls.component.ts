import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { BrowserAnnotateWorkflowState } from "../state/browser-annotate-workflow.service";
import {
  WorkflowControlsLayoutComponent,
  WORKFLOW_TEXT_SELECT_BOOLEAN_ACTIONS_TEMPLATE,
} from "../workflow-controls-layout.component";
import type { WorkflowActionButtonConfig, WorkflowBooleanFieldConfig, WorkflowSelectFieldConfig, WorkflowSelectOption, WorkflowTextFieldConfig } from "../workflow-field-list.component";

type AnnotateTextField =
  | "weightsPath"
  | "onnxPath"
  | "tensorrtPath"
  | "outputDir"
  | "split"
  | "backend"
  | "deviceId"
  | "maxDetsPerImage"
  | "threshold";
type AnnotateSelectField = "modelInput" | "compileMode";
type AnnotateBooleanField = "allowFp16" | "fullFrame";
type AnnotateBrowseField =
  | "weightsPath"
  | "onnxPath"
  | "tensorrtPath"
  | "outputDir";

@Component({
  selector: "app-left-panel-annotate-workflow-controls",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [WorkflowControlsLayoutComponent],
  template: `
    ${WORKFLOW_TEXT_SELECT_BOOLEAN_ACTIONS_TEMPLATE}
    @if (store.annotateControls().setupFrame.visible) {
      <p class="section-copy">{{ store.annotateControls().setupFrame.summary }}</p>
      <div class="action-row">
        <button
          type="button"
          [disabled]="!store.annotateControls().setupFrame.reload.enabled"
          (click)="store.reloadSetupFrame()"
        >
          {{ store.annotateControls().setupFrame.reload.label }}
        </button>
        <button
          type="button"
          [disabled]="!store.annotateControls().setupFrame.previous.enabled"
          (click)="store.previousSetupFrame()"
        >
          {{ store.annotateControls().setupFrame.previous.label }}
        </button>
        <button
          type="button"
          [disabled]="!store.annotateControls().setupFrame.next.enabled"
          (click)="store.nextSetupFrame()"
        >
          {{ store.annotateControls().setupFrame.next.label }}
        </button>
      </div>
    }
    @if (store.annotateLiveVisible()) {
      <p class="section-copy">{{ store.annotateLiveSummary() }}</p>
      <div class="action-row">
        <button
          type="button"
          [disabled]="!store.annotateLiveStartEnabled()"
          (click)="store.startLive()"
        >
          {{ store.annotateControls().liveAnnotate.start.label }}
        </button>
        <button
          type="button"
          [disabled]="!store.annotateControls().liveAnnotate.stop.enabled"
          (click)="store.stopLive()"
        >
          {{ store.annotateControls().liveAnnotate.stop.label }}
        </button>
      </div>
    }
    @if (store.annotateControls().save.visible) {
      <p class="section-copy">{{ store.annotateControls().save.summary }}</p>
      <div class="action-row">
        <button
          type="button"
          [disabled]="!store.annotateControls().save.saveNow.enabled"
          (click)="store.requestSaveNow()"
        >
          {{ store.annotateControls().save.saveNow.label }}
        </button>
        <button
          type="button"
          [attr.aria-pressed]="store.annotateControls().save.holdSave.value"
          [disabled]="!store.annotateControls().save.holdSave.enabled"
          (pointerdown)="armAnnotateHoldSave($event)"
          (pointerup)="releaseAnnotateHoldSave()"
          (pointerleave)="releaseAnnotateHoldSave()"
          (pointercancel)="releaseAnnotateHoldSave()"
          (blur)="releaseAnnotateHoldSave()"
        >
          {{ annotateHoldSaveLabel() }}
        </button>
      </div>
    }
    @if (store.annotateControls().brush.visible) {
      <div class="form-grid">
        <label class="field">
          <span>{{ store.annotateControls().brush.radius.label }}</span>
          <input
            #annotateBrushRadius
            type="number"
            inputmode="numeric"
            [attr.min]="store.annotateControls().brush.radius.min"
            [attr.max]="store.annotateControls().brush.radius.max"
            [value]="store.annotateControls().brush.radius.value"
            [disabled]="!store.annotateControls().brush.radius.enabled"
            (change)="store.setBrushRadius(annotateBrushRadius.value)"
          />
        </label>
      </div>
      <p class="section-copy">{{ store.annotateControls().brush.summary }}</p>
    }
    <p class="section-copy">{{ store.annotateControlsSummary() }}</p>
  `,
})
export class AnnotateWorkflowControlsComponent {
  protected readonly store = inject(BrowserAnnotateWorkflowState);
  protected readonly textFields: ReadonlyArray<
    WorkflowTextFieldConfig<AnnotateTextField>
  > = [
    { label: "Weights Path", field: "weightsPath", wide: true, browseAction: "weightsPath" },
    { label: "ONNX Path", field: "onnxPath", wide: true, browseAction: "onnxPath" },
    { label: "TensorRT Path", field: "tensorrtPath", wide: true, browseAction: "tensorrtPath" },
    { label: "Output Dir", field: "outputDir", wide: true, browseAction: "outputDir" },
    { label: "Split", field: "split" },
    { label: "Backend", field: "backend" },
    { label: "Device", field: "deviceId", inputmode: "numeric" },
    { label: "Max Dets", field: "maxDetsPerImage", inputmode: "numeric" },
    { label: "Threshold", field: "threshold", inputmode: "decimal" },
  ];
  protected readonly selectFields: ReadonlyArray<
    WorkflowSelectFieldConfig<AnnotateSelectField>
  > = [
    { label: "Model Input", field: "modelInput" },
    { label: "Compile Mode", field: "compileMode" },
  ];
  protected readonly booleanFields: ReadonlyArray<
    WorkflowBooleanFieldConfig<AnnotateBooleanField>
  > = [
    { label: "Allow FP16", field: "allowFp16" },
    { label: "Full Frame", field: "fullFrame" },
  ];
  protected readonly browseActions: ReadonlyArray<
    WorkflowActionButtonConfig<AnnotateBrowseField>
  > = [];
  protected readonly readTextField = (field: string): string =>
    this.store.draft()[field as AnnotateTextField];
  protected readonly writeTextField = (field: string, value: string): void => {
    this.store.updateTextField(field as AnnotateTextField, value);
  };
  protected readonly readSelectField = (field: string): string =>
    this.store.draft()[field as AnnotateSelectField];
  protected readonly readSelectOptions = (
    field: string,
  ): ReadonlyArray<WorkflowSelectOption> =>
    field === "modelInput"
      ? this.store.annotateModelInputOptions()
      : this.store.compileModeOptions;
  protected readonly writeSelectField = (field: string, value: string): void => {
    if (field === "modelInput") {
      this.store.updateModelInput(value);
      return;
    }
    this.store.updateCompileMode(value);
  };
  protected readonly readBooleanField = (field: string): boolean =>
    this.store.draft()[field as AnnotateBooleanField];
  protected readonly writeBooleanField = (field: string, value: boolean): void => {
    this.store.updateBooleanField(field as AnnotateBooleanField, value);
  };
  protected readonly browseField = (field: string): void => {
    this.store.browseField(field as AnnotateBrowseField);
  };

  protected armAnnotateHoldSave(event: PointerEvent): void {
    if (!this.store.annotateControls().save.holdSave.enabled) {
      return;
    }
    event.preventDefault();
    this.store.setHoldSave(true);
  }

  protected releaseAnnotateHoldSave(): void {
    if (!this.store.annotateControls().save.holdSave.value) {
      return;
    }
    this.store.setHoldSave(false);
  }

  protected annotateHoldSaveLabel(): string {
    const saveControls = this.store.annotateControls().save;
    if (saveControls.holdSaveBlocked) {
      return "Hold Save Blocked";
    }
    if (saveControls.holdSave.value) {
      return "Release to Stop Hold";
    }
    return saveControls.holdSave.label;
  }
}
