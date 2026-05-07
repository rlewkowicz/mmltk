import { ChangeDetectionStrategy, Component, inject, Input } from "@angular/core";
import { FormsModule } from "@angular/forms";

import { BrowserLiveWorkflowState } from "./state/browser-live-workflow.service";
import { BrowserPredictWorkflowState } from "./state/browser-predict-workflow.service";

@Component({
  selector: "app-predict-workflow-controls",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [FormsModule],
  template: `
    <div class="form-grid">
      <label class="field field-span">
        <span>Weights Path</span>
        <div class="field-group">
          <input
            type="text"
            [ngModel]="predict.draft().weightsPath"
            (ngModelChange)="predict.updateTextField('weightsPath', $event)"
          />
          <button class="field-browse" type="button" (click)="predict.browseField('weightsPath')">Browse</button>
        </div>
      </label>
      <label class="field field-span">
        <span>ONNX Path</span>
        <div class="field-group">
          <input
            type="text"
            [ngModel]="predict.draft().onnxPath"
            (ngModelChange)="predict.updateTextField('onnxPath', $event)"
          />
          <button class="field-browse" type="button" (click)="predict.browseField('onnxPath')">Browse</button>
        </div>
      </label>
      <label class="field field-span">
        <span>TensorRT Path</span>
        <div class="field-group">
          <input
            type="text"
            [ngModel]="predict.draft().tensorrtPath"
            (ngModelChange)="predict.updateTextField('tensorrtPath', $event)"
          />
          <button class="field-browse" type="button" (click)="predict.browseField('tensorrtPath')">Browse</button>
        </div>
      </label>
      @if (!liveMode) {
        <label class="field field-span">
          <span>Output Path</span>
          <div class="field-group">
            <input
              type="text"
              [ngModel]="predict.draft().outputPath"
              (ngModelChange)="predict.updateTextField('outputPath', $event)"
            />
            <button class="field-browse" type="button" (click)="predict.browseField('outputPath')">Browse</button>
          </div>
        </label>
      }
      <label class="field">
        <span>Backend</span>
        <input
          type="text"
          [ngModel]="predict.draft().backend"
          (ngModelChange)="predict.updateTextField('backend', $event)"
        />
      </label>
      <label class="field">
        <span>Model Input</span>
        <select
          [ngModel]="predict.draft().modelInput"
          (ngModelChange)="predict.updateModelInput($event)"
        >
          @for (option of predict.predictModelInputOptions(); track option.value) {
            <option [value]="option.value">{{ option.label }}</option>
          }
        </select>
      </label>
      @if (!liveMode) {
        <label class="field">
          <span>Batch Size</span>
          <input
            type="text"
            inputmode="numeric"
            [ngModel]="predict.draft().batchSize"
            (ngModelChange)="predict.updateTextField('batchSize', $event)"
          />
        </label>
      }
      <label class="field">
        <span>Device</span>
        <input
          type="text"
          inputmode="numeric"
          [ngModel]="predict.draft().deviceId"
          (ngModelChange)="predict.updateTextField('deviceId', $event)"
        />
      </label>
      @if (!liveMode) {
        <label class="field">
          <span>Workers</span>
          <input
            type="text"
            inputmode="numeric"
            [ngModel]="predict.draft().workers"
            (ngModelChange)="predict.updateTextField('workers', $event)"
          />
        </label>
        <label class="field">
          <span>Lanes</span>
          <input
            type="text"
            inputmode="numeric"
            [ngModel]="predict.draft().lanes"
            (ngModelChange)="predict.updateTextField('lanes', $event)"
          />
        </label>
      }
      <label class="field field-span">
        <span>CPU Affinity</span>
        <input
          type="text"
          [ngModel]="predict.draft().cpuAffinity"
          (ngModelChange)="predict.updateTextField('cpuAffinity', $event)"
        />
      </label>
      <label class="field">
        <span>Threshold</span>
        <input
          type="text"
          inputmode="decimal"
          [ngModel]="predict.draft().threshold"
          (ngModelChange)="predict.updateTextField('threshold', $event)"
        />
      </label>
      <label class="field">
        <span>Allow FP16</span>
        <input
          type="checkbox"
          [ngModel]="predict.draft().allowFp16"
          (ngModelChange)="predict.updateBooleanField('allowFp16', $event)"
        />
      </label>
      <label class="field">
        <span>Compile Mode</span>
        <select
          [ngModel]="predict.draft().compileMode"
          (ngModelChange)="predict.updateCompileMode($event)"
        >
          @for (option of predict.compileModeOptions; track option.value) {
            <option [value]="option.value">{{ option.label }}</option>
          }
        </select>
      </label>
    </div>
    @if (liveMode) {
      <div class="form-grid">
        <label class="field">
          <span>{{ live.livePreviewControls().fitToCapture.label }}</span>
          <input
            #liveFitToCapture
            type="checkbox"
            [checked]="live.livePreviewControls().fitToCapture.value"
            [disabled]="!live.livePreviewControls().fitToCapture.enabled"
            (change)="live.setPreviewFitToCapture(liveFitToCapture.checked)"
          />
        </label>
        <label class="field">
          <span>{{ live.livePreviewControls().fullFrameDisplay.label }}</span>
          <input
            #liveFullFrameDisplay
            type="checkbox"
            [checked]="live.livePreviewControls().fullFrameDisplay.value"
            [disabled]="!live.livePreviewControls().fullFrameDisplay.enabled"
            (change)="live.setPreviewFullFrameDisplay(liveFullFrameDisplay.checked)"
          />
        </label>
      </div>
      <p class="section-copy">{{ live.livePreviewControlsSummary() }}</p>
    }
  `,
})
export class PredictWorkflowControlsComponent {
  @Input({ required: true }) liveMode = false;

  protected readonly predict = inject(BrowserPredictWorkflowState);
  protected readonly live = inject(BrowserLiveWorkflowState);
}
