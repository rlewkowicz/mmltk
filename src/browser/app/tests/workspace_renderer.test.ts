import * as assert from "node:assert/strict";
import { test } from "node:test";

import { inject } from "@angular/core";

import { BrowserHostRuntimeState } from "../src/app/state/browser-host-runtime.service";
import { BrowserWorkspaceStateService } from "../src/app/state/browser-workspace.service";
import type { WorkspaceRenderInput } from "../src/workspace_renderer";
import type {
  BrowserHostWorkspaceGpuBridge,
  StateSnapshot,
  WorkspaceSurfaceInfo,
} from "../src/host_api";
import { WorkspaceWebGpuRenderer } from "../src/app/workspace/workspace-webgpu-renderer";
import type { WorkspaceRendererHostEvent } from "../src/app/workspace/workspace-webgpu-renderer";
import {
  buildWorkspaceCanvasLayout,
  buildWorkspaceRenderFrame,
  workspaceClipRectForInput,
  workspaceCanvasLayoutsEqual,
  workspaceOverlayBounds,
} from "../src/workspace_renderer";
import {
  createAngularServiceHarness,
  type AngularServiceHarness,
} from "./support/angular-test-harness";
import {
  makeBrowserWorkflowServiceProviders,
  makeBrowserTestSnapshot,
  makeBrowserTestSource,
  makeLivePredictRuntimeFixture,
} from "./support/browser-test-fixtures";
import { makeTestOverlayRectPrimitive } from "./support/workspace-render-fixtures";

class Deferred<T> {
  readonly promise: Promise<T>;
  resolve!: (value: T | PromiseLike<T>) => void;

  constructor() {
    this.promise = new Promise<T>((resolve) => {
      this.resolve = resolve;
    });
  }
}

interface FakeImportedTexture extends GPUTexture {
  readonly revision: string;
  readonly destroyed: boolean;
}

class FakeTexture implements FakeImportedTexture {
  readonly view: GPUTextureView = {};
  destroyed = false;

  constructor(readonly revision: string) {}

  createView(): GPUTextureView {
    return this.view;
  }

  destroy(): void {
    this.destroyed = true;
  }
}

function makeInput(): WorkspaceRenderInput {
  return {
    workflow: "annotate",
    nativePresentedLive: false,
    canvasWidth: 1280,
    canvasHeight: 720,
    captureWidth: 1920,
    captureHeight: 1080,
    frameX: 100,
    frameY: 40,
    frameWidth: 960,
    frameHeight: 540,
    clip: {
      x: 44,
      y: 48,
      width: 512,
      height: 144,
    },
    annotationCount: 2,
    overlayPrimitives: [],
  };
}

function makeWorkspaceSurface(revision: string): WorkspaceSurfaceInfo {
  return {
    surfaceId: "workspace",
    revision,
    width: 1920,
    height: 1080,
    textureFormat: "rgba8unorm",
    opaque: true,
    upright: true,
  };
}

function installFakeWebGpuBridge(
  currentSurface: { readonly value: WorkspaceSurfaceInfo | null },
  options: {
    acquireError?: Error;
    importError?: Error;
    releaseResult?: ReturnType<BrowserHostWorkspaceGpuBridge["releaseSurface"]>;
    releaseRendererTextureResult?: ReturnType<
      BrowserHostWorkspaceGpuBridge["releaseRendererTexture"]
    >;
    onRelease?: (revision: string) => void;
    onRendererTextureRelease?: (revision: string) => void;
  } = {},
): {
  readonly imports: FakeTexture[];
  readonly releases: string[];
  readonly rendererTextureReleases: string[];
  restore(): void;
} {
  const previousBridge = globalThis.__MMLTK_WORKSPACE_GPU_BRIDGE__;
  const imports: FakeTexture[] = [];
  const releases: string[] = [];
  const rendererTextureReleases: string[] = [];
  const bridge: BrowserHostWorkspaceGpuBridge = {
    acquireCurrentSurface(surfaceId: string): WorkspaceSurfaceInfo | null {
      if (options.acquireError !== undefined) {
        throw options.acquireError;
      }
      return surfaceId === "workspace" ? currentSurface.value : null;
    },
    importTexture(_device: GPUDevice, surfaceId: string, revision: string): GPUTexture {
      assert.equal(surfaceId, "workspace");
      if (options.importError !== undefined) {
        throw options.importError;
      }
      const texture = new FakeTexture(revision);
      imports.push(texture);
      return texture;
    },
    releaseSurface(
      surfaceId: string,
      revision: string,
    ): ReturnType<BrowserHostWorkspaceGpuBridge["releaseSurface"]> {
      assert.equal(surfaceId, "workspace");
      releases.push(revision);
      imports.find((texture) => texture.revision === revision)?.destroy();
      options.onRelease?.(revision);
      return options.releaseResult;
    },
    releaseRendererTexture(
      surfaceId: string,
      revision: string,
    ): ReturnType<BrowserHostWorkspaceGpuBridge["releaseRendererTexture"]> {
      assert.equal(surfaceId, "workspace");
      rendererTextureReleases.push(revision);
      imports.find((texture) => texture.revision === revision)?.destroy();
      options.onRendererTextureRelease?.(revision);
      return options.releaseRendererTextureResult;
    },
  };
  globalThis.__MMLTK_WORKSPACE_GPU_BRIDGE__ = bridge;

  return {
    imports,
    releases,
    rendererTextureReleases,
    restore(): void {
      globalThis.__MMLTK_WORKSPACE_GPU_BRIDGE__ = previousBridge;
    },
  };
}

