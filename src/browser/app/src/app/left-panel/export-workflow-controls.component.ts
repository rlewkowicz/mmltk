import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { BrowserExportWorkflowState } from "../state/browser-export-workflow.service";
import {
  WORKFLOW_TEXT_BOOLEAN_ACTIONS_TEMPLATE,
  WorkflowControlsLayoutComponent,
} from "../workflow-controls-layout.component";
import {
  WorkflowTextBooleanActionBindings,
  type WorkflowActionButtonConfig,
  type WorkflowBooleanFieldConfig,
  type WorkflowTextFieldConfig,
} from "../workflow-field-list.component";

type ExportTextField = Parameters<BrowserExportWorkflowState["updateTextField"]>[0];
type ExportBooleanField = Parameters<BrowserExportWorkflowState["updateBooleanField"]>[0];
type ExportBrowseField = Parameters<BrowserExportWorkflowState["browseField"]>[0];

@Component({
  selector: "app-left-panel-export-workflow-controls",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [WorkflowControlsLayoutComponent],
  template: WORKFLOW_TEXT_BOOLEAN_ACTIONS_TEMPLATE,
})
export class ExportWorkflowControlsComponent extends WorkflowTextBooleanActionBindings<
  ExportTextField,
  ExportBooleanField,
  ExportBrowseField
> {
  protected override readonly store = inject(BrowserExportWorkflowState);
  protected readonly textFields: ReadonlyArray<
    WorkflowTextFieldConfig<ExportTextField>
  > = [
    { label: "Weights Path", field: "weightsPath", wide: true },
    { label: "ONNX Path", field: "onnxPath", wide: true },
    { label: "Output Path", field: "outputPath", wide: true },
    { label: "Device", field: "deviceId", inputmode: "numeric" },
    { label: "Opset", field: "opsetVersion", inputmode: "numeric" },
  ];
  protected readonly booleanFields: ReadonlyArray<
    WorkflowBooleanFieldConfig<ExportBooleanField>
  > = [
    { label: "Allow FP16", field: "allowFp16" },
    { label: "Build TensorRT", field: "buildTensorRt" },
    { label: "Simplify", field: "simplify" },
  ];
  protected readonly browseActions: ReadonlyArray<
    WorkflowActionButtonConfig<ExportBrowseField>
  > = [
    { label: "Weights", action: "weightsPath" },
    { label: "ONNX", action: "onnxPath" },
    { label: "Output", action: "outputPath" },
  ];
}
