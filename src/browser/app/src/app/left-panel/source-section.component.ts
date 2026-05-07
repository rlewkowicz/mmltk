import { ChangeDetectionStrategy, Component, inject } from '@angular/core';
import { FormsModule } from '@angular/forms';
import { BrowserSourceState } from '../state/browser-source-state.service';

@Component({
  selector: 'app-left-panel-source-section',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [FormsModule],
  template: `
    <section class="panel-section">
      <div class="section-header">
        <h2>Source</h2>
      </div>
      <dl class="detail-grid">
        @for (entry of store.sourceDetails(); track entry.label) {
          <div>
            <dt>{{ entry.label }}</dt>
            <dd>{{ entry.value }}</dd>
          </div>
        }
      </dl>
      @if (store.sourceEditingSupported()) {
        <div class="form-grid">
          <label class="field">
            <span>Source Kind</span>
            <select
              [ngModel]="store.draft().kind"
              (ngModelChange)="updateSourceKind($event)"
            >
              @for (option of store.sourceKindOptions(); track option.value) {
                <option [value]="option.value">{{ option.label }}</option>
              }
            </select>
          </label>
          <label class="field" [class.field-span]="store.canEditSourceLocator()">
            <span>{{ sourceLocatorLabel() }}</span>
            <div class="field-group">
              <input
                type="text"
                [ngModel]="store.draft().locator"
                (ngModelChange)="updateSourceLocator($event)"
                [disabled]="!store.canEditSourceLocator()"
              />
              <button
                class="field-browse"
                type="button"
                [disabled]="!store.canBrowseSource()"
                (click)="store.browse()"
              >
                Browse
              </button>
            </div>
          </label>
          <label class="field">
            <span>Recursive</span>
            <input
              type="checkbox"
              [ngModel]="store.draft().recursive"
              (ngModelChange)="updateSourceRecursive($event)"
              [disabled]="!store.canEditSourceRecursive()"
            />
          </label>
          <label class="field">
            <span>Device</span>
            <input
              type="text"
              inputmode="numeric"
              [ngModel]="store.draft().deviceIndex"
              (ngModelChange)="updateSourceDeviceIndex($event)"
            />
          </label>
          <label class="field">
            <span>Capture Width</span>
            <input
              type="text"
              inputmode="numeric"
              [ngModel]="store.draft().captureWidth"
              (ngModelChange)="updateSourceCaptureWidth($event)"
            />
          </label>
          <label class="field">
            <span>Capture Height</span>
            <input
              type="text"
              inputmode="numeric"
              [ngModel]="store.draft().captureHeight"
              (ngModelChange)="updateSourceCaptureHeight($event)"
            />
          </label>
          <label class="field">
            <span>Capture FPS</span>
            <input
              type="text"
              inputmode="numeric"
              [ngModel]="store.draft().captureFps"
              (ngModelChange)="updateSourceCaptureFps($event)"
            />
          </label>
          <label class="field">
            <span>V4L2 Buffers</span>
            <input
              type="text"
              inputmode="numeric"
              [ngModel]="store.draft().v4l2BufferCount"
              (ngModelChange)="updateSourceV4l2BufferCount($event)"
            />
          </label>
        </div>
      } @else {
        <div class="form-grid">
          @for (entry of store.sourceControls(); track entry.label) {
            <label class="field" [class.field-span]="entry.wide === true">
              <span>{{ entry.label }}</span>
              <input type="text" [value]="entry.value" readonly />
            </label>
          }
        </div>
      }
    </section>
  `,
})
export class SourceSectionComponent {
  protected readonly store = inject(BrowserSourceState);

  protected sourceLocatorLabel(): string {
    switch (this.store.draft().kind) {
      case 'compiled_dataset':
        return 'Compiled Dataset';
      case 'single_image':
        return 'Single Image';
      case 'image_folder':
        return 'Image Folder';
      default:
        return 'Device Path';
    }
  }

  protected updateSourceKind(kind: string): void {
    const allowedKinds = this.store.sourceKindOptions().map((option) => option.value);
    if (!allowedKinds.includes(kind as (typeof allowedKinds)[number])) {
      return;
    }
    this.store.patch((current) => ({
      ...current,
      kind: kind as typeof current.kind,
      locator:
        kind === 'video_stream'
          ? `device:${current.deviceIndex.trim().length > 0 ? current.deviceIndex.trim() : '0'}`
          : current.locator,
      recursive: kind === 'image_folder' ? current.recursive : false,
    }));
  }

  protected updateSourceDeviceIndex(deviceIndex: string): void {
    this.store.patch((current) => ({
      ...current,
      deviceIndex,
      locator:
        current.kind === 'video_stream'
          ? `device:${deviceIndex.trim().length > 0 ? deviceIndex.trim() : '0'}`
          : current.locator,
    }));
  }

  protected updateSourceLocator(locator: string): void {
    this.store.patch((current) => ({
      ...current,
      locator,
    }));
  }

  protected updateSourceRecursive(recursive: boolean): void {
    this.store.patch((current) => ({
      ...current,
      recursive: current.kind === 'image_folder' ? recursive : false,
    }));
  }

  protected updateSourceCaptureWidth(captureWidth: string): void {
    this.store.patch((current) => ({
      ...current,
      captureWidth,
    }));
  }

  protected updateSourceCaptureHeight(captureHeight: string): void {
    this.store.patch((current) => ({
      ...current,
      captureHeight,
    }));
  }

  protected updateSourceCaptureFps(captureFps: string): void {
    this.store.patch((current) => ({
      ...current,
      captureFps,
    }));
  }

  protected updateSourceV4l2BufferCount(v4l2BufferCount: string): void {
    this.store.patch((current) => ({
      ...current,
      v4l2BufferCount,
    }));
  }
}