function installFakeWebGpuRuntime(): {
  readonly submittedWork: Array<Deferred<undefined>>;
  readonly textureCopies: Array<{
    readonly sourceRevision: string;
    readonly destinationRevision: string;
    readonly width: number;
    readonly height: number;
  }>;
  readonly canvas: HTMLCanvasElement;
  emitUncapturedError(error: Error): void;
  loseDevice(info: GPUDeviceLostInfo): void;
  restore(): void;
} {
  const previousNavigator = Reflect.get(globalThis, "navigator");
  const submittedWork: Array<Deferred<undefined>> = [];
  const textureCopies: Array<{
    readonly sourceRevision: string;
    readonly destinationRevision: string;
    readonly width: number;
    readonly height: number;
  }> = [];
  let activeDeviceLoss: Deferred<GPUDeviceLostInfo> | null = null;
  let activeUncapturedErrorListener:
    | ((event: GPUUncapturedErrorEvent) => void)
    | null = null;
  const currentTexture = new FakeTexture("canvas");
  const passEncoder: GPURenderPassEncoder = {
    setPipeline(): void {},
    setBindGroup(): void {},
    setVertexBuffer(): void {},
    draw(): void {},
    end(): void {},
  };
  const makeDevice = (): GPUDevice => {
    const deviceLoss = new Deferred<GPUDeviceLostInfo>();
    activeDeviceLoss = deviceLoss;
    return {
      lost: deviceLoss.promise,
      addEventListener(
        type: "uncapturederror",
        listener: (event: GPUUncapturedErrorEvent) => void,
      ): void {
        if (type === "uncapturederror") {
          activeUncapturedErrorListener = listener;
        }
      },
      queue: {
        writeBuffer(): void {},
        writeTexture(): void {},
        copyExternalImageToTexture(): void {},
        submit(): void {},
        onSubmittedWorkDone(): Promise<undefined> {
          const deferred = new Deferred<undefined>();
          submittedWork.push(deferred);
          return deferred.promise;
        },
      },
      createShaderModule(): GPUShaderModule {
        return {
          getCompilationInfo: () => Promise.resolve({ messages: [] }),
        };
      },
      createRenderPipeline(): GPURenderPipeline {
        return {
          getBindGroupLayout: () => ({}),
        };
      },
      createBuffer(): GPUBuffer {
        return {
          destroy(): void {},
        };
      },
      createSampler(): GPUSampler {
        return {};
      },
      createTexture(): GPUTexture {
        return new FakeTexture("created");
      },
      createBindGroup(): GPUBindGroup {
        return {};
      },
      createCommandEncoder(): GPUCommandEncoder {
        return {
          beginRenderPass: () => passEncoder,
          copyTextureToTexture(source: unknown, destination: unknown, copySize: unknown): void {
            const sourceTexture = (source as { texture?: FakeTexture }).texture;
            const destinationTexture = (destination as { texture?: FakeTexture }).texture;
            const size = copySize as { width?: number; height?: number };
            textureCopies.push({
              sourceRevision: sourceTexture?.revision ?? "",
              destinationRevision: destinationTexture?.revision ?? "",
              width: size.width ?? 0,
              height: size.height ?? 0,
            });
          },
          finish: () => ({}),
        };
      },
    };
  };
  const context: GPUCanvasContext = {
    configure(): void {},
    unconfigure(): void {},
    getCurrentTexture(): GPUTexture {
      return currentTexture;
    },
  };
  const canvas = {
    width: 640,
    height: 360,
    getBoundingClientRect(): DOMRect {
      return {
        x: 0,
        y: 0,
        left: 0,
        top: 0,
        right: 640,
        bottom: 360,
        width: 640,
        height: 360,
        toJSON: () => ({}),
      } as DOMRect;
    },
    getContext(contextId: "webgpu"): GPUCanvasContext | null {
      return contextId === "webgpu" ? context : null;
    },
  } as HTMLCanvasElement;

  Object.defineProperty(globalThis, "navigator", {
    configurable: true,
    value: {
      gpu: {
        requestAdapter: () =>
          Promise.resolve({
            requestDevice: () => Promise.resolve(makeDevice()),
          }),
        getPreferredCanvasFormat: () => "rgba8unorm",
      },
    },
  });

  return {
    submittedWork,
    textureCopies,
    canvas,
    emitUncapturedError(error: Error): void {
      assert.notEqual(activeUncapturedErrorListener, null);
      activeUncapturedErrorListener!({ error } as unknown as GPUUncapturedErrorEvent);
    },
    loseDevice(info: GPUDeviceLostInfo): void {
      assert.notEqual(activeDeviceLoss, null);
      activeDeviceLoss!.resolve(info);
      activeDeviceLoss = null;
    },
    restore(): void {
      if (previousNavigator === undefined) {
        Reflect.deleteProperty(globalThis, "navigator");
      } else {
        Object.defineProperty(globalThis, "navigator", {
          configurable: true,
          value: previousNavigator,
        });
      }
    },
  };
}

