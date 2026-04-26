import { deepEqual, equal, ok } from "node:assert/strict";
import { test } from "node:test";

import type { StateSnapshot } from "../src/host_api";
import {
  annotateBrowseRequestForField,
  annotateDraftFromSnapshot,
  buildAnnotateSettingsPatch,
  buildExportSettingsPatch,
  buildPredictSettingsPatch,
  buildShellSettingsPatch,
  buildSourceSettingsPatch,
  buildTrainSettingsPatch,
  buildValidateSettingsPatch,
  defaultShellSettingsDraft,
  exportBrowseRequestForField,
  exportDraftFromSnapshot,
  parseIntegerListInput,
  predictBrowseRequestForField,
  predictDraftFromSnapshot,
  shellSettingsDraftFromSnapshot,
  sourceBrowseRequestForDraft,
  sourceDraftFromSnapshot,
  sourceKindOptions,
  trainBrowseRequestForField,
  trainDraftFromSnapshot,
  validateBrowseRequestForField,
  validateDraftFromSnapshot,
  type SourceDraft,
} from "../src/app/state/browser-shell.store.helpers";
import {
  makeBrowserTestAnnotation,
  makeBrowserTestSnapshot,
  makeBrowserTestSource,
} from "./support/browser-test-fixtures";

function makeSnapshot(): StateSnapshot {
  return makeBrowserTestSnapshot({
    state_revision: 7,
    active_workflow: "annotate",
    workflow_state: {
      selected_preset: "rf-detr-seg-medium",
      workflows: {
        predict: {
          source: {
            kind: 1,
            single_image_path: "/predict/frame-0007.png",
            recursive: false,
            device_index: 2,
            capture_width: 1920,
            capture_height: 1080,
            capture_fps: 30,
            v4l2_buffer_count: 3,
          },
        },
        annotate: {
          source: {
            kind: 2,
            image_directory: "/annotate/frames",
            recursive: true,
            device_index: 0,
            capture_width: 1280,
            capture_height: 720,
            capture_fps: 60,
            v4l2_buffer_count: 4,
          },
        },
      },
    },
    settings_state: {
      ui_scale: 1.0,
      density: 1,
    },
    source: makeBrowserTestSource({
      kind: "image_folder",
      locator: "/snapshot/source",
      recursive: true,
      device_index: 0,
    }),
    annotation: makeBrowserTestAnnotation({
      document_generation: 3,
      session_revision: 9,
      capture_width: 1280,
      capture_height: 720,
      instance_count: 2,
      selected_instance: 1,
    }),
  });
}

function makeSourceDraft(overrides: Partial<SourceDraft> = {}): SourceDraft {
  return {
    kind: "image_folder",
    locator: "/annotate/frames",
    recursive: true,
    deviceIndex: "0",
    captureWidth: "1280",
    captureHeight: "720",
    captureFps: "60",
    v4l2BufferCount: "4",
    ...overrides,
  };
}

test("sourceDraftFromSnapshot prefers workflow source records", () => {
  const annotateDraft = sourceDraftFromSnapshot(makeSnapshot(), "annotate");
  deepEqual(annotateDraft, makeSourceDraft());

  const liveDraft = sourceDraftFromSnapshot(makeSnapshot(), "live");
  equal(liveDraft.kind, "single_image");
  equal(liveDraft.locator, "/predict/frame-0007.png");
  equal(liveDraft.deviceIndex, "2");
});

test("buildSourceSettingsPatch targets predict state for live workflow", () => {
  const draft: SourceDraft = {
    kind: "single_image",
    locator: "/tmp/live-source.png",
    recursive: false,
    deviceIndex: "5",
    captureWidth: "2048",
    captureHeight: "1152",
    captureFps: "24",
    v4l2BufferCount: "6",
  };

  deepEqual(buildSourceSettingsPatch("live", draft, makeSnapshot()), {
    workflows: {
      predict: {
        source: {
          kind: 1,
          recursive: false,
          device_index: 5,
          capture_width: 2048,
          capture_height: 1152,
          capture_fps: 24,
          v4l2_buffer_count: 6,
          compiled_path: "",
          single_image_path: "/tmp/live-source.png",
          image_directory: "",
        },
      },
    },
  });
});

