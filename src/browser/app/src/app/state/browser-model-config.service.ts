import { computed, effect, inject, Injectable, signal, untracked } from "@angular/core";

import {
  compactDisplayText,
  isRecord,
  numberFromValue,
  stringFromValue,
} from "../../app_shared";
import {
  RF_DETR_PRESET_OPTIONS,
  type RfDetrPresetContract,
} from "../../workflow_contract.generated";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import { BrowserWorkflowRouteState } from "./browser-workflow-route.service";

@Injectable({ providedIn: "root" })
export class BrowserModelConfigState {
  private readonly runtime = inject(BrowserHostRuntimeState);
  private readonly route = inject(BrowserWorkflowRouteState);
  private readonly localSelectedPreset = signal<string>(RF_DETR_PRESET_OPTIONS[6].presetName);

  readonly modelConfig = computed(() => {
    const root = this.runtime.snapshot().workflow_state;
    return isRecord(root.model_config) ? root.model_config : {};
  });
  readonly selectedPreset = computed(() => this.localSelectedPreset());
  readonly presets = computed<ReadonlyArray<RfDetrPresetContract>>(() => {
    const rawPresets = this.modelConfig().presets;
    const backendPresets = Array.isArray(rawPresets)
      ? rawPresets
          .filter(isRecord)
          .map((preset) => ({
            presetName: stringFromValue(preset.preset_name),
            displayName: stringFromValue(
              preset.display_name,
              stringFromValue(preset.preset_name),
            ),
          }))
          .filter((preset) => preset.presetName.length > 0)
      : [];
    const merged = new Map<string, RfDetrPresetContract>();
    for (const preset of RF_DETR_PRESET_OPTIONS) {
      merged.set(preset.presetName, preset);
    }
    for (const preset of backendPresets) {
      merged.set(preset.presetName, preset);
    }
    if (!merged.has(this.selectedPreset())) {
      merged.set(this.selectedPreset(), {
        presetName: this.selectedPreset(),
        displayName: this.selectedPreset(),
      });
    }
    return Array.from(merged.values());
  });
  readonly moduleLabel = computed(() =>
    compactDisplayText(this.modelConfig().module_label, "RF-DETR"),
  );
  readonly canonicalWeights = computed(() =>
    compactDisplayText(this.modelConfig().canonical_weight_filename, "-"),
  );
  readonly resolution = computed(() =>
    String(Math.round(numberFromValue(this.modelConfig().resolution, 0))),
  );
  readonly queries = computed(() =>
    String(Math.round(numberFromValue(this.modelConfig().num_queries, 0))),
  );
  readonly maxDets = computed(() =>
    String(Math.round(numberFromValue(this.modelConfig().max_dets, 0))),
  );

  constructor() {
    effect(() => {
      const selected = stringFromValue(
        this.modelConfig().selected_preset,
        stringFromValue(this.runtime.snapshot().workflow_state.selected_preset),
      );
      if (selected.trim().length > 0) {
        untracked(() => {
          this.localSelectedPreset.set(selected);
        });
      }
    });
  }

  setPreset(presetName: string): void {
    if (!this.presets().some((preset) => preset.presetName === presetName)) {
      return;
    }
    this.localSelectedPreset.set(presetName);
    this.runtime.dispatch(this.route.selectedWorkflow(), "settings.update", {
      patch: { selected_preset: presetName },
    });
  }
}