function restoreGlobalProperty(
  key: keyof typeof globalThis,
  descriptor: PropertyDescriptor | undefined,
): void {
  if (descriptor === undefined) {
    Reflect.deleteProperty(globalThis, key);
    return;
  }
  Object.defineProperty(globalThis, key, descriptor);
}

function installFakeBrowserFrameRuntime(): {
  flushNextFrame(): Promise<void>;
  restore(): void;
} {
  const previousRequestAnimationFrame = Object.getOwnPropertyDescriptor(
    globalThis,
    "requestAnimationFrame",
  );
  const previousCancelAnimationFrame = Object.getOwnPropertyDescriptor(
    globalThis,
    "cancelAnimationFrame",
  );
  const previousResizeObserver = Object.getOwnPropertyDescriptor(
    globalThis,
    "ResizeObserver",
  );
  const previousAddEventListener = Object.getOwnPropertyDescriptor(
    globalThis,
    "addEventListener",
  );
  const previousRemoveEventListener = Object.getOwnPropertyDescriptor(
    globalThis,
    "removeEventListener",
  );
  const previousDevicePixelRatio = Object.getOwnPropertyDescriptor(
    globalThis,
    "devicePixelRatio",
  );
  let nextFrameId = 1;
  const frameCallbacks = new Map<number, FrameRequestCallback>();

  class FakeResizeObserver implements ResizeObserver {
    disconnect(): void {}
    observe(): void {}
    unobserve(): void {}
  }

  Object.defineProperty(globalThis, "requestAnimationFrame", {
    configurable: true,
    value(callback: FrameRequestCallback): number {
      const frameId = nextFrameId;
      nextFrameId += 1;
      frameCallbacks.set(frameId, callback);
      return frameId;
    },
  });
  Object.defineProperty(globalThis, "cancelAnimationFrame", {
    configurable: true,
    value(frameId: number): void {
      frameCallbacks.delete(frameId);
    },
  });
  Object.defineProperty(globalThis, "ResizeObserver", {
    configurable: true,
    value: FakeResizeObserver,
  });
  Object.defineProperty(globalThis, "addEventListener", {
    configurable: true,
    value(): void {},
  });
  Object.defineProperty(globalThis, "removeEventListener", {
    configurable: true,
    value(): void {},
  });
  Object.defineProperty(globalThis, "devicePixelRatio", {
    configurable: true,
    value: 1,
  });

  return {
    async flushNextFrame(): Promise<void> {
      const next = frameCallbacks.entries().next();
      assert.equal(next.done, false);
      const [frameId, callback] = next.value;
      frameCallbacks.delete(frameId);
      callback(globalThis.performance?.now() ?? 0);
      await flushPromiseContinuations();
    },
    restore(): void {
      restoreGlobalProperty("requestAnimationFrame", previousRequestAnimationFrame);
      restoreGlobalProperty("cancelAnimationFrame", previousCancelAnimationFrame);
      restoreGlobalProperty("ResizeObserver", previousResizeObserver);
      restoreGlobalProperty("addEventListener", previousAddEventListener);
      restoreGlobalProperty("removeEventListener", previousRemoveEventListener);
      restoreGlobalProperty("devicePixelRatio", previousDevicePixelRatio);
    },
  };
}

async function flushPromiseContinuations(): Promise<void> {
  await new Promise<void>((resolve) => setImmediate(resolve));
}

function makeLiveBrowserSnapshot(
  revision: number,
  workspaceSurface: WorkspaceSurfaceInfo | null,
  liveRuntimeActive = workspaceSurface !== null,
  nativePresented = false,
): StateSnapshot {
  return makeBrowserTestSnapshot({
    state_revision: revision,
    active_workflow: "live",
    source: makeBrowserTestSource({
      kind: "video_stream",
      locator: "device:0",
      device_index: 0,
      capture_width: 1280,
      capture_height: 720,
      capture_fps: 60,
      v4l2_buffer_count: 4,
    }),
    runtime_capabilities: {
      host_backend: "cef",
      navigator_gpu: "available",
      workspace_surface_bridge: "available",
      workspace_surface_zero_copy: "available",
    },
    workspace_surface: workspaceSurface,
    workflow_state: liveRuntimeActive
      ? {
          live_runtime: makeLivePredictRuntimeFixture({
            preview: {
              native_presented: nativePresented,
              display_startup_state: nativePresented
                ? "native presented"
                : "surface published",
            },
          }),
        }
      : {},
  });
}

