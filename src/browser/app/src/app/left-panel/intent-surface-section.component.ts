import { ChangeDetectionStrategy, Component, computed, inject } from '@angular/core';
import { BrowserAnnotateSidebarState } from '../state/browser-annotate-sidebar.service';
import { BrowserExportWorkflowState } from '../state/browser-export-workflow.service';
import { BrowserHostRuntimeState } from '../state/browser-host-runtime.service';
import { BrowserLiveWorkflowState } from '../state/browser-live-workflow.service';
import { BrowserPredictWorkflowState } from '../state/browser-predict-workflow.service';
import { BrowserTrainWorkflowState } from '../state/browser-train-workflow.service';
import { BrowserValidateWorkflowState } from '../state/browser-validate-workflow.service';
import { BrowserWorkflowPanelsState } from '../state/browser-workflow-panels.service';
import { BrowserWorkflowPrimaryActionState } from '../state/browser-workflow-primary-action.service';
import { BrowserWorkflowRouteState } from '../state/browser-workflow-route.service';
import { BrowserWorkspaceStateService } from '../state/browser-workspace.service';

@Component({
  selector: 'app-left-panel-intent-surface-section',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <section class="panel-section">
      <div class="section-header">
        <h2>Intent Surface</h2>
      </div>
      @if (isPredictLikeWorkflow()) {
        <div class="action-row">
          <button
            type="button"
            [attr.aria-pressed]="live.livePreviewControls().fitToCapture.value"
            [disabled]="!live.livePreviewControls().fitToCapture.enabled"
            (click)="toggleLiveFitToCapture()"
          >
            {{ liveFitToCaptureLabel() }}
          </button>
          <button
            type="button"
            [attr.aria-pressed]="live.livePreviewControls().fullFrameDisplay.value"
            [disabled]="!live.livePreviewControls().fullFrameDisplay.enabled"
            (click)="toggleLiveFullFrameDisplay()"
          >
            {{ liveFullFrameDisplayLabel() }}
          </button>
        </div>
        <div class="action-row">
          <button type="button" (click)="predict.browseField('weightsPath')">Weights</button>
          <button type="button" (click)="predict.browseField('onnxPath')">ONNX</button>
          <button
            type="button"
            (click)="predict.browseField(predictLikeIntentBrowseField())"
          >
            {{ predictLikeIntentBrowseLabel() }}
          </button>
        </div>
        <p class="section-copy">{{ live.livePreviewControlsSummary() }}</p>
      } @else {
        @switch (workflow.selectedWorkflow()) {
          @case ('annotate') {
            <div class="tool-selector">
              @for (tool of annotate.toolPalette().options; track tool.id) {
                <button
                  type="button"
                  [attr.data-tool-id]="tool.id"
                  [attr.data-active]="tool.active"
                  [disabled]="tool.disabled"
                  (click)="annotate.selectTool(tool.id)"
                  [title]="tool.description"
                >
                  {{ tool.label }}
                </button>
              }
            </div>
            <p class="section-copy">{{ annotate.toolPaletteSummary() }}</p>
          }
          @case ('train') {
            @if (train.draft().executionTarget === 'local') {
              <div class="action-row">
                <button
                  type="button"
                  [disabled]="!train.trainLocalGpuDraftState().refreshSupported"
                  (click)="train.refreshLocalGpuInventory()"
                >
                  {{ trainLocalGpuRefreshLabel() }}
                </button>
              </div>
              <p class="section-copy">{{ train.trainLocalGpuDraftState().summary }}</p>
              <p class="section-copy">{{ train.trainLocalGpuDetail() }}</p>
            } @else {
              <div class="action-row">
                <button type="button" (click)="train.queryRemoteOffers()">
                  Query Offers
                </button>
                <button
                  type="button"
                  [disabled]="train.trainRemoteQuery().armedOfferId === null"
                  (click)="train.clearRemoteOffer()"
                >
                  Clear Armed
                </button>
              </div>
              <p class="section-copy">{{ train.trainRemoteStatusSummary() }}</p>
            }
          }
          @case ('validate') {
            <div class="action-row">
              <button type="button" (click)="validate.browseField('compiledPath')">
                Dataset
              </button>
              <button type="button" (click)="validate.browseField('sourceDir')">
                Source Root
              </button>
              <button type="button" (click)="validate.browseField('reportJsonPath')">
                Report
              </button>
            </div>
            <p class="section-copy">{{ panels.workflowDetailsNote() }}</p>
          }
          @case ('export') {
            <div class="action-row">
              <button type="button" (click)="exportWorkflow.browseField('weightsPath')">
                Weights
              </button>
              <button type="button" (click)="exportWorkflow.browseField('onnxPath')">ONNX</button>
              <button type="button" (click)="exportWorkflow.browseField('outputPath')">
                Output
              </button>
            </div>
            <p class="section-copy">{{ panels.workflowDetailsNote() }}</p>
          }
        }
      }
      <div class="action-stack">
        <button id="run-primary-action" type="button" (click)="primary.runPrimaryAction()">
          {{ primary.primaryActionLabel() }}
        </button>
        <button type="button" [disabled]="!workspace.canCommitCrop()" (click)="workspace.commitWorkspaceCrop()">
          Commit Crop
        </button>
        <button type="button" [disabled]="!workspace.canFitWorkspace()" (click)="workspace.fitWorkspaceToCapture(true)">
          Fit Workspace
        </button>
        <button
          type="button"
          [disabled]="!workspace.canFocusSelection()"
          (click)="workspace.focusSelectedAnnotation(true)"
        >
          Focus Selection
        </button>
        <button type="button" [disabled]="!workspace.canFocusCrop()" (click)="workspace.focusActiveClip(true)">
          Focus Crop
        </button>
      </div>
      <dl class="detail-grid">
        @for (item of workspace.workspaceDiagnostics().statusItems; track item.key) {
          <div>
            <dt>{{ item.label }}</dt>
            <dd>{{ item.summary }}</dd>
          </div>
        }
      </dl>
      <p class="section-copy">{{ diagnosticsNarrative() }}</p>
    </section>
  `,
})
export class IntentSurfaceSectionComponent {
  protected readonly annotate = inject(BrowserAnnotateSidebarState);
  protected readonly runtime = inject(BrowserHostRuntimeState);
  protected readonly workspace = inject(BrowserWorkspaceStateService);
  protected readonly workflow = inject(BrowserWorkflowRouteState);
  protected readonly panels = inject(BrowserWorkflowPanelsState);
  protected readonly primary = inject(BrowserWorkflowPrimaryActionState);
  protected readonly predict = inject(BrowserPredictWorkflowState);
  protected readonly live = inject(BrowserLiveWorkflowState);
  protected readonly train = inject(BrowserTrainWorkflowState);
  protected readonly validate = inject(BrowserValidateWorkflowState);
  protected readonly exportWorkflow = inject(BrowserExportWorkflowState);
  protected readonly diagnosticsNarrative = computed(() => {
    const details = [
      this.runtime.runtimeCapabilityStatus().find((item) => item.key === 'workspace_surface_bridge')?.detail,
      this.runtime.transportStatus().statusItems.find((item) => item.key === 'workspace_surface_zero_copy')?.detail,
      this.workspace.workspaceViewportDetail(),
    ].filter((detail): detail is string => typeof detail === 'string' && detail.trim().length > 0);

    return details.filter((detail, index, items) => items.indexOf(detail) === index).join(' ');
  });

  protected isPredictLikeWorkflow(): boolean {
    return this.workflow.selectedWorkflow() === 'predict' || this.workflow.selectedWorkflow() === 'live';
  }

  protected predictLikeIntentBrowseField(): 'outputPath' | 'tensorrtPath' {
    return this.workflow.selectedWorkflow() === 'predict' ? 'outputPath' : 'tensorrtPath';
  }

  protected predictLikeIntentBrowseLabel(): string {
    return this.workflow.selectedWorkflow() === 'predict' ? 'Output' : 'TensorRT';
  }

  protected trainLocalGpuRefreshLabel(): string {
    return this.train.trainLocalGpuDraftState().refreshRunning ? 'Refreshing GPUs' : 'Refresh GPUs';
  }

  protected liveFitToCaptureLabel(): string {
    const control = this.live.livePreviewControls().fitToCapture;
    return control.value ? `Disable ${control.label}` : `Enable ${control.label}`;
  }

  protected liveFullFrameDisplayLabel(): string {
    const control = this.live.livePreviewControls().fullFrameDisplay;
    return control.value ? `Disable ${control.label}` : `Enable ${control.label}`;
  }

  protected toggleLiveFitToCapture(): void {
    const control = this.live.livePreviewControls().fitToCapture;
    if (!control.enabled) {
      return;
    }
    this.live.setPreviewFitToCapture(!control.value);
  }

  protected toggleLiveFullFrameDisplay(): void {
    const control = this.live.livePreviewControls().fullFrameDisplay;
    if (!control.enabled) {
      return;
    }
    this.live.setPreviewFullFrameDisplay(!control.value);
  }
}
