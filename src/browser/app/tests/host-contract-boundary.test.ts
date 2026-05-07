import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "node:test";

import { BrowserShellChromeState } from "../src/app/state/browser-shell-chrome.service";
import { BrowserShellStore } from "../src/app/state/browser-shell.store";
import type {
  BrowserHostNativeBridge,
  BrowserHostTransport,
  IntentMessage,
  StateSnapshot,
  Workflow,
} from "../src/host_api";
import {
  HOST_API_CONTRACT_HASH,
  HOST_API_INTENTS,
  HOST_API_PROTOCOL_VERSION,
} from "../src/host_api.generated";
import { createIntent, resolveBrowserHostTransport } from "../src/transport";
import {
  HOST_API_INTENT_WORKFLOWS,
  HOST_API_SCHEMA_BUNDLE,
  WORKFLOW_CONTRACT_HASH,
  hostApiIntentPayloadSchema,
  type HostApiJsonSchema,
} from "../src/workflow_contract.generated";
import { createMockBrowserHostTransport } from "./mock_transport";
import {
  assertHostApiIntent,
  validateHostApiSchema,
} from "./support/host-api-contract-validator";
import {
  createAngularServiceHarness,
} from "./support/angular-test-harness";
import {
  installBrowserTestWindow,
  makeBrowserShellStoreProviders,
  type BrowserTestWindow,
} from "./support/browser-test-fixtures";

type JsonRecord = Record<string, unknown>;
type TestWindow = BrowserTestWindow;

