import { ChangeDetectionStrategy, Component, inject } from "@angular/core";
import { FormsModule } from "@angular/forms";

import { BrowserShellSettingsState } from "../state/browser-shell-settings.service";

@Component({
  selector: "app-right-panel-shell-settings-section",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [FormsModule],
  template: `
    <section class="panel-section">
      <div class="section-header">
        <h2>Shell Settings</h2>
      </div>
      <div class="form-grid">
        <label class="field field-inline">
          <span>Dark Mode</span>
          <input
            type="checkbox"
            [ngModel]="store.draft().darkMode"
            (ngModelChange)="store.updateDarkMode($event)"
          />
        </label>
        <label class="field">
          <span>UI Scale</span>
          <input
            type="text"
            inputmode="decimal"
            [ngModel]="store.draft().uiScale"
            (ngModelChange)="store.updateUiScale($event)"
          />
        </label>
        <label class="field">
          <span>Density</span>
          <select
            [ngModel]="store.draft().density"
            (ngModelChange)="store.updateDensity($event)"
          >
            @for (option of store.shellDensityOptions; track option.value) {
              <option [value]="option.value">{{ option.label }}</option>
            }
          </select>
        </label>
      </div>
    </section>
  `,
})
export class RightPanelShellSettingsSectionComponent {
  protected readonly store = inject(BrowserShellSettingsState);
}
