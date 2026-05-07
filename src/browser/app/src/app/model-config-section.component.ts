import { ChangeDetectionStrategy, Component, inject } from "@angular/core";
import { FormsModule } from "@angular/forms";

import { BrowserModelConfigState } from "./state/browser-model-config.service";
import { BrowserWorkflowRouteState } from "./state/browser-workflow-route.service";

@Component({
  selector: "app-model-config-section",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [FormsModule],
  template: `
    <section class="panel-section panel-section--compact">
      <div class="section-header">
        <h2>Workflow</h2>
        <span class="status-copy">{{ workflowLabel() }}</span>
      </div>
      <div class="form-grid">
        <label class="field field-span">
          <span>Preset</span>
          <select [ngModel]="model.selectedPreset()" (ngModelChange)="setPreset($event)">
            @for (preset of model.presets(); track preset.presetName) {
              <option [value]="preset.presetName">{{ preset.displayName }}</option>
            }
          </select>
        </label>
      </div>
      <dl class="detail-grid detail-grid--dense">
        <div>
          <dt>Module</dt>
          <dd>{{ model.moduleLabel() }}</dd>
        </div>
        <div>
          <dt>Resolution</dt>
          <dd>{{ model.resolution() }}</dd>
        </div>
        <div>
          <dt>Queries</dt>
          <dd>{{ model.queries() }}</dd>
        </div>
        <div>
          <dt>Max Dets</dt>
          <dd>{{ model.maxDets() }}</dd>
        </div>
        <div class="field-span">
          <dt>Canonical Weights</dt>
          <dd>{{ model.canonicalWeights() }}</dd>
        </div>
      </dl>
    </section>
  `,
})
export class ModelConfigSectionComponent {
  protected readonly model = inject(BrowserModelConfigState);
  private readonly route = inject(BrowserWorkflowRouteState);

  protected workflowLabel(): string {
    return this.route.selectedWorkflow().toUpperCase();
  }

  protected setPreset(presetName: string): void {
    this.model.setPreset(presetName);
  }
}
