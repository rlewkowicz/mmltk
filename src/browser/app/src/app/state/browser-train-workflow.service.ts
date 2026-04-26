import { computed, effect, inject, Injectable, signal, untracked } from "@angular/core";

import {
  booleanFromValue,
  capabilitySummaryText,
  combinedCapabilityStatus,
  compactDisplayText,
  integerDisplayText,
  isRecord,
  labeledValueField,
  numberFromValue,
  stringFromValue,
  type LabeledValueField,
  type CapabilityStatusViewState,
  workflowSectionRecord,
} from "../../app_shared";
import { trainLocalGpuState } from "../../browser_shell_state";
import type { IntentMessage, StateSnapshot } from "../../host_api";
import { BrowserHostRuntimeState } from "./browser-host-runtime.service";
import {
  buildTrainSettingsPatch,
  compileModeOptions as compileModeOptionsList,
  draftWithField,
  fileDialogRequestPayload,
  hasSelectOptionValue,
  parseIntegerListInput,
  trainBrowseRequestForField,
  trainDraftEquals,
  trainDraftFromSnapshot,
  trainInputModeOptions as trainInputModeOptionsList,
  trainOptimizerOptions as trainOptimizerOptionsList,
  trainRemoteFamilyOptions as trainRemoteFamilyOptionsList,
  trainTargetOptions as trainTargetOptionsList,
  type TrainBrowseField,
  type TrainDraft,
} from "./browser-shell.store.helpers";

export type DraftFieldVm = LabeledValueField;

export interface TrainRemoteOfferVm {
  offerId: number;
  family: string;
  gpuName: string;
  numGpus: number;
  dollarsPerHour: string;
  dlperfPerDollar: string;
  location: string;
  armed: boolean;
  summary: string;
}

export interface TrainRemoteQueryVm {
  apiKeyConfigured: boolean;
  running: boolean;
  lastSummary: string;
  lastError: string;
  armedOfferId: number | null;
  armedOfferSummary: string;
  offers: TrainRemoteOfferVm[];
}

function trainLocalGpuDetailText(
  state: ReturnType<typeof trainLocalGpuState>,
): string {
  if (state.error.length > 0) {
    return state.error;
  }
  if (state.warning.length > 0) {
    return state.warning;
  }
  return capabilitySummaryText(
    state.statusItems,
    state.hasEnumeratedDevices ? state.summary : "local GPU inventory pending",
  );
}

function trainRemoteStatusSummaryText(query: TrainRemoteQueryVm): string {
  if (query.lastError.length > 0) {
    return query.lastError;
  }
  if (query.armedOfferSummary.length > 0) {
    return query.armedOfferSummary;
  }
  if (query.lastSummary.length > 0) {
    return query.lastSummary;
  }
  return "Remote offer query and arming are ready.";
}

@Injectable({ providedIn: "root" })
export class BrowserTrainWorkflowState {
  private readonly runtime = inject(BrowserHostRuntimeState);