function createBrowserWorkspaceHarness(
  snapshot: StateSnapshot,
): AngularServiceHarness {
  return createAngularServiceHarness(
    snapshot,
    makeBrowserWorkflowServiceProviders({ workspace: true }),
    { mode: "native" },
  );
}

test("workspaceClipRectForInput projects crop coordinates into workspace frame space", () => {
  const clipRect = workspaceClipRectForInput(makeInput());

  assert.deepEqual(clipRect, {
    x: 122,
    y: 64,
    width: 256,
    height: 72,
  });
});

test("workspaceOverlayBounds unions annotation primitives in device space", () => {
  const bounds = workspaceOverlayBounds([
    makeTestOverlayRectPrimitive(),
    {
      kind: "circle",
      center: { x: 220, y: 180 },
      radius: 16,
      strokeStyle: "rgba(124, 198, 255, 1)",
      lineWidth: 4,
      fillStyle: null,
      dash: [],
    },
  ]);

  assert.deepEqual(bounds, {
    x: 31,
    y: 23,
    width: 207,
    height: 175,
  });
});

test("buildWorkspaceCanvasLayout scales viewport and overlay bounds into CSS space", () => {
  const input = makeInput();
  input.overlayPrimitives = [makeTestOverlayRectPrimitive()];

  const report = buildWorkspaceCanvasLayout(input, {
    devicePixelRatio: 2,
    surfaceBoundsCss: {
      x: 10,
      y: 20,
      width: 640,
      height: 360,
    },
  });

  assert.deepEqual(report.viewportBoundsDevice, {
    x: 100,
    y: 40,
    width: 960,
    height: 540,
  });
  assert.deepEqual(report.viewportBoundsCss, {
    x: 60,
    y: 40,
    width: 480,
    height: 270,
  });
  assert.deepEqual(report.clipBoundsCss, {
    x: 71,
    y: 52,
    width: 128,
    height: 36,
  });
  assert.deepEqual(report.overlayBoundsCss, {
    x: 25.5,
    y: 31.5,
    width: 65,
    height: 49,
  });
  assert.equal(report.scaleDevicePx, 0.5);
  assert.equal(report.scaleCssPx, 0.25);
});

test("buildWorkspaceRenderFrame packages the render input with canvas layout geometry", () => {
  const input = makeInput();
  const frame = buildWorkspaceRenderFrame(input, {
    devicePixelRatio: 2,
    surfaceBoundsCss: {
      x: 10,
      y: 20,
      width: 640,
      height: 360,
    },
  });

  assert.equal(frame.input, input);
  assert.deepEqual(frame.canvasLayout.viewportBoundsDevice, {
    x: 100,
    y: 40,
    width: 960,
    height: 540,
  });
  assert.deepEqual(frame.canvasLayout.viewportBoundsCss, {
    x: 60,
    y: 40,
    width: 480,
    height: 270,
  });
});

test("workspaceCanvasLayoutsEqual compares full layout payloads", () => {
  const left = buildWorkspaceCanvasLayout(makeInput(), {
    devicePixelRatio: 2,
    surfaceBoundsCss: {
      x: 10,
      y: 20,
      width: 640,
      height: 360,
    },
  });
  const right = buildWorkspaceCanvasLayout(makeInput(), {
    devicePixelRatio: 2,
    surfaceBoundsCss: {
      x: 10,
      y: 20,
      width: 640,
      height: 360,
    },
  });
  const changed = buildWorkspaceCanvasLayout(
    {
      ...makeInput(),
      frameX: 120,
    },
    {
      devicePixelRatio: 2,
      surfaceBoundsCss: {
        x: 10,
        y: 20,
        width: 640,
        height: 360,
      },
    },
  );

  assert.equal(workspaceCanvasLayoutsEqual(left, right), true);
  assert.equal(workspaceCanvasLayoutsEqual(left, changed), false);
});

