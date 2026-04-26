import { ChangeDetectionStrategy, Component, inject } from "@angular/core";
import { FormsModule } from "@angular/forms";

import { BrowserValidateWorkflowState } from "../state/browser-validate-workflow.service";

@Component({
  selector: "app-right-panel-workflow-validate-details",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [FormsModule],
  template: `
    <div class="form-grid">
      <label class="field field-span">
        <span>Save Engine Path</span>
        <input
          type="text"
          [ngModel]="store.draft().saveEnginePath"
          (ngModelChange)="store.updateTextField('saveEnginePath', $event)"
        />
      </label>
      <label class="field">
        <span>Split</span>
        <input
          type="text"
          [ngModel]="store.draft().split"
          (ngModelChange)="store.updateTextField('split', $event)"
        />
      </label>
      <label class="field field-span">
        <span>Eval Order</span>
        <input
          type="text"
          [ngModel]="store.draft().evalOrder"
          (ngModelChange)="store.updateTextField('evalOrder', $event)"
        />
      </label>
      <label class="field">
        <span>Resolution</span>
        <input
          type="text"
          inputmode="numeric"
          [ngModel]="store.draft().resolution"
          (ngModelChange)="store.updateTextField('resolution', $event)"
        />
      </label>
      <label class="field">
        <span>Limit Images</span>
        <input
          type="text"
          inputmode="numeric"
          [ngModel]="store.draft().limitImages"
          (ngModelChange)="store.updateTextField('limitImages', $event)"
        />
      </label>
      <label class="field">
        <span>Alignment Images</span>
        <input
          type="text"
          inputmode="numeric"
          [ngModel]="store.draft().alignmentImages"
          (ngModelChange)="store.updateTextField('alignmentImages', $event)"
        />
      </label>
      <label class="field">
        <span>Eval Max Dets</span>
        <input
          type="text"
          inputmode="numeric"
          [ngModel]="store.draft().evalMaxDets"
          (ngModelChange)="store.updateTextField('evalMaxDets', $event)"
        />
      </label>
      <label class="field">
        <span>Prefetch</span>
        <input
          type="text"
          inputmode="numeric"
          [ngModel]="store.draft().prefetchFactor"
          (ngModelChange)="store.updateTextField('prefetchFactor', $event)"
        />
      </label>
      <label class="field">
        <span>Recompile</span>
        <input
          type="checkbox"
          [ngModel]="store.draft().recompile"
          (ngModelChange)="store.updateBooleanField('recompile', $event)"
        />
      </label>
      <label class="field">
        <span>Profile</span>
        <input
          type="checkbox"
          [ngModel]="store.draft().profile"
          (ngModelChange)="store.updateBooleanField('profile', $event)"
        />
      </label>
    </div>
    <div class="action-row">
      <button type="button" (click)="store.browseField('saveEnginePath')">
        Save Engine
      </button>
    </div>
  `,
})
export class RightPanelWorkflowValidateDetailsComponent {
  protected readonly store = inject(BrowserValidateWorkflowState);
}
