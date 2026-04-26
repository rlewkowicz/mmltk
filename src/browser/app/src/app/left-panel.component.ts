import { ChangeDetectionStrategy, Component } from '@angular/core';
import { AnnotationSummarySectionComponent } from './left-panel/annotation-summary-section.component';
import { IntentSurfaceSectionComponent } from './left-panel/intent-surface-section.component';
import { SourceSectionComponent } from './left-panel/source-section.component';
import { WorkflowControlsSectionComponent } from './left-panel/workflow-controls-section.component';

@Component({
  selector: 'app-left-panel',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [
    AnnotationSummarySectionComponent,
    IntentSurfaceSectionComponent,
    SourceSectionComponent,
    WorkflowControlsSectionComponent,
  ],
  template: `
    <aside class="panel panel-left">
      <app-left-panel-source-section />
      <app-left-panel-intent-surface-section />
      <app-left-panel-workflow-controls-section />
      <app-left-panel-annotation-summary-section />
    </aside>
  `,
})
export class LeftPanelComponent {}