  readonly draft = signal<TrainDraft>(trainDraftFromSnapshot(this.runtime.snapshot()));
  readonly dirty = signal(false);
  readonly trainTargetOptions = trainTargetOptionsList();
  readonly trainInputModeOptions = trainInputModeOptionsList();
  readonly trainOptimizerOptions = trainOptimizerOptionsList();
  readonly trainRemoteFamilyOptions = trainRemoteFamilyOptionsList();
  readonly compileModeOptions = compileModeOptionsList();
  readonly trainLocalGpuDraftState = computed(() =>
    trainLocalGpuState(
      this.runtime.snapshot(),
      parseIntegerListInput(this.draft().localDeviceIds),
    ),
  );
  readonly trainRemoteQuery = computed(() =>
    this.buildTrainRemoteQueryState(this.runtime.snapshot()),
  );
  readonly trainLocalGpuDetail = computed(() =>
    trainLocalGpuDetailText(this.trainLocalGpuDraftState()),
  );
  readonly trainRemoteStatusSummary = computed(() =>
    trainRemoteStatusSummaryText(this.trainRemoteQuery()),
  );
  readonly trainRemoteSessionState = computed(() => {
    const remoteSession = workflowSectionRecord(
      this.runtime.snapshot(),
      "train",
      "remote_session",
    );
    return compactDisplayText(
      remoteSession?.current_state ?? remoteSession?.actual_status,
      "idle",
    );
  });
  readonly controlsStatus = computed<CapabilityStatusViewState>(() =>
    this.draft().executionTarget === "remote"
      ? {
          key: "workflow",
          label: "Train",
          status: this.trainRemoteQuery().running ? "active" : "ready",
          summary: "remote train controls ready",
          detail: this.trainRemoteStatusSummary(),
        }
      : {
          key: "workflow",
          label: "Train",
          status: combinedCapabilityStatus(
            this.trainLocalGpuDraftState().statusItems,
            "ready",
          ),
          summary: "local train controls ready",
          detail: this.trainLocalGpuDetail(),
        },
  );
  readonly detailsStatus = computed<CapabilityStatusViewState>(() =>
    this.draft().executionTarget === "remote"
      ? {
          key: "remote_session",
          label: "Remote Session",
          status: this.trainRemoteQuery().running
            ? "active"
            : this.trainRemoteSessionState() === "idle"
              ? "pending"
              : "ready",
          summary: `remote session ${this.trainRemoteSessionState()}`,
          detail: this.trainRemoteStatusSummary(),
        }
      : {
          key: "local_gpu",
          label: "Local GPU",
          status: combinedCapabilityStatus(
            this.trainLocalGpuDraftState().statusItems,
            "pending",
          ),
          summary: this.trainLocalGpuDraftState().summary,
          detail: this.trainLocalGpuDetail(),
        },
  );
  readonly trainRuntimeNote = computed(() => {
    if (this.draft().executionTarget !== "remote") {
      return this.trainLocalGpuDetail();
    }
    const sessionState = this.trainRemoteSessionState();
    return sessionState === "idle" ? "remote session idle" : sessionState;
  });
  readonly trainRuntimeFields = computed<DraftFieldVm[]>(() => {
    const snapshot = this.runtime.snapshot();
    const root = snapshot.workflow_state;
    const trainRuntime =
      isRecord(root) && isRecord(root.train_runtime) ? root.train_runtime : null;
    const remoteSession = workflowSectionRecord(snapshot, "train", "remote_session");
    if (this.draft().executionTarget === "remote") {
      const sshHost = compactDisplayText(remoteSession?.ssh_host);
      const sshPort =
        remoteSession?.ssh_port === null
          ? "-"
          : integerDisplayText(remoteSession?.ssh_port);
      return [
        labeledValueField(
          "State",
          compactDisplayText(
            remoteSession?.current_state ?? remoteSession?.actual_status,
            "idle",
          ),
        ),
        labeledValueField("Instance", integerDisplayText(remoteSession?.instance_id)),
        labeledValueField("Offer", integerDisplayText(remoteSession?.offer_id)),
        labeledValueField("GPU", compactDisplayText(remoteSession?.gpu_name), true),
        labeledValueField("SSH", `${sshHost}:${sshPort}`, true),
        labeledValueField("Ports", compactDisplayText(remoteSession?.ports), true),
      ];
    }
    return [
      labeledValueField(
        "Runtime",
        booleanFromValue(trainRuntime?.running, false) ? "running" : "idle",
      ),
      labeledValueField("Label", compactDisplayText(trainRuntime?.label, "train.local")),
      labeledValueField("Summary", compactDisplayText(trainRuntime?.summary), true),
      labeledValueField(
        "Output Dir",
        compactDisplayText(
          trainRuntime?.output_dir ??
            workflowSectionRecord(snapshot, "train", "training")?.output_dir,
        ),
        true,
      ),
      labeledValueField("Local GPUs", this.trainLocalGpuDraftState().summary, true),
      labeledValueField(
        "Refresh",
        this.trainLocalGpuDraftState().refreshRunning ? "refreshing" : "ready",
      ),
    ];
  });

