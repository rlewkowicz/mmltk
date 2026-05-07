import { ChangeDetectionStrategy, Component, inject } from "@angular/core";
import { FormsModule } from "@angular/forms";

import { BrowserShellChromeState } from "./state/browser-shell-chrome.service";
import { BrowserShellSettingsState } from "./state/browser-shell-settings.service";

@Component({
  selector: "app-settings-modal",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [FormsModule],
  template: `
    @if (chrome.settingsOpen()) {
      <section class="modal-backdrop" role="presentation" (click)="closeSettings()">
        <dialog class="settings-modal" open aria-labelledby="settings-title" (click)="$event.stopPropagation()">
          <div class="section-header settings-modal__header">
            <button type="button" class="button-compact" (click)="closeSettings()">Close</button>
            <h2 id="settings-title">Settings</h2>
          </div>

          <section class="settings-modal__section">
            <h3>Appearance</h3>
            <div class="form-grid">
              <label class="field field-inline">
                <span>Dark Mode</span>
                <input
                  type="checkbox"
                  [ngModel]="settings.draft().darkMode"
                  (ngModelChange)="settings.updateDarkMode($event)"
                />
              </label>
            </div>
          </section>

          <section class="settings-modal__section">
            <h3>Typography</h3>
            <div class="form-grid">
              <label class="field">
                <span>UI Scale</span>
                <input
                  type="text"
                  inputmode="decimal"
                  [ngModel]="settings.draft().uiScale"
                  (ngModelChange)="settings.updateTextField('uiScale', $event)"
                />
              </label>
              <label class="field">
                <span>Primary Font</span>
                <input
                  type="text"
                  inputmode="decimal"
                  [ngModel]="settings.draft().fontSize"
                  (ngModelChange)="settings.updateTextField('fontSize', $event)"
                />
              </label>
              <label class="field">
                <span>Secondary Font</span>
                <input
                  type="text"
                  inputmode="decimal"
                  [ngModel]="settings.draft().secondaryFontSize"
                  (ngModelChange)="settings.updateTextField('secondaryFontSize', $event)"
                />
              </label>
              <label class="field">
                <span>Mono Font</span>
                <input
                  type="text"
                  inputmode="decimal"
                  [ngModel]="settings.draft().monoFontSize"
                  (ngModelChange)="settings.updateTextField('monoFontSize', $event)"
                />
              </label>
            </div>
          </section>

          <section class="settings-modal__section">
            <h3>Layout</h3>
            <div class="form-grid">
              <label class="field">
                <span>Density</span>
                <select
                  [ngModel]="settings.draft().density"
                  (ngModelChange)="settings.updateDensity($event)"
                >
                  @for (option of settings.shellDensityOptions; track option.value) {
                    <option [value]="option.value">{{ option.label }}</option>
                  }
                </select>
              </label>
              <label class="field">
                <span>Label Width</span>
                <input
                  type="text"
                  inputmode="numeric"
                  [ngModel]="settings.draft().propertyLabelWidth"
                  (ngModelChange)="settings.updateTextField('propertyLabelWidth', $event)"
                />
              </label>
            </div>
          </section>

          <section class="settings-modal__section">
            <h3>Crop Interaction</h3>
            <div class="form-grid">
              <label class="field">
                <span>Edge Hit Width</span>
                <input
                  type="text"
                  inputmode="decimal"
                  [ngModel]="settings.draft().cropEdgeHitHalfWidth"
                  (ngModelChange)="settings.updateTextField('cropEdgeHitHalfWidth', $event)"
                />
              </label>
              <label class="field">
                <span>Corner Hit Size</span>
                <input
                  type="text"
                  inputmode="decimal"
                  [ngModel]="settings.draft().cropCornerHitSize"
                  (ngModelChange)="settings.updateTextField('cropCornerHitSize', $event)"
                />
              </label>
              <label class="field">
                <span>Handle Radius</span>
                <input
                  type="text"
                  inputmode="decimal"
                  [ngModel]="settings.draft().cropHandleRadius"
                  (ngModelChange)="settings.updateTextField('cropHandleRadius', $event)"
                />
              </label>
            </div>
          </section>

          <div class="action-row">
            <button type="button" (click)="settings.reset()">Reset UI</button>
            <button type="button" (click)="closeSettings()">Close</button>
          </div>
        </dialog>
      </section>
    }
  `,
})
export class SettingsModalComponent {
  protected readonly chrome = inject(BrowserShellChromeState);
  protected readonly settings = inject(BrowserShellSettingsState);

  protected closeSettings(): void {
    this.settings.apply();
    this.chrome.closeSettings();
  }
}
