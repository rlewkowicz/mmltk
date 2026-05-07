import { ChangeDetectionStrategy, Component, inject } from "@angular/core";

import { BrowserTrainWorkflowState } from "../state/browser-train-workflow.service";
import {
  WorkflowControlsLayoutComponent,
  WORKFLOW_TEXT_SELECT_BOOLEAN_ACTIONS_TEMPLATE,
} from "../workflow-controls-layout.component";
import type { WorkflowActionButtonConfig, WorkflowBooleanFieldConfig, WorkflowSelectFieldConfig, WorkflowSelectOption, WorkflowTextFieldConfig } from "../workflow-field-list.component";

type TrainTextField = Parameters<BrowserTrainWorkflowState["updateTextField"]>[0];
type TrainSelectField =
  | "executionTarget"
  | "inputMode"
  | "optimizer"
  | "compileMode";
type TrainBooleanField = "amp" | "ema" | "freezeEncoder" | "progressBar";
type TrainBrowseField =
  | "trainCompiledPath"
  | "valCompiledPath"
  | "testCompiledPath"
  | "weightsPath"
  | "resumePath"
  | "outputDir";

@Component({
  selector: "app-left-panel-train-workflow-controls",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [WorkflowControlsLayoutComponent],
  template: WORKFLOW_TEXT_SELECT_BOOLEAN_ACTIONS_TEMPLATE,
})
export class TrainWorkflowControlsComponent {
  protected readonly store = inject(BrowserTrainWorkflowState);
  protected readonly textFields: ReadonlyArray<
    WorkflowTextFieldConfig<TrainTextField>
  > = [
    { label: "Train Compiled", field: "trainCompiledPath", wide: true, browseAction: "trainCompiledPath" },
    { label: "Val Compiled", field: "valCompiledPath", wide: true, browseAction: "valCompiledPath" },
    { label: "Test Compiled", field: "testCompiledPath", wide: true, browseAction: "testCompiledPath" },
    { label: "Weights Path", field: "weightsPath", wide: true, browseAction: "weightsPath" },
    { label: "Resume Path", field: "resumePath", wide: true, browseAction: "resumePath" },
    { label: "Output Dir", field: "outputDir", wide: true, browseAction: "outputDir" },
    { label: "CPU Affinity", field: "cpuAffinity", wide: true },
    { label: "Batch Size", field: "batchSize", inputmode: "numeric" },
    { label: "Val Batch Size", field: "valBatchSize", inputmode: "numeric" },
    { label: "Epochs", field: "epochs", inputmode: "numeric" },
    { label: "Grad Accum", field: "gradAccumSteps", inputmode: "numeric" },
    { label: "Eval Max Dets", field: "evalMaxDets", inputmode: "numeric" },
    { label: "Print Freq", field: "printFreq", inputmode: "numeric" },
    { label: "Prefetch", field: "prefetchFactor", inputmode: "numeric" },
    { label: "Workers", field: "workers", inputmode: "numeric" },
    { label: "Lanes", field: "lanes", inputmode: "numeric" },
    { label: "Momentum", field: "momentum", inputmode: "decimal" },
    { label: "Learning Rate", field: "learningRate", inputmode: "decimal" },
    { label: "LR Encoder", field: "lrEncoder", inputmode: "decimal" },
    { label: "Weight Decay", field: "weightDecay", inputmode: "decimal" },
    { label: "LR Component Decay", field: "lrComponentDecay", inputmode: "decimal" },
    { label: "Encoder Layer Decay", field: "encoderLayerDecay", inputmode: "decimal" },
    { label: "Warmup Epochs", field: "warmupEpochs", inputmode: "decimal" },
    { label: "Warmup Momentum", field: "warmupMomentum", inputmode: "decimal" },
    { label: "LR Min Factor", field: "lrMinFactor", inputmode: "decimal" },
    { label: "Clip Max Norm", field: "clipMaxNorm", inputmode: "decimal" },
    { label: "LR Drop", field: "lrDrop", inputmode: "numeric" },
    { label: "LR Scheduler", field: "lrScheduler" },
  ];
  protected readonly selectFields: ReadonlyArray<
    WorkflowSelectFieldConfig<TrainSelectField>
  > = [
    { label: "Train Target", field: "executionTarget" },
    { label: "Input Mode", field: "inputMode" },
    { label: "Optimizer", field: "optimizer" },
    { label: "Compile Mode", field: "compileMode" },
  ];
  protected readonly booleanFields: ReadonlyArray<
    WorkflowBooleanFieldConfig<TrainBooleanField>
  > = [
    { label: "AMP", field: "amp" },
    { label: "EMA", field: "ema" },
    { label: "Freeze Encoder", field: "freezeEncoder" },
    { label: "Progress Bar", field: "progressBar" },
  ];
  protected readonly browseActions: ReadonlyArray<
    WorkflowActionButtonConfig<TrainBrowseField>
  > = [];
  protected readonly readTextField = (field: string): string =>
    this.store.draft()[field as TrainTextField];
  protected readonly writeTextField = (field: string, value: string): void => {
    this.store.updateTextField(field as TrainTextField, value);
  };
  protected readonly readSelectField = (field: string): string =>
    this.store.draft()[field as TrainSelectField];
  protected readonly readSelectOptions = (
    field: string,
  ): ReadonlyArray<WorkflowSelectOption> => {
    switch (field as TrainSelectField) {
      case "executionTarget":
        return this.store.trainTargetOptions;
      case "inputMode":
        return this.store.trainInputModeOptions;
      case "optimizer":
        return this.store.trainOptimizerOptions;
      case "compileMode":
        return this.store.compileModeOptions;
    }
  };
  protected readonly writeSelectField = (field: string, value: string): void => {
    switch (field as TrainSelectField) {
      case "executionTarget":
        this.store.updateExecutionTarget(value);
        return;
      case "inputMode":
        this.store.updateInputMode(value);
        return;
      case "optimizer":
        this.store.updateOptimizer(value);
        return;
      case "compileMode":
        this.store.updateCompileMode(value);
        return;
    }
  };
  protected readonly readBooleanField = (field: string): boolean =>
    this.store.draft()[field as TrainBooleanField];
  protected readonly writeBooleanField = (field: string, value: boolean): void => {
    this.store.updateBooleanField(field as TrainBooleanField, value);
  };
  protected readonly browseField = (field: string): void => {
    this.store.browseField(field as TrainBrowseField);
  };
}
