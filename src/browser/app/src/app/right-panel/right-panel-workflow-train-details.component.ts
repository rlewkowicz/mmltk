import { ChangeDetectionStrategy, Component, inject } from "@angular/core";
import { FormsModule } from "@angular/forms";

import { BrowserTrainWorkflowState } from "../state/browser-train-workflow.service";

type TrainRemoteActionKind = "query" | "clear";

@Component({
  selector: "app-right-panel-workflow-train-details",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [FormsModule],
  template: `
    <div class="form-grid">
      @if (store.draft().executionTarget === "local") {
        <label class="field field-span">
          <span>Local Device IDs</span>
          <input
            type="text"
            [ngModel]="store.draft().localDeviceIds"
            (ngModelChange)="store.updateTextField('localDeviceIds', $event)"
          />
        </label>
      } @else {
        <label class="field field-span">
          <span>Remote Container</span>
          <input
            type="text"
            [ngModel]="store.draft().remoteContainerImage"
            (ngModelChange)="store.updateTextField('remoteContainerImage', $event)"
          />
        </label>
        <label class="field field-span">
          <span>Remote Template</span>
          <input
            type="text"
            [ngModel]="store.draft().remoteLaunchTemplate"
            (ngModelChange)="store.updateTextField('remoteLaunchTemplate', $event)"
          />
        </label>
      }
    </div>
    <div class="action-row">
      @if (store.draft().executionTarget === "local") {
        <button type="button" (click)="store.refreshLocalGpuInventory()">
          Refresh GPUs
        </button>
      } @else {
        @for (action of trainRemoteActions(); track action.label) {
          <button
            type="button"
            [disabled]="action.disabled"
            (click)="runTrainRemoteAction(action.kind)"
          >
            {{ action.label }}
          </button>
        }
      }
    </div>
    @if (store.draft().executionTarget === "local") {
      @if (store.trainLocalGpuDraftState().visibleDevices.length > 0) {
        <div class="choice-grid">
          @for (
            device of store.trainLocalGpuDraftState().visibleDevices;
            track device.deviceId
          ) {
            <label class="choice-chip">
              <input
                type="checkbox"
                [ngModel]="device.selected"
                (ngModelChange)="
                  store.setLocalGpuDeviceSelected(device.deviceId, $event)
                "
              />
              <span>{{ device.label }}</span>
            </label>
          }
        </div>
      }
      <p class="section-copy">
        {{
          store.trainLocalGpuDraftState().error ||
            store.trainLocalGpuDraftState().warning ||
            store.trainLocalGpuDraftState().summary
        }}
      </p>
    } @else {
      <div class="choice-grid">
        @for (family of store.trainRemoteFamilyOptions; track family.value) {
          <label class="choice-chip">
            <input
              type="checkbox"
              [ngModel]="store.draft().remoteFamilies[family.value]"
              (ngModelChange)="
                store.setRemoteFamilyEnabled(family.value, $event)
              "
            />
            <span>{{ family.label }}</span>
          </label>
        }
      </div>
      @if (!store.trainRemoteQuery().apiKeyConfigured) {
        <p class="section-copy">
          Remote queries require --vast-api-key or VAST_API_KEY in the native
          runtime.
        </p>
      }
      @if (store.trainRemoteQuery().offers.length > 0) {
        <div class="slot-list">
          @for (offer of store.trainRemoteQuery().offers; track offer.offerId) {
            <button
              type="button"
              class="slot-card"
              [attr.data-selected]="offer.armed"
              (click)="store.armRemoteOffer(offer.offerId)"
            >
              <strong>#{{ offer.offerId }} · {{ offer.family }}</strong>
              <p class="section-copy">{{ offer.summary }}</p>
              <p class="section-copy">{{ offer.location }}</p>
            </button>
          }
        </div>
      }
      <p class="section-copy">
        {{
          store.trainRemoteQuery().lastError ||
            store.trainRemoteQuery().armedOfferSummary ||
            store.trainRemoteQuery().lastSummary ||
            "Remote offers have not been queried yet."
        }}
      </p>
    }
  `,
})
export class RightPanelWorkflowTrainDetailsComponent {
  protected readonly store = inject(BrowserTrainWorkflowState);

  protected trainRemoteActions(): ReadonlyArray<{
    kind: TrainRemoteActionKind;
    label: string;
    disabled: boolean;
  }> {
    return [
      {
        kind: "query",
        label: "Query Offers",
        disabled: false,
      },
      {
        kind: "clear",
        label: "Clear Armed",
        disabled: this.store.trainRemoteQuery().armedOfferId === null,
      },
    ];
  }

  protected runTrainRemoteAction(kind: TrainRemoteActionKind): void {
    if (kind === "query") {
      this.store.queryRemoteOffers();
      return;
    }
    this.store.clearRemoteOffer();
  }
}