test("source browse requests only map supported host dialog ids", () => {
  const liveImageRequest = sourceBrowseRequestForDraft("live", makeSourceDraft({
    kind: "single_image",
    locator: "/tmp/live-source.png",
    recursive: false,
  }));
  equal(liveImageRequest?.dialogId, "predict.source.single_image");
  equal(liveImageRequest?.mode, "open_file");

  const annotateFolderRequest = sourceBrowseRequestForDraft(
    "annotate",
    makeSourceDraft(),
  );
  equal(annotateFolderRequest?.dialogId, "annotate.source.image_folder");
  equal(annotateFolderRequest?.mode, "open_folder");

  equal(
    sourceBrowseRequestForDraft("predict", {
      ...makeSourceDraft({
        kind: "compiled_dataset",
        locator: "/datasets/demo/compiled.mmltk",
        recursive: false,
      }),
    }),
    null,
  );
});

test("shell settings helpers normalize density and clamp ui scale", () => {
  deepEqual(shellSettingsDraftFromSnapshot(makeSnapshot()), defaultShellSettingsDraft());

  deepEqual(
    buildShellSettingsPatch(
      {
        darkMode: true,
        uiScale: "3.25",
        fontSize: "32.0",
        secondaryFontSize: "6.0",
        monoFontSize: "15.4",
        propertyLabelWidth: "300",
        cropEdgeHitHalfWidth: "2.0",
        cropCornerHitSize: "40.0",
        cropHandleRadius: "9.4",
        density: "comfortable",
      },
      makeSnapshot(),
    ),
    {
      ui: {
        dark_mode: true,
        ui_scale: 1.75,
        font_size: 28,
        secondary_font_size: 12,
        mono_font_size: 15.4,
        property_label_width: 260,
        crop_edge_hit_half_width: 4,
        crop_corner_hit_size: 32,
        crop_handle_radius: 9.4,
        density: 2,
      },
    },
  );
});

test("annotate source kinds preserve unsupported current state when needed", () => {
  const options = sourceKindOptions("annotate", "compiled_dataset");
  equal(options[0]?.value, "compiled_dataset");
  ok(options.some((option) => option.value === "video_stream"));
});

test("train workflow helpers build nested settings patches and browse requests", () => {
  const draft = trainDraftFromSnapshot(makeSnapshot());
  draft.selectedPreset = "rf-detr-seg-large";
  draft.trainCompiledPath = "/datasets/new/train.mmltk";
  draft.valCompiledPath = "/datasets/new/val.mmltk";
  draft.testCompiledPath = "/datasets/new/test.mmltk";
  draft.weightsPath = "/models/new/checkpoint.pt";
  draft.resumePath = "/models/new/checkpoint-last.pt";
  draft.outputDir = "/tmp/new-train-output";
  draft.executionTarget = "remote";
  draft.inputMode = "resume";
  draft.optimizer = "adamw";
  draft.cpuAffinity = "0-3";
  draft.batchSize = "8";
  draft.valBatchSize = "4";
  draft.epochs = "32";
  draft.workers = "12";
  draft.lanes = "3";
  draft.compileMode = "full";
  draft.progressBar = false;
  draft.localDeviceIds = "0, 2, 2";
  draft.remoteFamilies = {
    a100: true,
    b200: false,
    h100: true,
    h200: false,
    l_series: true,
  };
  draft.remoteContainerImage = "ghcr.io/mmltk/train:remote";
  draft.remoteLaunchTemplate = "vast launch --template angular";

  deepEqual(buildTrainSettingsPatch(draft, makeSnapshot()), {
    selected_preset: "rf-detr-seg-large",
    workflows: {
      train: {
        dataset_paths: {
          train_compiled_path: "/datasets/new/train.mmltk",
          val_compiled_path: "/datasets/new/val.mmltk",
          test_compiled_path: "/datasets/new/test.mmltk",
        },
        model_artifacts: {
          weights_path: "/models/new/checkpoint.pt",
        },
        execution: {
          cpu_affinity: "0-3",
          workers: 12,
          lanes: 3,
          progress_bar: false,
          compile_mode: 2,
        },
        training: {
          output_dir: "/tmp/new-train-output",
          resume_path: "/models/new/checkpoint-last.pt",
          input_mode: 1,
          execution_target: 1,
          batch_size: 8,
          val_batch_size: 4,
          epochs: 32,
          grad_accum_steps: 1,
          eval_max_dets: 500,
          print_freq: 100,
          prefetch_factor: 2,
          optimizer: 0,
          momentum: 0.95,
          lr: 0.0001,
          lr_encoder: 0.00015,
          weight_decay: 0.0001,
          lr_component_decay: 0.7,
          encoder_layer_decay: 0.8,
          warmup_epochs: 0,
          warmup_momentum: 0,
          lr_min_factor: 0,
          clip_max_norm: 0.1,
          lr_drop: 100,
          lr_scheduler: "step",
          amp: true,
          use_ema: false,
          freeze_encoder: false,
          local_device_ids: [0, 2],
          remote_family_enabled: [true, false, true, false, true],
          remote_container_image: "ghcr.io/mmltk/train:remote",
          remote_launch_template: "vast launch --template angular",
        },
      },
    },
  });

  equal(
    trainBrowseRequestForField("trainCompiledPath", draft).dialogId,
    "train.dataset.train_compiled_path",
  );
  equal(trainBrowseRequestForField("outputDir", draft).mode, "open_folder");
  deepEqual(parseIntegerListInput("0, 2, 2, -1, foo"), [0, 2]);
});