  constructor() {
    effect(() => {
      this.runtime.snapshot();
      untracked(() => {
        this.sync();
      });
    });
  }

  patch(updater: (current: TrainDraft) => TrainDraft): void {
    const nextDraft = updater(this.draft());
    this.draft.set(nextDraft);
    this.dirty.set(
      !trainDraftEquals(nextDraft, trainDraftFromSnapshot(this.runtime.snapshot())),
    );
  }

  sync(force = false): void {
    const nextDraft = trainDraftFromSnapshot(this.runtime.snapshot());
    if (force || !this.dirty()) {
      this.draft.set(nextDraft);
      this.dirty.set(false);
      return;
    }
    this.dirty.set(!trainDraftEquals(this.draft(), nextDraft));
  }

  updateTextField(
    field:
      | "selectedPreset"
      | "trainCompiledPath"
      | "valCompiledPath"
      | "testCompiledPath"
      | "weightsPath"
      | "resumePath"
      | "outputDir"
      | "cpuAffinity"
      | "batchSize"
      | "valBatchSize"
      | "epochs"
      | "gradAccumSteps"
      | "evalMaxDets"
      | "printFreq"
      | "prefetchFactor"
      | "workers"
      | "lanes"
      | "momentum"
      | "learningRate"
      | "lrEncoder"
      | "weightDecay"
      | "lrComponentDecay"
      | "encoderLayerDecay"
      | "warmupEpochs"
      | "warmupMomentum"
      | "lrMinFactor"
      | "clipMaxNorm"
      | "lrDrop"
      | "lrScheduler"
      | "localDeviceIds"
      | "remoteContainerImage"
      | "remoteLaunchTemplate",
    value: string,
  ): void {
    this.patch((current) => draftWithField(current, field, value));
  }

  updateExecutionTarget(target: string): void {
    if (!hasSelectOptionValue(this.trainTargetOptions, target)) {
      return;
    }
    this.patch((current) => draftWithField(current, "executionTarget", target));
  }

  updateInputMode(inputMode: string): void {
    if (!hasSelectOptionValue(this.trainInputModeOptions, inputMode)) {
      return;
    }
    this.patch((current) => draftWithField(current, "inputMode", inputMode));
  }

  updateOptimizer(optimizer: string): void {
    if (!hasSelectOptionValue(this.trainOptimizerOptions, optimizer)) {
      return;
    }
    this.patch((current) => draftWithField(current, "optimizer", optimizer));
  }

  updateCompileMode(mode: string): void {
    if (!hasSelectOptionValue(this.compileModeOptions, mode)) {
      return;
    }
    this.patch((current) => draftWithField(current, "compileMode", mode));
  }

  updateProgressBar(progressBar: boolean): void {
    this.patch((current) => draftWithField(current, "progressBar", progressBar));
  }

  updateBooleanField(
    field: "amp" | "ema" | "freezeEncoder" | "progressBar",
    value: boolean,
  ): void {
    this.patch((current) => draftWithField(current, field, value));
  }

  setLocalGpuDeviceSelected(deviceId: number, selected: boolean): void {
    const currentIds = parseIntegerListInput(this.draft().localDeviceIds);
    const next = selected
      ? [...currentIds, deviceId]
          .filter((entry, index, values) => values.indexOf(entry) === index)
          .sort((left, right) => left - right)
      : currentIds.filter((entry) => entry !== deviceId);
    this.patch((current) => draftWithField(current, "localDeviceIds", next.join(", ")));
  }

  setRemoteFamilyEnabled(
    family: keyof TrainDraft["remoteFamilies"],
    enabled: boolean,
  ): void {
    this.patch((current) => ({
      ...current,
      remoteFamilies: {
        ...current.remoteFamilies,
        [family]: enabled,
      },
    }));
  }

  apply(): IntentMessage | null {
    if (!this.dirty()) {
      return null;
    }
    const message = this.runtime.dispatch("train", "settings.update", {
      patch: buildTrainSettingsPatch(this.draft(), this.runtime.snapshot()),
    });
    this.dirty.set(false);
    return message;
  }

