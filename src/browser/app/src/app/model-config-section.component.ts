import { ChangeDetectionStrategy, Component, computed, inject } from "@angular/core";
import { FormsModule } from "@angular/forms";

import { compactDisplayText, isRecord, numberFromValue, stringFromValue } from "../app_shared";
import { BrowserHostRuntimeState } from "./state/browser-host-runtime.service";
import { BrowserWorkflowRouteState } from "./state/browser-workflow-route.service";

interface ModelPresetOption {
  presetName: string;
  displayName: string;
}

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
          <select [ngModel]="selectedPreset()" (ngModelChange)="setPreset($event)">
            @for (preset of presets(); track preset.presetName) {
              <option [value]="preset.presetName">{{ preset.displayName }}</option>
            }
          </select>
        </label>
      </div>
      <dl class="detail-grid detail-grid--dense">
        <div>
          <dt>Module</dt>
          <dd>{{ moduleLabel() }}</dd>
        </div>
        <div>
          <dt>Resolution</dt>
          <dd>{{ resolution() }}</dd>
        </div>
        <div>
          <dt>Queries</dt>
          <dd>{{ queries() }}</dd>
        </div>
        <div>
          <dt>Max Dets</dt>
          <dd>{{ maxDets() }}</dd>
        </div>
        <div class="field-span">
          <dt>Canonical Weights</dt>
          <dd>{{ canonicalWeights() }}</dd>
        </div>
      </dl>
    </section>
  `,
})
export class ModelConfigSectionComponent {
  private readonly runtime = inject(BrowserHostRuntimeState);
  private readonly route = inject(BrowserWorkflowRouteState);

  protected readonly modelConfig = computed(() => {
    const root = this.runtime.snapshot().workflow_state;
    return isRecord(root.model_config) ? root.model_config : {};
  });
  protected readonly selectedPreset = computed(() =>
    stringFromValue(this.modelConfig().selected_preset, "rf-detr-seg-medium"),
  );
  protected readonly presets = computed<ModelPresetOption[]>(() => {
    const rawPresets = this.modelConfig().presets;
    if (!Array.isArray(rawPresets)) {
      return [
        {
          presetName: this.selectedPreset(),
          displayName: this.selectedPreset(),
        },
      ];
    }
    const presets = rawPresets.filter(isRecord).map((preset) => ({
      presetName: stringFromValue(preset.preset_name),
      displayName: stringFromValue(preset.display_name, stringFromValue(preset.preset_name)),
    })).filter((preset) => preset.presetName.length > 0);
    return presets.some((preset) => preset.presetName === this.selectedPreset())
      ? presets
      : [
          { presetName: this.selectedPreset(), displayName: this.selectedPreset() },
          ...presets,
        ];
  });
  protected readonly moduleLabel = computed(() =>
    compactDisplayText(this.modelConfig().module_label, "RF-DETR"),
  );
  protected readonly canonicalWeights = computed(() =>
    compactDisplayText(this.modelConfig().canonical_weight_filename, "-"),
  );
  protected readonly resolution = computed(() =>
    String(Math.round(numberFromValue(this.modelConfig().resolution, 0))),
  );
  protected readonly queries = computed(() =>
    String(Math.round(numberFromValue(this.modelConfig().num_queries, 0))),
  );
  protected readonly maxDets = computed(() =>
    String(Math.round(numberFromValue(this.modelConfig().max_dets, 0))),
  );

  protected workflowLabel(): string {
    return this.route.selectedWorkflow().toUpperCase();
  }

  protected setPreset(presetName: string): void {
    this.runtime.dispatch(this.route.selectedWorkflow(), "settings.update", {
      patch: { selected_preset: presetName },
    });
  }
}