test("WorkspaceWebGpuRenderer copies imported live workspace revisions and releases native surfaces", async () => {
  const currentSurface = { value: makeWorkspaceSurface("101") };
  const bridge = installFakeWebGpuBridge(currentSurface);
  const webgpu = installFakeWebGpuRuntime();
  const renderer = new WorkspaceWebGpuRenderer();

  try {
    renderer.attachCanvas(webgpu.canvas);

    const first = await renderer.render(
      buildWorkspaceRenderFrame(makeInput(), {
        devicePixelRatio: 1,
        surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
      }),
    );

    assert.equal(first.captureSource, "bridge");
    assert.equal(first.surfaceInfo?.revision, "101");
    assert.equal(first.statusText, "webgpu ready · surface 101");
    assert.equal(bridge.imports.length, 1);
    assert.equal(bridge.imports[0].revision, "101");
    assert.deepEqual(webgpu.textureCopies, [
      {
        sourceRevision: "101",
        destinationRevision: "created",
        width: 1920,
        height: 1080,
      },
    ]);
    assert.equal(webgpu.submittedWork.length, 1);
    assert.equal(bridge.releases.length, 0);

    webgpu.submittedWork[0].resolve(undefined);
    await flushPromiseContinuations();
    assert.equal(bridge.imports[0].destroyed, true);
    assert.deepEqual(bridge.releases, ["101"]);

    currentSurface.value = makeWorkspaceSurface("102");
    const second = await renderer.render(
      buildWorkspaceRenderFrame(makeInput(), {
        devicePixelRatio: 1,
        surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
      }),
    );

    assert.equal(second.captureSource, "bridge");
    assert.equal(second.surfaceInfo?.revision, "102");
    assert.equal(bridge.imports.length, 2);
    assert.equal(bridge.imports[1].revision, "102");
    assert.deepEqual(webgpu.textureCopies.slice(-1), [
      {
        sourceRevision: "102",
        destinationRevision: "created",
        width: 1920,
        height: 1080,
      },
    ]);
    assert.deepEqual(bridge.releases, ["101"]);
    assert.equal(webgpu.submittedWork.length, 2);

    webgpu.submittedWork[1].resolve(undefined);
    await flushPromiseContinuations();
    assert.equal(bridge.imports[1].destroyed, true);
    assert.deepEqual(bridge.releases, ["101", "102"]);

    renderer.dispose();
    assert.equal(webgpu.submittedWork.length, 2);
    assert.equal(bridge.imports[1].destroyed, true);
    assert.deepEqual(bridge.releases, ["101", "102"]);
  } finally {
    renderer.dispose();
    webgpu.restore();
    bridge.restore();
  }
});

test("WorkspaceWebGpuRenderer leaves native-presented live frames out of the bridge import path", async () => {
  const currentSurface = { value: makeWorkspaceSurface("201") };
  const bridge = installFakeWebGpuBridge(currentSurface);
  const webgpu = installFakeWebGpuRuntime();
  const events: WorkspaceRendererHostEvent[] = [];
  const renderer = new WorkspaceWebGpuRenderer((event) => events.push(event));

  try {
    renderer.attachCanvas(webgpu.canvas);
    const stats = await renderer.render(
      buildWorkspaceRenderFrame(
        { ...makeInput(), workflow: "annotate", nativePresentedLive: true },
        {
          devicePixelRatio: 1,
          surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
        },
      ),
    );

    assert.equal(stats.captureSource, "native");
    assert.equal(stats.surfaceInfo, null);
    assert.equal(stats.statusText, "native-presented live workspace");
    assert.equal(stats.displayStartupState, "native presented");
    assert.deepEqual(bridge.imports, []);
    assert.deepEqual(bridge.releases, []);
    assert.deepEqual(bridge.rendererTextureReleases, []);
    assert.deepEqual(events, []);
    assert.equal(webgpu.submittedWork.length, 0);
  } finally {
    renderer.dispose();
    webgpu.restore();
    bridge.restore();
  }
});

test("WorkspaceWebGpuRenderer dissociates only the renderer texture after WebGPU device loss", async () => {
  const currentSurface = { value: makeWorkspaceSurface("501") };
  const bridge = installFakeWebGpuBridge(currentSurface);
  const webgpu = installFakeWebGpuRuntime();
  const events: WorkspaceRendererHostEvent[] = [];
  const renderer = new WorkspaceWebGpuRenderer((event) => events.push(event));

  try {
    renderer.attachCanvas(webgpu.canvas);

    const first = await renderer.render(
      buildWorkspaceRenderFrame(makeInput(), {
        devicePixelRatio: 1,
        surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
      }),
    );

    assert.equal(first.captureSource, "bridge");
    assert.equal(first.surfaceInfo?.revision, "501");
    assert.deepEqual(bridge.releases, []);
    assert.deepEqual(bridge.rendererTextureReleases, []);

    webgpu.loseDevice({
      message: "Device was destroyed.",
      reason: "destroyed",
    } as GPUDeviceLostInfo);
    await flushPromiseContinuations();

    assert.deepEqual(bridge.rendererTextureReleases, ["501"]);
    assert.deepEqual(bridge.releases, []);
    assert.equal(events.at(-1)?.intent, "workspace.release_complete");
    assert.equal(events.at(-1)?.operation, "releaseRendererTexture");
    assert.equal(
      events.at(-1)?.resultDetail,
      "cause=device_lost:destroyed:Device was destroyed.",
    );

    const second = await renderer.render(
      buildWorkspaceRenderFrame(makeInput(), {
        devicePixelRatio: 1,
        surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
      }),
    );

    assert.equal(second.captureSource, "bridge");
    assert.equal(second.surfaceInfo?.revision, "501");
    assert.equal(bridge.imports.length, 2);
    assert.equal(bridge.imports[1].revision, "501");
    assert.deepEqual(bridge.rendererTextureReleases, ["501"]);
    assert.deepEqual(bridge.releases, []);
  } finally {
    renderer.dispose();
    webgpu.restore();
    bridge.restore();
  }
});

