import { ChangeDetectionStrategy, Component, inject, Input } from "@angular/core";

import { BrowserPredictWorkflowState } from "./state/browser-predict-workflow.service";
import {
  WorkflowBooleanFieldListComponent,
  type WorkflowBooleanFieldConfig,
  WorkflowTextFieldListComponent,
  type WorkflowTextFieldConfig,
} from "./workflow-field-list.component";

type PredictRuntimeTextField =
  | "batchSize"
  | "maxDetsPerImage"
  | "liveSplitCount";
type PredictRuntimeBooleanField = "progressBar";

@Component({
  selector: "app-predict-runtime-fields",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [WorkflowTextFieldListComponent, WorkflowBooleanFieldListComponent],
  template: `
    <div class="form-grid">
      <app-workflow-text-field-list
        [fields]="textFields()"
        [readValue]="readTextField"
        [writeValue]="writeTextField"
      />
      <app-workflow-boolean-field-list
        [fields]="booleanFields"
        [readValue]="readBooleanField"
        [writeValue]="writeBooleanField"
      />
    </div>
  `,
})
export class PredictRuntimeFieldsComponent {
  @Input({ required: true }) liveMode = false;

  protected readonly store = inject(BrowserPredictWorkflowState);
  protected readonly booleanFields: ReadonlyArray<
    WorkflowBooleanFieldConfig<PredictRuntimeBooleanField>
  > = [{ label: "Progress Bar", field: "progressBar" }];
  protected readonly readTextField = (field: string): string =>
    this.store.draft()[field as PredictRuntimeTextField];
  protected readonly writeTextField = (field: string, value: string): void => {
    this.store.updateTextField(field as PredictRuntimeTextField, value);
  };
  protected readonly readBooleanField = (field: string): boolean =>
    this.store.draft()[field as PredictRuntimeBooleanField];
  protected readonly writeBooleanField = (field: string, value: boolean): void => {
    this.store.updateBooleanField(field as PredictRuntimeBooleanField, value);
  };

  protected textFields(): ReadonlyArray<
    WorkflowTextFieldConfig<PredictRuntimeTextField>
  > {
    return [
      {
        label: "Batch Size",
        field: "batchSize",
        inputmode: "numeric",
        visible: !this.liveMode,
      },
      { label: "Max Dets", field: "maxDetsPerImage", inputmode: "numeric" },
      {
        label: "Live Split Count",
        field: "liveSplitCount",
        inputmode: "numeric",
        visible: this.liveMode,
      },
    ];
  }
}