test("validate workflow helpers target validation sections and dialog ids", () => {
  const draft = validateDraftFromSnapshot(makeSnapshot());
  draft.compiledPath = "/datasets/validate/compiled.mmltk";
  draft.sourceDir = "/datasets/validate/images";
  draft.onnxPath = "/models/validate/model.onnx";
  draft.tensorrtPath = "/models/validate/model.engine";
  draft.saveEnginePath = "/tmp/validate.engine";
  draft.reportJsonPath = "/tmp/validate-report.json";
  draft.split = "test";
  draft.evalOrder = "tensorrt,onnx";
  draft.resolution = "1280";
  draft.limitImages = "24";
  draft.alignmentImages = "12";
  draft.evalMaxDets = "750";
  draft.batchSize = "3";
  draft.prefetchFactor = "5";
  draft.cpuAffinity = "4-7";
  draft.deviceId = "2";
  draft.workers = "6";
  draft.allowFp16 = false;
  draft.writeReportJson = false;
  draft.recompile = true;
  draft.profile = true;

  deepEqual(buildValidateSettingsPatch(draft, makeSnapshot()), {
    workflows: {
      validate: {
        dataset_paths: {
          compiled_path: "/datasets/validate/compiled.mmltk",
          source_dir: "/datasets/validate/images",
        },
        model_artifacts: {
          onnx_path: "/models/validate/model.onnx",
          tensorrt_path: "/models/validate/model.engine",
        },
        execution: {
          cpu_affinity: "4-7",
          device_id: 2,
          workers: 6,
          allow_fp16: false,
        },
        validation: {
          save_engine_path: "/tmp/validate.engine",
          report_json_path: "/tmp/validate-report.json",
          split: "test",
          eval_order: "tensorrt,onnx",
          resolution: 1280,
          limit_images: 24,
          alignment_images: 12,
          eval_max_dets: 750,
          batch_size: 3,
          prefetch_factor: 5,
          recompile: true,
          profile: true,
          write_report_json: false,
        },
      },
    },
  });

  equal(validateBrowseRequestForField("sourceDir", draft).dialogId, "validate.dataset.source_dir");
  equal(validateBrowseRequestForField("saveEnginePath", draft).mode, "save_file");
});

