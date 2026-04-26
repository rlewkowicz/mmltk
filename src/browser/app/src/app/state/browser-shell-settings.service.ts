import { DOCUMENT } from "@angular/common";
import { computed, effect, inject, Injectable, signal, untracked } from "@angular/core";

import type { IntentMessage } from "../../host_api";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import { BrowserWorkflowRouteState } from "./browser-workflow-route.service";
import {
  buildShellSettingsPatch,
  defaultShellSettingsDraft,
  shellDensityOptions as shellDensityOptionsList,
  shellSettingsDraftEquals,
  shellSettingsDraftFromSnapshot,
  type ShellDensity,
  type ShellSettingsDraft,
} from "./browser-shell.store.helpers";

@Injectable({ providedIn: "root" })
export class BrowserShellSettingsState {
  private readonly runtime = inject(BrowserHostRuntimeState);
  private readonly workflowRoute = inject(BrowserWorkflowRouteState);
  private readonly document = inject(DOCUMENT, { optional: true });

  readonly draft = signal<ShellSettingsDraft>(
    shellSettingsDraftFromSnapshot(this.runtime.snapshot()),
  );
  readonly dirty = signal(false);
  readonly shellDensityOptions = shellDensityOptionsList();
  readonly canApplyShellSettings = computed(() => this.dirty());
  readonly shellSettingsNote = computed(() =>
    this.dirty() ? "shell settings draft staged" : "shell settings synchronized",
  );

  constructor() {
    effect(() => {
      this.runtime.snapshot();
      untracked(() => {
        this.sync();
      });
    });
    effect(() => {
      this.document?.documentElement.setAttribute(
        "data-theme",
        this.draft().darkMode ? "dark" : "light",
      );
    });
  }

  patch(updater: (current: ShellSettingsDraft) => ShellSettingsDraft): void {
    const nextDraft = updater(this.draft());
    this.draft.set(nextDraft);
    this.dirty.set(
      !shellSettingsDraftEquals(
        nextDraft,
        shellSettingsDraftFromSnapshot(this.runtime.snapshot()),
      ),
    );
  }

  sync(force = false): void {
    const nextDraft = shellSettingsDraftFromSnapshot(this.runtime.snapshot());
    if (force || !this.dirty()) {
      this.draft.set(nextDraft);
      this.dirty.set(false);
      return;
    }
    this.dirty.set(!shellSettingsDraftEquals(this.draft(), nextDraft));
  }

  updateUiScale(uiScale: string): void {
    this.patch((current) => ({
      ...current,
      uiScale,
    }));
  }

  updateTextField(
    field:
      | "uiScale"
      | "fontSize"
      | "secondaryFontSize"
      | "monoFontSize"
      | "propertyLabelWidth"
      | "cropEdgeHitHalfWidth"
      | "cropCornerHitSize"
      | "cropHandleRadius",
    value: string,
  ): void {
    this.patch((current) => ({
      ...current,
      [field]: value,
    }));
  }

  updateDensity(density: string): void {
    const allowedDensities = this.shellDensityOptions.map((option) => option.value);
    if (!allowedDensities.includes(density as ShellDensity)) {
      return;
    }
    this.patch((current) => ({
      ...current,
      density: density as ShellDensity,
    }));
  }

  updateDarkMode(darkMode: boolean): void {
    this.patch((current) => ({
      ...current,
      darkMode,
    }));
  }

  apply(): IntentMessage | null {
    if (!this.canApplyShellSettings()) {
      return null;
    }
    const message = this.runtime.dispatch(
      this.workflowRoute.selectedWorkflow(),
      "settings.update",
      {
        patch: buildShellSettingsPatch(this.draft(), this.runtime.snapshot()),
      },
    );
    this.dirty.set(false);
    return message;
  }

  reset(): void {
    this.patch(defaultShellSettingsDraft);
  }
}