function isRecord(value: unknown): value is JsonRecord {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function schemaRecord(value: unknown): HostApiJsonSchema {
  return isRecord(value) ? (value as HostApiJsonSchema) : {};
}

function schemaArray(value: unknown): readonly unknown[] {
  return Array.isArray(value) ? value : [];
}

function localRefSchema(ref: string): HostApiJsonSchema {
  assert.ok(ref.startsWith("#/components/schemas/"));
  const schemaName = ref.slice("#/components/schemas/".length);
  const schemas = HOST_API_SCHEMA_BUNDLE.schemas as Readonly<
    Record<string, HostApiJsonSchema>
  >;
  const schema = schemas[schemaName];
  assert.ok(schema, `missing generated schema ${schemaName}`);
  return schema;
}

function resolveSchema(schema: HostApiJsonSchema): HostApiJsonSchema {
  return typeof schema.$ref === "string"
    ? resolveSchema(localRefSchema(schema.$ref))
    : schema;
}

function schemaType(schema: HostApiJsonSchema): string | undefined {
  if (typeof schema.type === "string") {
    return schema.type;
  }
  if (Array.isArray(schema.type)) {
    return schema.type.find(
      (entry): entry is string => typeof entry === "string" && entry !== "null",
    );
  }
  if (isRecord(schema.properties)) {
    return "object";
  }
  if (isRecord(schema.items)) {
    return "array";
  }
  return undefined;
}

function sampleValueForSchema(schemaInput: HostApiJsonSchema): unknown {
  const schema = resolveSchema(schemaInput);
  const oneOf = schemaArray(schema.oneOf);
  if (oneOf.length > 0) {
    const preferred = oneOf.find((candidate) => {
      const candidateSchema = resolveSchema(schemaRecord(candidate));
      return candidateSchema.type !== "null";
    });
    return sampleValueForSchema(schemaRecord(preferred ?? oneOf[0]));
  }

  const enumValues = schemaArray(schema.enum);
  if (enumValues.length > 0) {
    return enumValues[0];
  }

  switch (schemaType(schema)) {
    case "object": {
      const out: JsonRecord = {};
      const properties = isRecord(schema.properties) ? schema.properties : {};
      for (const required of schemaArray(schema.required)) {
        if (typeof required === "string") {
          out[required] = sampleValueForSchema(schemaRecord(properties[required]));
        }
      }
      return out;
    }
    case "array":
      return [sampleValueForSchema(schemaRecord(schema.items))];
    case "string":
      return "sample";
    case "integer":
    case "number":
      return 1;
    case "boolean":
      return true;
    case "null":
      return null;
    default:
      return {};
  }
}

function samplePayloadForIntent(intent: string): JsonRecord {
  const schema = hostApiIntentPayloadSchema(intent);
  assert.ok(schema, `missing generated payload schema for ${intent}`);
  const payload = sampleValueForSchema(schema);
  assert.ok(isRecord(payload), `payload schema for ${intent} must be an object`);
  const errors = validateHostApiSchema(schema, payload);
  assert.deepEqual(errors, []);
  return payload;
}

function assertSnapshotMatchesContract(snapshot: StateSnapshot): void {
  assert.deepEqual(
    validateHostApiSchema(
      { $ref: "#/components/schemas/StateSnapshot" },
      snapshot,
      "state.snapshot",
    ),
    [],
  );
}

function record(value: unknown): JsonRecord {
  assert.ok(isRecord(value));
  return value;
}

function createShellUiHarness(transport: BrowserHostTransport): {
  chrome: BrowserShellChromeState;
  destroy: () => void;
  flush: () => void;
  store: BrowserShellStore;
} {
  const harness = createAngularServiceHarness(
    transport.getSnapshot(),
    makeBrowserShellStoreProviders(),
    { mode: transport.mode, transport },
  );
  const store = harness.run(() => new BrowserShellStore());
  const chrome = harness.run(() => harness.injector.get(BrowserShellChromeState));
  harness.flush();
  return {
    chrome,
    store,
    destroy: harness.destroy,
    flush: harness.flush,
  };
}

function installNativeHostSimulator(): {
  host: BrowserHostTransport;
  nativeBridge: BrowserHostNativeBridge;
  postedMessages: IntentMessage[];
  restoreWindow: () => void;
} {
  const host = createMockBrowserHostTransport();
  const postedMessages: IntentMessage[] = [];
  let nativeBridge: BrowserHostNativeBridge;
  nativeBridge = {
    postMessage(messageText: string): void {
      const parsed = JSON.parse(messageText) as IntentMessage;
      assertHostApiIntent(parsed);
      postedMessages.push(parsed);
      host.dispatch(parsed);
      nativeBridge.deliverSnapshot?.(JSON.stringify(host.getSnapshot()));
    },
  };

  return {
    host,
    nativeBridge,
    postedMessages,
    restoreWindow: installBrowserTestWindow({
      __MMLTK_NATIVE_BRIDGE__: nativeBridge,
    } as TestWindow),
  };
}

test("generated host contract schemas validate representative intent corpus", () => {
  assert.equal(HOST_API_CONTRACT_HASH, WORKFLOW_CONTRACT_HASH);
  const workflowMap = HOST_API_INTENT_WORKFLOWS as Readonly<
    Record<string, readonly Workflow[]>
  >;

  for (const intent of HOST_API_INTENTS) {
    const payload = samplePayloadForIntent(intent);
    const workflows = workflowMap[intent];
    assert.ok(workflows?.length, `intent ${intent} has no generated workflows`);

    for (const workflow of workflows) {
      assert.doesNotThrow(() => {
        assertHostApiIntent(createIntent(workflow, intent, payload));
      }, `${intent} should validate for ${workflow}`);
    }
  }
});

test("generated OpenAPI publication matches generated TypeScript contract", () => {
  const openApiPath = path.join(process.cwd(), "host_api.openapi.json");
  assert.ok(
    fs.existsSync(openApiPath),
    "generated OpenAPI publication must be staged before browser app tests",
  );
  const openApi = JSON.parse(fs.readFileSync(openApiPath, "utf8")) as JsonRecord;
  const paths = record(openApi.paths);
  const components = record(openApi.components);
  const schemas = record(components.schemas);
  const rawPublishedIntents = openApi["x-mmltk-intents"];
  assert.ok(Array.isArray(rawPublishedIntents));
  const publishedIntents = rawPublishedIntents as JsonRecord[];
  const workflowMap = HOST_API_INTENT_WORKFLOWS as Readonly<
    Record<string, readonly Workflow[]>
  >;

  assert.equal(openApi.openapi, "3.1.0");
  assert.equal(openApi["x-mmltk-contract-kind"], "browser-host");
  assert.equal(openApi["x-mmltk-protocol-version"], HOST_API_PROTOCOL_VERSION);
  assert.equal(openApi["x-mmltk-contract-hash"], HOST_API_CONTRACT_HASH);
  assert.deepEqual(Object.keys(schemas), Object.keys(HOST_API_SCHEMA_BUNDLE.schemas));

  const publishedIntentIds = publishedIntents.map((intent) => {
    assert.ok(isRecord(intent));
    assert.ok(typeof intent.id === "string");
    assert.ok(Array.isArray(intent.workflows));
    assert.ok(intent.payloadSchema === null || isRecord(intent.payloadSchema));
    return intent.id;
  });
  assert.deepEqual(publishedIntentIds, [...HOST_API_INTENTS]);

  for (const intentId of HOST_API_INTENTS) {
    const route = record(paths[`/intents/${intentId}`]);
    const post = record(route.post);
    assert.equal(post["x-mmltk-intent-name"], intentId);
    assert.deepEqual(post["x-mmltk-workflows"], workflowMap[intentId]);
    assert.ok(hostApiIntentPayloadSchema(intentId));
  }
});

test("host transport boundary exchanges generated snapshots and intents", () => {
  const transport = createMockBrowserHostTransport();
  const snapshots: StateSnapshot[] = [];
  const unsubscribe = transport.subscribe((snapshot) => {
    assertSnapshotMatchesContract(snapshot);
    snapshots.push(snapshot);
  });

  try {
    const dispatch = (
      workflow: Workflow,
      intent: string,
      payload: JsonRecord,
    ): void => {
      const message = createIntent(workflow, intent, payload);
      assertHostApiIntent(message);
      transport.dispatch(message);
    };

    dispatch("annotate", "settings.update", {
      patch: {
        workflows: {
          annotate: {
            source: {
              kind: 3,
            },
          },
        },
      },
    });
    dispatch("live", "live.preview.fit_to_capture", { enabled: true });
    dispatch("annotate", "file_dialog.request", {
      dialog_id: "annotate.source.image_folder",
      target_field: "source.imageDirectory",
      mode: "open_folder",
      title: "Choose annotation folder",
      filters: [],
    });

    const snapshot = transport.getSnapshot();
    assertSnapshotMatchesContract(snapshot);
    assert.equal(snapshot.source.kind, "video_stream");
    assert.equal(
      record(record(snapshot.workflow_state.live_preview_controls).fit_to_capture)
        .value,
      true,
    );
    assert.equal(snapshot.job.summary, "intent dispatched: file_dialog.request");
    assert.equal(snapshots.length, 4);
  } finally {
    unsubscribe();
  }
});

test("native bridge transport exchanges contract JSON with host simulator", () => {
  const { host, nativeBridge, postedMessages, restoreWindow } = installNativeHostSimulator();

  try {
    const transport = resolveBrowserHostTransport();
    assert.equal(transport.mode, "native");
    nativeBridge.deliverSnapshot?.(JSON.stringify(host.getSnapshot()));
    assertSnapshotMatchesContract(transport.getSnapshot());

    transport.dispatch(
      createIntent("live", "live.preview.full_frame_display", {
        enabled: true,
      }),
    );

    const snapshot = transport.getSnapshot();
    assertSnapshotMatchesContract(snapshot);
    assert.equal(postedMessages.length, 1);
    assert.equal(postedMessages[0]?.intent, "live.preview.full_frame_display");
    assert.equal(
      record(
        record(snapshot.workflow_state.live_preview_controls)
          .full_frame_display,
      ).value,
      true,
    );
    assert.equal(
      snapshot.job.summary,
      "live.preview.full_frame_display enabled",
    );
  } finally {
    restoreWindow();
  }
});

test("Angular shell UI contract harness runs against native host simulator", () => {
  const { host, nativeBridge, postedMessages, restoreWindow } = installNativeHostSimulator();

  try {
    const transport = resolveBrowserHostTransport();
    nativeBridge.deliverSnapshot?.(JSON.stringify(host.getSnapshot()));
    const ui = createShellUiHarness(transport);
    try {
      assert.equal(ui.store.footerWorkflow(), "workflow: annotate");
      assert.equal(ui.chrome.contractHashMatched(), true);

      ui.store.selectWorkflow("live");
      ui.flush();
      assert.equal(ui.store.footerWorkflow(), "workflow: live (host annotate)");
      assert.equal(postedMessages.at(-1)?.intent, "settings.update");
      assert.deepEqual(postedMessages.at(-1)?.payload, {
        patch: {
          current_view: "live",
        },
      });

      ui.store.activateWorkflow("annotate");
      ui.store.updateSourceKind("video_stream");
      ui.store.applySource();
      ui.flush();
      assert.equal(ui.store.snapshot().source.kind, "video_stream");
      assert.match(ui.store.jobFields()[2]?.value ?? "", /settings\.update/);

      ui.store.updateSourceKind("image_folder");
      const browseRequest = ui.store.sourceBrowseRequest();
      assert.ok(browseRequest);
      ui.store.browseSource();
      ui.flush();
      assert.equal(postedMessages.at(-1)?.intent, "file_dialog.request");
      assert.equal(
        record(postedMessages.at(-1)?.payload).dialog_id,
        browseRequest.dialogId,
      );

      ui.store.activateWorkflow("live");
      ui.flush();
      ui.store.setLivePreviewFitToCapture(true);
      ui.flush();
      assert.equal(postedMessages.at(-1)?.intent, "live.preview.fit_to_capture");
      assert.equal(
        record(record(ui.store.snapshot().workflow_state.live_preview_controls).fit_to_capture)
          .value,
        true,
      );

      ui.store.setWorkspaceCanvasSize(800, 450);
      ui.store.fitWorkspaceToCapture(true);
      ui.flush();
      assert.equal(postedMessages.at(-1)?.intent, "viewport.commit");

      nativeBridge.deliverSnapshot?.(
        JSON.stringify({
          ...host.getSnapshot(),
          contract_hash: "stale-contract",
          state_revision: host.getSnapshot().state_revision + 1,
        }),
      );
      ui.flush();
      assert.equal(ui.chrome.contractHashMatched(), false);
      assert.match(ui.chrome.contractMismatchDetail(), /stale-contract/);
    } finally {
      ui.destroy();
    }
  } finally {
    restoreWindow();
  }
});
