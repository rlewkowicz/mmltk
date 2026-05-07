import { ChangeDetectionStrategy, Component, inject, signal } from "@angular/core";

import { AnnotateWorkflowControlsComponent } from "./left-panel/annotate-workflow-controls.component";
import { AnnotationSummarySectionComponent } from "./left-panel/annotation-summary-section.component";
import { SourceSectionComponent } from "./left-panel/source-section.component";
import { ModelConfigSectionComponent } from "./model-config-section.component";
import { RightPanelAnnotateSidebarSectionComponent } from "./right-panel/right-panel-annotate-sidebar-section.component";
import { BrowserAnnotateWorkflowState } from "./state/browser-annotate-workflow.service";

type AnnotateUtilityTab = "setup" | "objects" | "mask" | "save";

@Component({
  selector: "app-annotate-utility-tabs",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [
    AnnotateWorkflowControlsComponent,
    AnnotationSummarySectionComponent,
    ModelConfigSectionComponent,
    RightPanelAnnotateSidebarSectionComponent,
    SourceSectionComponent,
  ],
  template: `
    <section class="panel-section annotate-tabs">
      <div class="section-header">
        <h2>Annotate</h2>
      </div>
      <div class="tab-row" role="tablist" aria-label="Annotate utilities">
        @for (tab of tabs; track tab.id) {
          <button
            type="button"
            role="tab"
            [attr.aria-selected]="activeTab() === tab.id"
            [attr.data-active]="activeTab() === tab.id"
            (click)="activeTab.set(tab.id)"
          >
            {{ tab.label }}
          </button>
        }
      </div>
    </section>

    <section class="panel-section annotate-quick-actions">
      <div class="section-header">
        <h2>Actions</h2>
      </div>
      <div class="action-row">
        <button
          type="button"
          [disabled]="!annotate.annotateLiveStartEnabled()"
          (click)="annotate.startLive()"
        >
          {{ annotate.annotateControls().liveAnnotate.start.label }}
        </button>
        <button
          type="button"
          [disabled]="!annotate.annotateControls().liveAnnotate.stop.enabled"
          (click)="annotate.stopLive()"
        >
          {{ annotate.annotateControls().liveAnnotate.stop.label }}
        </button>
        <button
          type="button"
          [disabled]="!annotate.annotateControls().save.saveNow.enabled"
          (click)="annotate.requestSaveNow()"
        >
          {{ annotate.annotateControls().save.saveNow.label }}
        </button>
      </div>
    </section>

    @switch (activeTab()) {
      @case ("setup") {
        <app-left-panel-source-section />
        <app-model-config-section />
        <section class="panel-section">
          <div class="section-header">
            <h2>Setup</h2>
          </div>
          <app-left-panel-annotate-workflow-controls />
        </section>
      }
      @case ("objects") {
        <app-left-panel-annotation-summary-section />
        <app-right-panel-annotate-sidebar-section />
      }
      @case ("mask") {
        <app-right-panel-annotate-sidebar-section />
      }
      @case ("save") {
        <section class="panel-section">
          <div class="section-header">
            <h2>Save</h2>
          </div>
          <app-left-panel-annotate-workflow-controls />
        </section>
      }
    }
  `,
})
export class AnnotateUtilityTabsComponent {
  protected readonly annotate = inject(BrowserAnnotateWorkflowState);
  protected readonly activeTab = signal<AnnotateUtilityTab>("setup");
  protected readonly tabs: ReadonlyArray<{
    id: AnnotateUtilityTab;
    label: string;
  }> = [
    { id: "setup", label: "Setup" },
    { id: "objects", label: "Objects" },
    { id: "mask", label: "Mask" },
    { id: "save", label: "Save" },
  ];
}
