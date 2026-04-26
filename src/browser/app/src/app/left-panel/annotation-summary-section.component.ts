import { ChangeDetectionStrategy, Component, computed, inject } from '@angular/core';
import { BrowserHostRuntimeState } from '../state/browser-host-runtime.service';

@Component({
  selector: 'app-left-panel-annotation-summary-section',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <section class="panel-section">
      <h2>Annotation</h2>
      <dl class="detail-grid">
        @for (entry of details(); track entry.label) {
          <div>
            <dt>{{ entry.label }}</dt>
            <dd>{{ entry.value }}</dd>
          </div>
        }
      </dl>
    </section>
  `,
})
export class AnnotationSummarySectionComponent {
  private readonly runtime = inject(BrowserHostRuntimeState);

  protected readonly details = computed(() => {
    const annotation = this.runtime.snapshot().annotation;
    return [
      { label: 'Document', value: String(Math.round(annotation.document_generation)) },
      { label: 'Session', value: String(Math.round(annotation.session_revision)) },
      { label: 'Instances', value: String(Math.round(annotation.instance_count)) },
      {
        label: 'Selected',
        value:
          annotation.selected_instance === null
            ? 'none'
            : String(Math.round(annotation.selected_instance)),
      },
    ];
  });
}