test("Synthetic zero-copy WebGPU harness draws the imported workspace revision before native release", async () => {
  const currentSurface: { value: WorkspaceSurfaceInfo | null } = {
    value: makeWorkspaceSurface("401"),
  };
  const bridge = installFakeWebGpuBridge(currentSurface, {
    releaseResult: {
      released: true,
      resultCode: "ok",
      resultDetail: "",
    },
  });
  const webgpu = installFakeWebGpuRuntime();
  const hostEvents: Array<{
    intent: string;
    surfaceId: string;
    revision: string;
    resultCode: string;
    resultDetail: string;
  }> = [];
  const renderer = new WorkspaceWebGpuRenderer((event) => {
    hostEvents.push({
      intent: event.intent,
      surfaceId: event.surfaceId,
      revision: event.revision,
      resultCode: event.resultCode,
      resultDetail: event.resultDetail,
    });
  });

  try {
    renderer.attachCanvas(webgpu.canvas);
    const first = await renderer.render(
      buildWorkspaceRenderFrame(
        { ...makeInput(), workflow: "live" },
        {
          devicePixelRatio: 1,
          surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
        },
      ),
    );

    assert.equal(first.captureSource, "bridge");
    assert.equal(first.surfaceInfo?.revision, "401");
    assert.equal(first.importedRevision, "401");
    assert.equal(first.drawnRevision, "");
    assert.equal(first.displayStartupState, "draw submitted");
    assert.deepEqual(
      hostEvents.map((event) => event.intent),
      ["workspace.imported"],
    );
    assert.deepEqual(bridge.releases, []);
    assert.equal(webgpu.submittedWork.length, 1);
    assert.deepEqual(webgpu.textureCopies, [
      {
        sourceRevision: "401",
        destinationRevision: "created",
        width: 1920,
        height: 1080,
      },
    ]);

    currentSurface.value = null;
    const second = await renderer.render(
      buildWorkspaceRenderFrame(
        { ...makeInput(), workflow: "live" },
        {
          devicePixelRatio: 1,
          surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
        },
      ),
    );

    assert.equal(second.captureSource, "bridge");
    assert.equal(second.surfaceInfo?.revision, "401");
    assert.equal(second.statusText, "webgpu ready · surface 401");
    assert.equal(second.releasePendingRevision, "401");
    assert.deepEqual(bridge.releases, []);
    assert.equal(webgpu.submittedWork.length, 1);
    assert.deepEqual(
      hostEvents.map((event) => `${event.intent}:${event.revision}:${event.resultCode}`),
      ["workspace.imported:401:ok"],
    );
    assert.equal(hostEvents[0].resultDetail, "copied to renderer-owned texture");

    webgpu.submittedWork[0].resolve(undefined);
    await flushPromiseContinuations();
    assert.deepEqual(bridge.releases, ["401"]);
    assert.deepEqual(
      hostEvents.map((event) => `${event.intent}:${event.revision}:${event.resultCode}`),
      [
        "workspace.imported:401:ok",
        "workspace.drawn:401:ok",
        "workspace.release_complete:401:ok",
      ],
    );
    assert.equal(hostEvents[1].resultDetail, "");
    assert.equal(hostEvents.at(-1)?.intent, "workspace.release_complete");
    assert.equal(hostEvents.at(-1)?.resultDetail, "cause=copied_to_renderer_texture");
  } finally {
    renderer.dispose();
    webgpu.restore();
    bridge.restore();
  }
});

test("WorkspaceWebGpuRenderer emits one acquire failure event for a missing live surface", async () => {
  const bridge = installFakeWebGpuBridge({ value: null });
  const webgpu = installFakeWebGpuRuntime();
  const hostEvents: Array<{
    intent: string;
    revision: string;
    operation: string;
    resultCode: string;
    resultDetail: string;
  }> = [];
  const renderer = new WorkspaceWebGpuRenderer((event) => {
    hostEvents.push({
      intent: event.intent,
      revision: event.revision,
      operation: event.operation,
      resultCode: event.resultCode,
      resultDetail: event.resultDetail,
    });
  });

  try {
    renderer.attachCanvas(webgpu.canvas);
    for (let i = 0; i < 2; ++i) {
      const result = await renderer.render(
        buildWorkspaceRenderFrame(
          { ...makeInput(), workflow: "live" },
          {
            devicePixelRatio: 1,
            surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
          },
        ),
      );
      assert.equal(result.captureSource, "blocked");
      assert.equal(result.statusText, "webgpu blocked · no published live surface");
    }

    assert.deepEqual(hostEvents, [
      {
        intent: "workspace.acquire_failed",
        revision: "",
        operation: "acquireCurrentSurface",
        resultCode: "no_published_live_surface",
        resultDetail: "no published live workspace surface",
      },
    ]);
  } finally {
    renderer.dispose();
    webgpu.restore();
    bridge.restore();
  }
});

