import { Component, Input } from "@angular/core";

import {
  WorkflowActionButtonRowComponent,
  type WorkflowActionButtonConfig,
  WorkflowBooleanFieldListComponent,
  type WorkflowBooleanFieldConfig,
  WorkflowSelectFieldListComponent,
  type WorkflowSelectFieldConfig,
  type WorkflowSelectOption,
  WorkflowTextFieldListComponent,
  type WorkflowTextFieldConfig,
} from "./workflow-field-list.component";

export const WORKFLOW_TEXT_BOOLEAN_ACTIONS_TEMPLATE = `
  <app-workflow-controls-layout
    [textFields]="textFields"
    [readTextValue]="readTextField"
    [writeTextValue]="writeTextField"
    [booleanFields]="booleanFields"
    [readBooleanValue]="readBooleanField"
    [writeBooleanValue]="writeBooleanField"
    [actions]="browseActions"
    [triggerAction]="browseField"
  />
`;

export const WORKFLOW_TEXT_SELECT_BOOLEAN_ACTIONS_TEMPLATE = `
  <app-workflow-controls-layout
    [textFields]="textFields"
    [readTextValue]="readTextField"
    [writeTextValue]="writeTextField"
    [selectFields]="selectFields"
    [readSelectValue]="readSelectField"
    [writeSelectValue]="writeSelectField"
    [readSelectOptions]="readSelectOptions"
    [booleanFields]="booleanFields"
    [readBooleanValue]="readBooleanField"
    [writeBooleanValue]="writeBooleanField"
    [actions]="browseActions"
    [triggerAction]="browseField"
  />
`;

@Component({
  selector: "app-workflow-controls-layout",
  standalone: true,
  imports: [
    WorkflowActionButtonRowComponent,
    WorkflowBooleanFieldListComponent,
    WorkflowSelectFieldListComponent,
    WorkflowTextFieldListComponent,
  ],
  template: `
    <div class="form-grid">
      @if (textFields.length > 0) {
        <app-workflow-text-field-list
          [fields]="textFields"
          [readValue]="readTextValue"
          [writeValue]="writeTextValue"
          [browse]="triggerAction"
        />
      }
      @if (selectFields.length > 0) {
        <app-workflow-select-field-list
          [fields]="selectFields"
          [readValue]="readSelectValue"
          [writeValue]="writeSelectValue"
          [readOptions]="readSelectOptions"
        />
      }
      @if (booleanFields.length > 0) {
        <app-workflow-boolean-field-list
          [fields]="booleanFields"
          [readValue]="readBooleanValue"
          [writeValue]="writeBooleanValue"
        />
      }
    </div>
    @if (actions.length > 0) {
      <app-workflow-action-button-row
        [actions]="actions"
        [triggerAction]="triggerAction"
      />
    }
  `,
})
export class WorkflowControlsLayoutComponent {
  @Input()
  textFields: ReadonlyArray<WorkflowTextFieldConfig<string>> = [];

  @Input()
  readTextValue: (field: string) => string = () => "";

  @Input()
  writeTextValue: (field: string, value: string) => void = () => {};

  @Input()
  selectFields: ReadonlyArray<WorkflowSelectFieldConfig<string>> = [];

  @Input()
  readSelectValue: (field: string) => string = () => "";

  @Input()
  writeSelectValue: (field: string, value: string) => void = () => {};

  @Input()
  readSelectOptions: (field: string) => ReadonlyArray<WorkflowSelectOption> = () => [];

  @Input()
  booleanFields: ReadonlyArray<WorkflowBooleanFieldConfig<string>> = [];

  @Input()
  readBooleanValue: (field: string) => boolean = () => false;

  @Input()
  writeBooleanValue: (field: string, value: boolean) => void = () => {};

  @Input()
  actions: ReadonlyArray<WorkflowActionButtonConfig<string>> = [];

  @Input()
  triggerAction: (action: string) => void = () => {};
}