  browseField(field: TrainBrowseField): IntentMessage {
    this.apply();
    const request = trainBrowseRequestForField(field, this.draft());
    return this.runtime.dispatch(
      "train",
      "file_dialog.request",
      fileDialogRequestPayload(request),
    );
  }

  refreshLocalGpuInventory(): IntentMessage | null {
    const gpuState = this.trainLocalGpuDraftState();
    if (!gpuState.refreshSupported || gpuState.refreshIntent === null) {
      return null;
    }
    return this.runtime.dispatch(gpuState.refreshWorkflow, gpuState.refreshIntent, {});
  }

  queryRemoteOffers(): IntentMessage | null {
    if (this.draft().executionTarget !== "remote") {
      return null;
    }
    return this.runtime.dispatch("train", "train.remote.query", {});
  }

  armRemoteOffer(offerId: number): IntentMessage | null {
    if (this.draft().executionTarget !== "remote") {
      return null;
    }
    return this.runtime.dispatch("train", "train.remote.offer.arm", { offer_id: offerId });
  }

  clearRemoteOffer(): IntentMessage | null {
    if (this.draft().executionTarget !== "remote") {
      return null;
    }
    return this.runtime.dispatch("train", "train.remote.offer.clear", {});
  }

  runPrimaryAction(): void {
    const snapshot = this.runtime.snapshot();
    if (this.draft().executionTarget === "remote") {
      const remoteSession = workflowSectionRecord(snapshot, "train", "remote_session");
      const running = booleanFromValue(remoteSession?.instance_running, false);
      this.runtime.dispatch(
        "train",
        running ? "train.remote.stop" : "train.remote.start",
        {},
      );
      return;
    }
    const root = snapshot.workflow_state;
    const runtime = isRecord(root) && isRecord(root.train_runtime) ? root.train_runtime : null;
    const running = booleanFromValue(runtime?.running, false);
    this.runtime.dispatch("train", running ? "train.stop" : "train.start", {});
  }

  private buildTrainRemoteQueryState(snapshot: StateSnapshot): TrainRemoteQueryVm {
    const record = workflowSectionRecord(snapshot, "train", "remote_query");
    const armedOfferIdRaw = record?.armed_offer_id;
    const armedOfferId =
      armedOfferIdRaw === null || armedOfferIdRaw === undefined
        ? null
        : Math.max(0, Math.round(numberFromValue(armedOfferIdRaw, 0)));
    const offers = Array.isArray(record?.results)
      ? record.results
          .filter(isRecord)
          .map((offer): TrainRemoteOfferVm => {
            const offerId = Math.max(0, Math.round(numberFromValue(offer.offer_id, 0)));
            const family = compactDisplayText(offer.family, "unknown");
            const gpuName = compactDisplayText(offer.gpu_name, "Unnamed GPU");
            const numGpus = Math.max(0, Math.round(numberFromValue(offer.num_gpus, 0)));
            const dollarsPerHour = numberFromValue(offer.dph, 0).toFixed(2);
            const dlperfPerDollar = numberFromValue(offer.dlperf_usd, 0).toFixed(2);
            const location = compactDisplayText(offer.geolocation, "-");
            return {
              offerId,
              family,
              gpuName,
              numGpus,
              dollarsPerHour,
              dlperfPerDollar,
              location,
              armed: armedOfferId === offerId,
              summary: `${family} · ${gpuName} · ${numGpus} GPUs · DLPerf/$ ${dlperfPerDollar} · $${dollarsPerHour}/hr`,
            };
          })
      : [];
    return {
      apiKeyConfigured: booleanFromValue(record?.api_key_configured, false),
      running: booleanFromValue(record?.running, false),
      lastSummary: compactDisplayText(record?.last_summary, ""),
      lastError: compactDisplayText(record?.last_error, ""),
      armedOfferId,
      armedOfferSummary: compactDisplayText(record?.armed_offer_summary, ""),
      offers,
    };
  }
}