test("WorkspaceWebGpuRenderer emits import and release failure host result details", async () => {
  const currentSurface = { value: makeWorkspaceSurface("501") };
  const bridge = installFakeWebGpuBridge(currentSurface, {
    importError: new Error(
      "workspace GPU bridge blocked [dawn_import_failure]: Dawn import failed",
    ),
    releaseResult: {
      released: false,
      resultCode: "release_failure",
      resultDetail: "native release rejected",
    },
  });
  const webgpu = installFakeWebGpuRuntime();
  const hostEvents: Array<{
    intent: string;
    revision: string;
    resultCode: string;
    resultDetail: string;
  }> = [];
  const renderer = new WorkspaceWebGpuRenderer((event) => {
    hostEvents.push({
      intent: event.intent,
      revision: event.revision,
      resultCode: event.resultCode,
      resultDetail: event.resultDetail,
    });
  });

  try {
    renderer.attachCanvas(webgpu.canvas);
    const result = await renderer.render(
      buildWorkspaceRenderFrame(
        { ...makeInput(), workflow: "live" },
        {
          devicePixelRatio: 1,
          surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
        },
      ),
    );

    assert.equal(result.captureSource, "blocked");
    assert.equal(result.statusText, "webgpu blocked · dawn texture import failed");
    assert.deepEqual(
      hostEvents.map(
        (event) => `${event.intent}:${event.revision}:${event.resultCode}:${event.resultDetail}`,
      ),
      [
        "workspace.import_failed:501:dawn_import_failure:Dawn import failed",
        "workspace.release_rejected:501:release_failure:native release rejected",
      ],
    );
  } finally {
    renderer.dispose();
    webgpu.restore();
    bridge.restore();
  }
});

test("Browser workspace uses native-presented live status without bridge imports", async () => {
  const harness = createBrowserWorkspaceHarness(makeLiveBrowserSnapshot(300, null, true, true));
  const currentSurface = {
    get value(): WorkspaceSurfaceInfo | null {
      return harness.transport.getSnapshot().workspace_surface ?? null;
    },
  };
  const nativeSlotRevisions: string[] = [];
  let stateRevision = 300;
  const publishLiveRevision = (revision: string): void => {
    nativeSlotRevisions.push(revision);
    stateRevision += 1;
    harness.transport.publish(
      makeLiveBrowserSnapshot(stateRevision, makeWorkspaceSurface(revision), true, true),
    );
  };
  const clearLiveRevision = (): void => {
    stateRevision += 1;
    harness.transport.publish(makeLiveBrowserSnapshot(stateRevision, null, true, true));
  };
  const bridge = installFakeWebGpuBridge(currentSurface, {
    onRelease(revision: string): void {
      assert.fail(`native-presented live should not release bridge revision ${revision}`);
    },
  });
  const webgpu = installFakeWebGpuRuntime();
  const frameRuntime = installFakeBrowserFrameRuntime();

  try {
    const workspace = harness.run(() => inject(BrowserWorkspaceStateService));
    const runtime = harness.run(() => inject(BrowserHostRuntimeState));
    assert.equal(
      runtime.runtimeCapabilityStatus().find(
        (item) => item.key === "workspace_surface_zero_copy",
      )?.status,
      "ready",
    );
    workspace.registerWorkspaceCanvas(webgpu.canvas, () =>
      workspace.workspaceRenderInput(),
    );
    await frameRuntime.flushNextFrame();

    assert.equal(workspace.rendererStatus(), "native-presented live workspace");
    assert.equal(bridge.imports.length, 0);
    assert.deepEqual(bridge.releases, []);
    assert.equal(webgpu.submittedWork.length, 0);

    publishLiveRevision("301");
    workspace.requestWorkspaceRender();
    await frameRuntime.flushNextFrame();
    assert.equal(workspace.rendererStatus(), "native-presented live workspace");
    assert.equal(harness.transport.getSnapshot().workspace_surface?.revision, "301");
    assert.deepEqual(bridge.imports, []);
    assert.deepEqual(bridge.releases, []);
    assert.equal(webgpu.submittedWork.length, 0);

    clearLiveRevision();
    workspace.requestWorkspaceRender();
    await frameRuntime.flushNextFrame();
    assert.equal(workspace.rendererStatus(), "native-presented live workspace");
    assert.deepEqual(bridge.imports, []);
    assert.deepEqual(bridge.releases, []);
    assert.equal(webgpu.submittedWork.length, 0);

    publishLiveRevision("302");
    workspace.requestWorkspaceRender();
    await frameRuntime.flushNextFrame();
    assert.equal(workspace.rendererStatus(), "native-presented live workspace");
    assert.deepEqual(nativeSlotRevisions, ["301", "302"]);
    assert.deepEqual(bridge.imports, []);
    assert.deepEqual(bridge.releases, []);
    assert.equal(webgpu.submittedWork.length, 0);
  } finally {
    harness.destroy();
    frameRuntime.restore();
    webgpu.restore();
    bridge.restore();
  }
});

