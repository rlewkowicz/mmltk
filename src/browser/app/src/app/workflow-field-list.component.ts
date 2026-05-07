import { Component, Input } from "@angular/core";
import { FormsModule } from "@angular/forms";

export interface WorkflowTextFieldConfig<Field extends string = string> {
  label: string;
  field: Field;
  wide?: boolean;
  inputmode?: "numeric" | "decimal";
  visible?: boolean;
  browseAction?: string;
}

export interface WorkflowBooleanFieldConfig<Field extends string = string> {
  label: string;
  field: Field;
  visible?: boolean;
}

export interface WorkflowSelectOption {
  value: string;
  label: string;
}

export interface WorkflowSelectFieldConfig<Field extends string = string> {
  label: string;
  field: Field;
  visible?: boolean;
}

export interface WorkflowActionButtonConfig<Action extends string = string> {
  label: string;
  action: Action;
  visible?: boolean;
}

type WorkflowTextBooleanDraft<
  TextField extends string,
  BooleanField extends string,
> = Record<TextField, string> & Record<BooleanField, boolean>;

export interface WorkflowTextBooleanActionStore<
  TextField extends string,
  BooleanField extends string,
  BrowseField extends string,
> {
  draft(): WorkflowTextBooleanDraft<TextField, BooleanField>;
  updateTextField(field: TextField, value: string): void;
  updateBooleanField(field: BooleanField, value: boolean): void;
  browseField(field: BrowseField): void;
}

export abstract class WorkflowTextBooleanActionBindings<
  TextField extends string,
  BooleanField extends string,
  BrowseField extends string,
> {
  protected abstract readonly store: WorkflowTextBooleanActionStore<
    TextField,
    BooleanField,
    BrowseField
  >;

  protected readonly readTextField = (field: string): string =>
    this.store.draft()[field as TextField];
  protected readonly writeTextField = (field: string, value: string): void => {
    this.store.updateTextField(field as TextField, value);
  };
  protected readonly readBooleanField = (field: string): boolean =>
    this.store.draft()[field as BooleanField];
  protected readonly writeBooleanField = (field: string, value: boolean): void => {
    this.store.updateBooleanField(field as BooleanField, value);
  };
  protected readonly browseField = (field: string): void => {
    this.store.browseField(field as BrowseField);
  };
}

@Component({
  selector: "app-workflow-text-field-list",
  standalone: true,
  imports: [FormsModule],
  template: `
    @for (field of fields; track field.field) {
      @if (field.visible !== false) {
        <label class="field" [class.field-span]="field.wide">
          <span>{{ field.label }}</span>
          <div [class.field-group]="field.browseAction !== undefined">
            <input
              type="text"
              [attr.inputmode]="field.inputmode ?? null"
              [ngModel]="readValue(field.field)"
              (ngModelChange)="writeValue(field.field, $event)"
            />
            @if (field.browseAction !== undefined) {
              <button
                class="field-browse"
                type="button"
                (click)="browse(field.browseAction)"
              >
                Browse
              </button>
            }
          </div>
        </label>
      }
    }
  `,
})
export class WorkflowTextFieldListComponent {
  @Input({ required: true })
  fields: ReadonlyArray<WorkflowTextFieldConfig<string>> = [];

  @Input({ required: true })
  readValue!: (field: string) => string;

  @Input({ required: true })
  writeValue!: (field: string, value: string) => void;

  @Input()
  browse: (action: string) => void = () => {};
}

@Component({
  selector: "app-workflow-boolean-field-list",
  standalone: true,
  imports: [FormsModule],
  template: `
    @for (field of fields; track field.field) {
      @if (field.visible !== false) {
        <label class="field field-toggle">
          <span>{{ field.label }}</span>
          <input
            type="checkbox"
            [ngModel]="readValue(field.field)"
            (ngModelChange)="writeValue(field.field, $event)"
          />
        </label>
      }
    }
  `,
})
export class WorkflowBooleanFieldListComponent {
  @Input({ required: true })
  fields: ReadonlyArray<WorkflowBooleanFieldConfig<string>> = [];

  @Input({ required: true })
  readValue!: (field: string) => boolean;

  @Input({ required: true })
  writeValue!: (field: string, value: boolean) => void;
}

@Component({
  selector: "app-workflow-select-field-list",
  standalone: true,
  imports: [FormsModule],
  template: `
    @for (field of fields; track field.field) {
      @if (field.visible !== false) {
        <label class="field">
          <span>{{ field.label }}</span>
          <select
            [ngModel]="readValue(field.field)"
            (ngModelChange)="writeValue(field.field, $event)"
          >
            @for (option of readOptions(field.field); track option.value) {
              <option [value]="option.value">{{ option.label }}</option>
            }
          </select>
        </label>
      }
    }
  `,
})
export class WorkflowSelectFieldListComponent {
  @Input({ required: true })
  fields: ReadonlyArray<WorkflowSelectFieldConfig<string>> = [];

  @Input({ required: true })
  readValue!: (field: string) => string;

  @Input({ required: true })
  writeValue!: (field: string, value: string) => void;

  @Input({ required: true })
  readOptions!: (field: string) => ReadonlyArray<WorkflowSelectOption>;
}

@Component({
  selector: "app-workflow-action-button-row",
  standalone: true,
  template: `
    <div class="action-row">
      @for (action of actions; track action.action) {
        @if (action.visible !== false) {
          <button type="button" (click)="triggerAction(action.action)">
            {{ action.label }}
          </button>
        }
      }
    </div>
  `,
})
export class WorkflowActionButtonRowComponent {
  @Input({ required: true })
  actions: ReadonlyArray<WorkflowActionButtonConfig<string>> = [];

  @Input({ required: true })
  triggerAction!: (action: string) => void;
}
