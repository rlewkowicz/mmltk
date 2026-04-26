import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { BrowserValidateWorkflowState } from "../state/browser-validate-workflow.service";
import {
  WorkflowControlsLayoutComponent,
  WORKFLOW_TEXT_BOOLEAN_ACTIONS_TEMPLATE,
} from "../workflow-controls-layout.component";
import type {
  WorkflowActionButtonConfig,
  WorkflowBooleanFieldConfig,
  WorkflowTextFieldConfig,
} from "../workflow-field-list.component";
import { WorkflowTextBooleanActionBindings } from "../workflow-field-list.component";

type ValidateTextField = Parameters<BrowserValidateWorkflowState["updateTextField"]>[0];
type ValidateBooleanField = Parameters<
  BrowserValidateWorkflowState["updateBooleanField"]
>[0];
type ValidateBrowseField = Parameters<BrowserValidateWorkflowState["browseField"]>[0];

@Component({
  selector: "app-left-panel-validate-workflow-controls",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [WorkflowControlsLayoutComponent],
  template: WORKFLOW_TEXT_BOOLEAN_ACTIONS_TEMPLATE,
})
export class ValidateWorkflowControlsComponent extends WorkflowTextBooleanActionBindings<
  ValidateTextField,
  ValidateBooleanField,
  ValidateBrowseField
> {
  protected override readonly store = inject(BrowserValidateWorkflowState);
  protected readonly textFields: ReadonlyArray<
    WorkflowTextFieldConfig<ValidateTextField>
  > = [
    { label: "Compiled Dataset", field: "compiledPath", wide: true },
    { label: "Source Root", field: "sourceDir", wide: true },
    { label: "ONNX Path", field: "onnxPath", wide: true },
    { label: "TensorRT Path", field: "tensorrtPath", wide: true },
    { label: "Report JSON", field: "reportJsonPath", wide: true },
    { label: "CPU Affinity", field: "cpuAffinity", wide: true },
    { label: "Device", field: "deviceId", inputmode: "numeric" },
    { label: "Batch Size", field: "batchSize", inputmode: "numeric" },
    { label: "Workers", field: "workers", inputmode: "numeric" },
  ];
  protected readonly booleanFields: ReadonlyArray<
    WorkflowBooleanFieldConfig<ValidateBooleanField>
  > = [
    { label: "Allow FP16", field: "allowFp16" },
    { label: "Write Report", field: "writeReportJson" },
  ];
  protected readonly browseActions: ReadonlyArray<
    WorkflowActionButtonConfig<ValidateBrowseField>
  > = [
    { label: "Dataset", action: "compiledPath" },
    { label: "Source Root", action: "sourceDir" },
    { label: "ONNX", action: "onnxPath" },
    { label: "TensorRT", action: "tensorrtPath" },
    { label: "Report", action: "reportJsonPath" },
  ];
}