test("Browser workspace keeps active live in the bridge import path until native-presented publication", async () => {
  const harness = createBrowserWorkspaceHarness(
    makeLiveBrowserSnapshot(200, makeWorkspaceSurface("200"), true, false),
  );
  const currentSurface = {
    get value(): WorkspaceSurfaceInfo | null {
      return harness.transport.getSnapshot().workspace_surface ?? null;
    },
  };
  const bridge = installFakeWebGpuBridge(currentSurface);
  const webgpu = installFakeWebGpuRuntime();
  const frameRuntime = installFakeBrowserFrameRuntime();

  try {
    const workspace = harness.run(() => inject(BrowserWorkspaceStateService));
    workspace.registerWorkspaceCanvas(webgpu.canvas, () =>
      workspace.workspaceRenderInput(),
    );
    await frameRuntime.flushNextFrame();

    assert.equal(workspace.rendererStatus(), "webgpu ready · surface 200");
    assert.equal(bridge.imports.length, 1);
    assert.equal(bridge.imports[0].revision, "200");
    assert.deepEqual(bridge.releases, []);
    assert.equal(webgpu.submittedWork.length, 1);

    webgpu.submittedWork[0].resolve(undefined);
    await flushPromiseContinuations();
    assert.deepEqual(bridge.releases, ["200"]);

    harness.transport.publish(
      makeLiveBrowserSnapshot(201, makeWorkspaceSurface("201"), true, true),
    );
    workspace.requestWorkspaceRender();
    await frameRuntime.flushNextFrame();

    assert.equal(workspace.rendererStatus(), "native-presented live workspace");
    assert.equal(bridge.imports.length, 1);
    assert.deepEqual(bridge.releases, ["200"]);
    assert.equal(webgpu.submittedWork.length, 1);
  } finally {
    harness.destroy();
    frameRuntime.restore();
    webgpu.restore();
    bridge.restore();
  }
});

test("WorkspaceWebGpuRenderer reports distinct blocked bridge states", async () => {
  const webgpu = installFakeWebGpuRuntime();
  const renderer = new WorkspaceWebGpuRenderer();

  try {
    renderer.attachCanvas(webgpu.canvas);

    const previousBridge = globalThis.__MMLTK_WORKSPACE_GPU_BRIDGE__;
    delete globalThis.__MMLTK_WORKSPACE_GPU_BRIDGE__;
    let result = await renderer.render(
      buildWorkspaceRenderFrame(makeInput(), {
        devicePixelRatio: 1,
        surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
      }),
    );
    assert.equal(result.captureSource, "blocked");
    assert.equal(result.statusText, "webgpu blocked · workspace gpu bridge missing");
    globalThis.__MMLTK_WORKSPACE_GPU_BRIDGE__ = previousBridge;

    let bridge = installFakeWebGpuBridge({ value: null });
    result = await renderer.render(
      buildWorkspaceRenderFrame(makeInput(), {
        devicePixelRatio: 1,
        surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
      }),
    );
    assert.equal(result.captureSource, "blocked");
    assert.equal(result.statusText, "webgpu blocked · no published live surface");
    bridge.restore();

    bridge = installFakeWebGpuBridge(
      { value: makeWorkspaceSurface("201") },
      {
        acquireError: new Error(
          "workspace GPU bridge blocked [shared_image_export_rejected]: missing Linux import metadata",
        ),
      },
    );
    result = await renderer.render(
      buildWorkspaceRenderFrame(makeInput(), {
        devicePixelRatio: 1,
        surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
      }),
    );
    assert.equal(result.captureSource, "blocked");
    assert.equal(result.statusText, "webgpu blocked · shared image export rejected");
    bridge.restore();

    bridge = installFakeWebGpuBridge(
      { value: makeWorkspaceSurface("202") },
      { importError: new Error("SharedImage import failed in renderer") },
    );
    result = await renderer.render(
      buildWorkspaceRenderFrame(makeInput(), {
        devicePixelRatio: 1,
        surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
      }),
    );
    assert.equal(result.captureSource, "blocked");
    assert.equal(result.statusText, "webgpu blocked · renderer texture import rejected");
    assert.deepEqual(bridge.releases, ["202"]);
    bridge.restore();

    bridge = installFakeWebGpuBridge(
      { value: makeWorkspaceSurface("203") },
      { acquireError: new Error("workspace allocation failed") },
    );
    result = await renderer.render(
      buildWorkspaceRenderFrame(makeInput(), {
        devicePixelRatio: 1,
        surfaceBoundsCss: { x: 0, y: 0, width: 640, height: 360 },
      }),
    );
    assert.equal(result.captureSource, "blocked");
    assert.equal(result.statusText, "webgpu blocked · workspace allocation failed");
    bridge.restore();
  } finally {
    renderer.dispose();
    webgpu.restore();
    delete globalThis.__MMLTK_WORKSPACE_GPU_BRIDGE__;
  }
});