test("predict workflow helpers reuse predict settings for live", () => {
  const draft = predictDraftFromSnapshot(makeSnapshot(), "live");
  draft.selectedPreset = "rf-detr-live";
  draft.weightsPath = "/models/live/model.pt";
  draft.onnxPath = "/models/live/model.onnx";
  draft.tensorrtPath = "/models/live/model.engine";
  draft.outputPath = "/tmp/live-output.json";
  draft.backend = "trt";
  draft.modelInput = "tensorrt";
  draft.batchSize = "5";
  draft.maxDetsPerImage = "640";
  draft.liveSplitCount = "4";
  draft.cpuAffinity = "1,3";
  draft.deviceId = "1";
  draft.workers = "7";
  draft.lanes = "2";
  draft.threshold = "0.61";
  draft.allowFp16 = false;
  draft.progressBar = true;
  draft.compileMode = "full";

  deepEqual(buildPredictSettingsPatch("live", draft, makeSnapshot()), {
    selected_preset: "rf-detr-live",
    workflows: {
      predict: {
        model_artifacts: {
          weights_path: "/models/live/model.pt",
          onnx_path: "/models/live/model.onnx",
          tensorrt_path: "/models/live/model.engine",
        },
        execution: {
          cpu_affinity: "1,3",
          device_id: 1,
          workers: 7,
          lanes: 2,
          allow_fp16: false,
          progress_bar: true,
          compile_mode: 2,
        },
        predict: {
          output_path: "/tmp/live-output.json",
          backend: "trt",
          model_input: 2,
          batch_size: 5,
          max_dets_per_image: 640,
          live_split_count: 4,
          threshold: 0.61,
        },
      },
    },
  });

  equal(predictBrowseRequestForField("live", "outputPath", draft).dialogId, "predict.output_path");
});

test("annotate and export helpers build route-specific patches and browse ids", () => {
  const annotateDraft = annotateDraftFromSnapshot(makeSnapshot());
  annotateDraft.weightsPath = "/models/annotate/model.pt";
  annotateDraft.onnxPath = "/models/annotate/model.onnx";
  annotateDraft.tensorrtPath = "/models/annotate/model.engine";
  annotateDraft.outputDir = "/datasets/annotate/output";
  annotateDraft.split = "val";
  annotateDraft.backend = "onnx";
  annotateDraft.modelInput = "onnx";
  annotateDraft.maxDetsPerImage = "420";
  annotateDraft.deviceId = "3";
  annotateDraft.threshold = "0.42";
  annotateDraft.allowFp16 = false;
  annotateDraft.fullFrame = true;
  annotateDraft.compileMode = "none";

  deepEqual(buildAnnotateSettingsPatch(annotateDraft, makeSnapshot()), {
    workflows: {
      annotate: {
        model_artifacts: {
          weights_path: "/models/annotate/model.pt",
          onnx_path: "/models/annotate/model.onnx",
          tensorrt_path: "/models/annotate/model.engine",
        },
        execution: {
          device_id: 3,
          allow_fp16: false,
          compile_mode: 0,
        },
        annotate: {
          output_dir: "/datasets/annotate/output",
          split: "val",
          backend: "onnx",
          model_input: 1,
          max_dets_per_image: 420,
          threshold: 0.42,
          full_frame: true,
        },
      },
    },
  });
  equal(annotateBrowseRequestForField("outputDir", annotateDraft).dialogId, "annotate.output_dir");

  const exportDraft = exportDraftFromSnapshot(makeSnapshot());
  exportDraft.weightsPath = "/models/export/model.pt";
  exportDraft.onnxPath = "/models/export/model.onnx";
  exportDraft.outputPath = "/models/export/model.engine";
  exportDraft.deviceId = "4";
  exportDraft.opsetVersion = "21";
  exportDraft.allowFp16 = false;
  exportDraft.buildTensorRt = false;
  exportDraft.simplify = true;

  deepEqual(buildExportSettingsPatch(exportDraft, makeSnapshot()), {
    workflows: {
      export: {
        model_artifacts: {
          weights_path: "/models/export/model.pt",
          onnx_path: "/models/export/model.onnx",
        },
        execution: {
          device_id: 4,
          allow_fp16: false,
        },
        export: {
          output_path: "/models/export/model.engine",
          opset_version: 21,
          build_tensorrt: false,
          simplify: true,
        },
      },
    },
  });
  equal(exportBrowseRequestForField("outputPath", exportDraft).mode, "save_file");
});
