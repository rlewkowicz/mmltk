import type { WorkspaceRenderFrame } from "../../workspace_renderer";
import type { WorkspaceSurfaceInfo } from "../../host_api";
import {
  acquireWorkspaceSurfaceResult,
  importWorkspaceSurfaceTextureResult,
  releaseWorkspaceRendererTexture,
  releaseWorkspaceSurface,
  type WorkspaceGpuBridgeBlockedReason,
  type WorkspaceGpuBridgeResultCode,
  type WorkspaceSurfaceReleaseResult,
} from "./workspace-gpu-bridge";

import workspaceCaptureShader from "../../shaders/workspace_capture.wgsl";

const kClearColor: GPUColor = {
  r: 0.028,
  g: 0.035,
  b: 0.042,
  a: 1,
};
const kCaptureUniformFloatCount = 16;
type CaptureSource = "bridge" | "blocked" | "unsupported";
type WorkspaceRendererCaptureSource = CaptureSource | "native";
type CaptureBlockedReason = WorkspaceGpuBridgeBlockedReason;
export type WorkspaceRendererDisplayStartupState =
  | "idle"
  | "capture running"
  | "surface published"
  | "texture imported"
  | "draw submitted"
  | "native presented";

export type WorkspaceRendererHostEventIntent =
  | "workspace.drawn"
  | "workspace.imported"
  | "workspace.import_failed"
  | "workspace.acquire_failed"
  | "workspace.release_complete"
  | "workspace.release_rejected";

export interface WorkspaceRendererHostEvent {
  workflow: WorkspaceRenderFrame["input"]["workflow"];
  intent: WorkspaceRendererHostEventIntent;
  surfaceId: string;
  revision: string;
  operation: string;
  resultCode: WorkspaceGpuBridgeResultCode | "ok";
  resultDetail: string;
}

export type WorkspaceRendererHostEventSink = (
  event: WorkspaceRendererHostEvent,
) => void;

interface RetainedWorkspaceSurface {
  info: WorkspaceSurfaceInfo;
  texture: GPUTexture;
  view: GPUTextureView;
  workflow: WorkspaceRenderFrame["input"]["workflow"];
}

interface PendingWorkspaceSurfaceRelease {
  info: WorkspaceSurfaceInfo;
  workflow: WorkspaceRenderFrame["input"]["workflow"];
  importedTexture: GPUTexture;
}

export interface WorkspaceWebGpuRenderStats {
  captureSource: WorkspaceRendererCaptureSource;
  overlayDrawCount: number;
  surfaceInfo: WorkspaceSurfaceInfo | null;
  statusText: string;
  acquiredRevision: string;
  importedRevision: string;
  submittedRevision: string;
  drawnRevision: string;
  releasePendingRevision: string;
  lastDrawError: string;
  displayStartupState: WorkspaceRendererDisplayStartupState;
}

function workflowCode(workflow: WorkspaceRenderFrame["input"]["workflow"]): number {
  switch (workflow) {
    case "train":
      return 0;
    case "validate":
      return 1;
    case "predict":
      return 2;
    case "annotate":
      return 3;
    case "export":
      return 4;
    case "live":
      return 5;
  }
}

function errorTextFromUnknown(error: unknown): string {
  if (error instanceof Error && error.message.trim().length > 0) {
    return error.message;
  }
  const message = String(error ?? "");
  return message.trim().length > 0 ? message : "workspace render failed";
}

export class WorkspaceWebGpuRenderer {
  private canvas: HTMLCanvasElement | null = null;
  private context: GPUCanvasContext | null = null;
  private adapter: GPUAdapter | null = null;
  private device: GPUDevice | null = null;
  private canvasFormat: GPUTextureFormat | null = null;
  private sampler: GPUSampler | null = null;
  private capturePipeline: GPURenderPipeline | null = null;
  private captureUniformBuffer: GPUBuffer | null = null;
  private captureUniformData = new Float32Array(kCaptureUniformFloatCount);
  private captureBindGroup: GPUBindGroup | null = null;
  private captureBindGroupView: GPUTextureView | null = null;
  private currentSurface: RetainedWorkspaceSurface | null = null;
  private initialization: Promise<boolean> | null = null;
  private disposed = false;
  private lastStatus = "webgpu pending";
  private acquiredRevision = "";
  private importedRevision = "";
  private submittedRevision = "";
  private drawnRevision = "";
  private releasePendingRevision = "";
  private lastDrawError = "";
  private lastDrawnEventRevision = "";
  private lastImportedEventRevision = "";
  private lastAcquireFailureEventKey = "";
  private lastWebGpuDiagnosticEventKey = "";
  private lastWebGpuUncapturedError = "";
  private activeWorkflow: WorkspaceRenderFrame["input"]["workflow"] = "annotate";
  private releasePendingCauses = new Map<string, string>();
  private surfaceFailureCauses = new Map<string, string>();

  constructor(private readonly hostEventSink: WorkspaceRendererHostEventSink = () => {}) {}

  attachCanvas(canvas: HTMLCanvasElement): void {
    if (this.canvas === canvas) {
      return;
    }
    this.canvas = canvas;
    this.context = null;
    this.captureBindGroup = null;
    this.captureBindGroupView = null;
  }

  async render(frame: WorkspaceRenderFrame): Promise<WorkspaceWebGpuRenderStats> {
    this.activeWorkflow = frame.input.workflow;
    if (this.disposed) {
      return this.renderStats("unsupported", null, "webgpu disposed");
    }
    if (frame.input.nativePresentedLive) {
      return this.renderNativePresentedLive(frame);
    }
    const ready = await this.ensureReady();
    if (!ready || this.device === null || this.context === null) {
      return this.renderStats("unsupported", null, this.lastStatus);
    }

    const {
      captureSource,
      captureView,
      blockedReason,
      surfaceInfo,
      releaseAfterSubmit,
      destroyAfterSubmit,
    } = this.prepareCaptureSurface(frame);
    if (captureSource === "bridge") {
      this.writeCaptureUniforms(frame);
    }
    const currentTextureView = this.context.getCurrentTexture().createView();

    const encoder = this.device.createCommandEncoder({
      label: "workspace-webgpu-renderer",
    });
    this.copyPendingImports(encoder, releaseAfterSubmit, destroyAfterSubmit);
    const pass = encoder.beginRenderPass({
      colorAttachments: [
        {
          view: currentTextureView,
          clearValue: kClearColor,
          loadOp: "clear",
          storeOp: "store",
        },
      ],
    });

    if (captureSource === "bridge" && captureView !== null) {
      const captureBindGroup = this.captureBindGroupForView(captureView);
      pass.setPipeline(this.capturePipeline!);
      pass.setBindGroup(0, captureBindGroup);
      pass.draw(3);
    }

    pass.end();
    this.device.queue.submit([encoder.finish()]);
    if (captureSource === "bridge" && captureView !== null && surfaceInfo !== null) {
      this.submittedRevision = surfaceInfo.revision;
      this.lastDrawError = "";
    }
    for (const surface of releaseAfterSubmit) {
      this.completeSubmittedSurfaceAfterSubmittedWork(surface, "copied_to_renderer_texture");
    }
    for (const surface of destroyAfterSubmit) {
      this.destroyRetainedSurfaceAfterSubmittedWork(surface);
    }

    const statusText = this.statusTextFromRender(
      captureSource,
      blockedReason,
      surfaceInfo,
    );
    this.lastStatus = statusText;
    return this.renderStats(captureSource, surfaceInfo, statusText);
  }

  private async renderNativePresentedLive(
    frame: WorkspaceRenderFrame,
  ): Promise<WorkspaceWebGpuRenderStats> {
    this.activeWorkflow = frame.input.workflow;
    this.releaseCurrentSurface("native_presented_live");
    this.acquiredRevision = "";
    this.importedRevision = "";
    this.submittedRevision = "";
    this.drawnRevision = "";
    this.releasePendingRevision = "";
    this.lastAcquireFailureEventKey = "";
    this.lastDrawError = "";
    this.lastStatus = "native-presented live workspace";

    return this.renderStats("native", null, this.lastStatus);
  }

  dispose(): void {
    this.disposed = true;
    this.releaseCurrentSurface();
    this.captureBindGroup = null;
    this.captureBindGroupView = null;
    this.captureUniformBuffer?.destroy();
    this.captureUniformBuffer = null;
    if (this.context !== null && typeof this.context.unconfigure === "function") {
      this.context.unconfigure();
    }
    this.context = null;
    this.capturePipeline = null;
    this.sampler = null;
    this.device = null;
    this.adapter = null;
    this.canvasFormat = null;
  }

  private async ensureReady(): Promise<boolean> {
    if (this.disposed) {
      return false;
    }
    if (
      this.canvas !== null &&
      this.context !== null &&
      this.device !== null &&
      this.capturePipeline !== null &&
      this.captureUniformBuffer !== null &&
      this.sampler !== null &&
      this.canvasFormat !== null
    ) {
      return true;
    }
    if (this.initialization !== null) {
      return this.initialization;
    }
    this.initialization = this.initializeRenderer().finally(() => {
      this.initialization = null;
    });
    return this.initialization;
  }

  private async initializeRenderer(): Promise<boolean> {
    if (this.canvas === null) {
      this.lastStatus = "webgpu pending";
      return false;
    }
    if (typeof navigator === "undefined" || navigator.gpu === undefined) {
      this.lastStatus = "webgpu unavailable";
      return false;
    }

    const context = this.canvas.getContext("webgpu");
    if (context === null) {
      this.lastStatus = "webgpu context unavailable";
      return false;
    }

    const adapter = await navigator.gpu.requestAdapter();
    if (adapter === null) {
      this.lastStatus = "webgpu adapter unavailable";
      return false;
    }

    const device = await adapter.requestDevice();
    device.addEventListener("uncapturederror", (event: GPUUncapturedErrorEvent) => {
      const errorObject = event.error as GPUError & {
        readonly constructor?: { readonly name?: string };
        readonly name?: string;
      };
      const errorName =
        typeof errorObject.name === "string" && errorObject.name.trim().length > 0
          ? errorObject.name.trim()
          : typeof errorObject.constructor?.name === "string" &&
              errorObject.constructor.name.trim().length > 0
            ? errorObject.constructor.name.trim()
            : "GPUError";
      const errorMessage = event.error.message.trim();
      const resultDetail =
        errorMessage.length > 0
          ? `${errorName}: ${errorMessage}`
          : `${errorName}: WebGPU uncaptured error`;
      this.lastWebGpuUncapturedError = resultDetail;
      this.lastDrawError = `webgpu uncaptured error: ${resultDetail}`;
      console.error(
        `[mmltk.webgpu] uncaptured_error workflow=${this.activeWorkflow} surface=${this.currentSurface?.info.surfaceId ?? "workspace"} revision=${this.currentSurface?.info.revision ?? ""} ${resultDetail}`,
      );
      this.emitWebGpuDiagnosticEvent("webgpuUncapturedError", "webgpu_uncaptured_error", resultDetail);
    });
    device.lost.then((info) => {
      if (this.disposed) {
        return;
      }
      const lostMessage = info.message.trim();
      const lostInfoRecord = info as unknown as { reason?: unknown };
      const lostReason =
        typeof lostInfoRecord.reason === "string"
          ? lostInfoRecord.reason
          : "unknown";
      const lostCause =
        lostMessage.length > 0
          ? `device_lost:${lostReason}:${lostMessage}`
          : `device_lost:${lostReason}`;
      const detail =
        this.lastWebGpuUncapturedError.length > 0
          ? `${lostCause}; previous_uncaptured=${this.lastWebGpuUncapturedError}`
          : lostCause;
      this.lastDrawError = detail;
      console.error(
        `[mmltk.webgpu] device_lost workflow=${this.activeWorkflow} surface=${this.currentSurface?.info.surfaceId ?? "workspace"} revision=${this.currentSurface?.info.revision ?? ""} ${detail}`,
      );
      this.emitWebGpuDiagnosticEvent("deviceLost", "webgpu_device_lost", detail);
      this.releaseCurrentSurfaceForDeviceLoss(detail);
      this.lastImportedEventRevision = "";
      this.lastDrawnEventRevision = "";
      this.captureBindGroup = null;
      this.captureBindGroupView = null;
      this.capturePipeline = null;
      this.captureUniformBuffer = null;
      this.sampler = null;
      this.device = null;
      this.adapter = null;
      this.canvasFormat = null;
      this.context = null;
      this.lastStatus =
        info.message.trim().length > 0 ? `webgpu lost: ${info.message}` : "webgpu lost";
    });

    const canvasFormat = navigator.gpu.getPreferredCanvasFormat();
    context.configure({
      device,
      format: canvasFormat,
      alphaMode: "opaque",
    });

    const captureShaderModule = device.createShaderModule({
      label: "workspace-capture-shader",
      code: workspaceCaptureShader,
    });

    const captureUniformBuffer = device.createBuffer({
      label: "workspace-capture-uniforms",
      size: kCaptureUniformFloatCount * Float32Array.BYTES_PER_ELEMENT,
      usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });

    const sampler = device.createSampler({
      label: "workspace-capture-sampler",
      addressModeU: "clamp-to-edge",
      addressModeV: "clamp-to-edge",
      magFilter: "linear",
      minFilter: "linear",
    });

    const capturePipeline = device.createRenderPipeline({
      label: "workspace-capture-pipeline",
      layout: "auto",
      vertex: {
        module: captureShaderModule,
        entryPoint: "vs_main",
      },
      fragment: {
        module: captureShaderModule,
        entryPoint: "fs_main",
        targets: [{ format: canvasFormat }],
      },
      primitive: {
        topology: "triangle-list",
      },
    });

    this.context = context;
    this.adapter = adapter;
    this.device = device;
    this.canvasFormat = canvasFormat;
    this.captureUniformBuffer = captureUniformBuffer;
    this.sampler = sampler;
    this.capturePipeline = capturePipeline;
    this.lastStatus = "webgpu ready";
    return true;
  }

  private writeCaptureUniforms(frame: WorkspaceRenderFrame): void {
    if (this.device === null || this.captureUniformBuffer === null) {
      return;
    }
    const canvasLayout = frame.canvasLayout;
    this.captureUniformData.fill(0);
    this.captureUniformData[0] = Math.max(1, canvasLayout.canvasWidth);
    this.captureUniformData[1] = Math.max(1, canvasLayout.canvasHeight);
    this.captureUniformData[2] = canvasLayout.viewportBoundsDevice.x;
    this.captureUniformData[3] = canvasLayout.viewportBoundsDevice.y;
    this.captureUniformData[4] = Math.max(1, canvasLayout.viewportBoundsDevice.width);
    this.captureUniformData[5] = Math.max(1, canvasLayout.viewportBoundsDevice.height);
    this.captureUniformData[6] = Math.max(1, canvasLayout.captureWidth);
    this.captureUniformData[7] = Math.max(1, canvasLayout.captureHeight);
    this.captureUniformData[8] = workflowCode(frame.input.workflow);
    this.device.queue.writeBuffer(
      this.captureUniformBuffer,
      0,
      this.captureUniformData,
    );
  }

  private prepareCaptureSurface(frame: WorkspaceRenderFrame): {
    captureView: GPUTextureView | null;
    captureSource: CaptureSource;
    blockedReason: CaptureBlockedReason | null;
    surfaceInfo: WorkspaceSurfaceInfo | null;
    releaseAfterSubmit: PendingWorkspaceSurfaceRelease[];
    destroyAfterSubmit: RetainedWorkspaceSurface[];
  } {
    const acquired = acquireWorkspaceSurfaceResult();
    const acquiredSurface = acquired.surface;
    const releaseAfterSubmit: PendingWorkspaceSurfaceRelease[] = [];
    const destroyAfterSubmit: RetainedWorkspaceSurface[] = [];
    let blockedReason: CaptureBlockedReason | null = acquired.blockedReason;

    if (acquiredSurface === null) {
      const resultCode =
        acquired.resultCode ?? acquired.blockedReason ?? "no_published_live_surface";
      if (
        this.currentSurface !== null &&
        resultCode === "no_published_live_surface"
      ) {
        blockedReason = null;
      } else {
        this.emitAcquireFailure(frame, acquired);
        if (this.currentSurface !== null && resultCode !== "no_published_live_surface") {
          destroyAfterSubmit.push(this.currentSurface);
          this.currentSurface = null;
        }
      }
    } else if (this.currentSurface?.info.revision !== acquiredSurface.revision) {
      this.acquiredRevision = acquiredSurface.revision;
      const imported =
        this.device === null
          ? {
              operation: "importTexture" as const,
              texture: null,
              blockedReason: "renderer_import_rejected" as const,
              resultCode: "renderer_import_rejected" as const,
              resultDetail: "webgpu device unavailable",
            }
          : importWorkspaceSurfaceTextureResult(this.device, acquiredSurface);
      if (imported.texture === null) {
        const resultCode =
          imported.resultCode ?? imported.blockedReason ?? "renderer_import_rejected";
        const resultDetail =
          imported.resultDetail.length > 0
            ? imported.resultDetail
            : "workspace texture import failed";
        this.lastDrawError = resultDetail;
        this.lastAcquireFailureEventKey = "";
        this.emitHostEvent({
          workflow: frame.input.workflow,
          intent: "workspace.import_failed",
          surfaceId: acquiredSurface.surfaceId,
          revision: acquiredSurface.revision,
          operation: imported.operation,
          resultCode,
          resultDetail,
        });
        this.emitReleaseResult(
          frame.input.workflow,
          acquiredSurface,
          releaseWorkspaceSurface(acquiredSurface),
          "import_failed",
        );
        blockedReason = imported.blockedReason ?? "renderer_import_rejected";
      } else {
        const copied = this.retainImportedSurfaceTexture(
          imported.texture,
          acquiredSurface,
          frame.input.workflow,
        );
        if (copied.replacedSurface !== null) {
          destroyAfterSubmit.push(copied.replacedSurface);
        }
        releaseAfterSubmit.push({
          info: acquiredSurface,
          workflow: frame.input.workflow,
          importedTexture: copied.importedTexture,
        });
        this.importedRevision = acquiredSurface.revision;
        this.lastDrawError = "";
        this.lastAcquireFailureEventKey = "";
        if (this.lastImportedEventRevision !== acquiredSurface.revision) {
          this.lastImportedEventRevision = acquiredSurface.revision;
          this.emitHostEvent({
            workflow: frame.input.workflow,
            intent: "workspace.imported",
            surfaceId: acquiredSurface.surfaceId,
            revision: acquiredSurface.revision,
            operation: imported.operation,
            resultCode: "ok",
            resultDetail: "copied to renderer-owned texture",
          });
        }
        blockedReason = null;
      }
    }

    const surface = this.currentSurface;
    return {
      captureView: surface?.view ?? null,
      captureSource: surface === null ? "blocked" : "bridge",
      blockedReason,
      surfaceInfo: surface?.info ?? null,
      releaseAfterSubmit,
      destroyAfterSubmit,
    };
  }

  private retainImportedSurfaceTexture(
    importedTexture: GPUTexture,
    surface: WorkspaceSurfaceInfo,
    workflow: WorkspaceRenderFrame["input"]["workflow"],
  ): {
    retainedSurface: RetainedWorkspaceSurface;
    replacedSurface: RetainedWorkspaceSurface | null;
    importedTexture: GPUTexture;
  } {
    const current = this.currentSurface;
    if (current !== null && this.retainedSurfaceMatches(current, surface)) {
      current.info = surface;
      current.workflow = workflow;
      return {
        retainedSurface: current,
        replacedSurface: null,
        importedTexture,
      };
    }

    const texture = this.device!.createTexture({
      label: `workspace-retained-surface-${surface.surfaceId}`,
      size: {
        width: surface.width,
        height: surface.height,
        depthOrArrayLayers: 1,
      },
      format: surface.textureFormat as GPUTextureFormat,
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });
    const retainedSurface: RetainedWorkspaceSurface = {
      info: surface,
      texture,
      view: texture.createView(),
      workflow,
    };
    this.currentSurface = retainedSurface;
    return {
      retainedSurface,
      replacedSurface: current,
      importedTexture,
    };
  }

  private retainedSurfaceMatches(
    retained: RetainedWorkspaceSurface,
    surface: WorkspaceSurfaceInfo,
  ): boolean {
    return (
      retained.info.surfaceId === surface.surfaceId &&
      retained.info.width === surface.width &&
      retained.info.height === surface.height &&
      retained.info.textureFormat === surface.textureFormat
    );
  }

  private copyPendingImports(
    encoder: GPUCommandEncoder,
    releases: readonly PendingWorkspaceSurfaceRelease[],
    destroyedRetainedSurfaces: readonly RetainedWorkspaceSurface[],
  ): void {
    void destroyedRetainedSurfaces;
    for (const release of releases) {
      const retained = this.currentSurface;
      if (
        retained === null ||
        retained.info.surfaceId !== release.info.surfaceId ||
        retained.info.revision !== release.info.revision
      ) {
        continue;
      }
      encoder.copyTextureToTexture(
        { texture: release.importedTexture },
        { texture: retained.texture },
        {
          width: release.info.width,
          height: release.info.height,
          depthOrArrayLayers: 1,
        },
      );
    }
  }

  private captureBindGroupForView(textureView: GPUTextureView): GPUBindGroup {
    if (
      this.captureBindGroup !== null &&
      this.captureBindGroupView === textureView
    ) {
      return this.captureBindGroup;
    }
    this.captureBindGroup = this.device!.createBindGroup({
      label: "workspace-capture-bind-group",
      layout: this.capturePipeline!.getBindGroupLayout(0),
      entries: [
        {
          binding: 0,
          resource: {
            buffer: this.captureUniformBuffer!,
          },
        },
        {
          binding: 1,
          resource: textureView,
        },
        {
          binding: 2,
          resource: this.sampler!,
        },
      ],
    });
    this.captureBindGroupView = textureView;
    return this.captureBindGroup;
  }

  private emitAcquireFailure(
    frame: WorkspaceRenderFrame,
    acquired: ReturnType<typeof acquireWorkspaceSurfaceResult>,
  ): void {
    const resultCode =
      acquired.resultCode ?? acquired.blockedReason ?? "no_published_live_surface";
    if (resultCode !== "no_published_live_surface") {
      return;
    }
    const resultDetail =
      acquired.resultDetail.length > 0
        ? acquired.resultDetail
        : "no published live workspace surface";
    const eventKey = `${resultCode}\n${resultDetail}`;
    if (this.lastAcquireFailureEventKey === eventKey) {
      return;
    }
    this.lastAcquireFailureEventKey = eventKey;
    this.emitHostEvent({
      workflow: frame.input.workflow,
      intent: "workspace.acquire_failed",
      surfaceId: "workspace",
      revision: "",
      operation: acquired.operation,
      resultCode,
      resultDetail,
    });
  }

  private completeSubmittedSurfaceAfterSubmittedWork(
    surface: PendingWorkspaceSurfaceRelease,
    releaseCause: string,
  ): void {
    const device = this.device;
    this.releasePendingRevision = surface.info.revision;
    if (device === null) {
      this.emitSubmittedSurfaceDrawn(surface.workflow, surface.info);
      this.releasePublishedSurface(surface, releaseCause);
      return;
    }
    void device.queue.onSubmittedWorkDone().then(
      () => {
        const failureCause = this.takeSurfaceFailureCause(surface.info);
        if (failureCause === null) {
          this.emitSubmittedSurfaceDrawn(surface.workflow, surface.info);
          this.releasePublishedSurface(surface, releaseCause);
          return;
        }
        this.releasePublishedSurface(surface, failureCause);
      },
      (error) => {
        const resultDetail = errorTextFromUnknown(error);
        this.lastDrawError = resultDetail;
        this.markSurfaceFailure(surface.info, `draw_failed:${resultDetail}`);
        this.emitHostEvent({
          workflow: surface.workflow,
          intent: "workspace.import_failed",
          surfaceId: surface.info.surfaceId,
          revision: surface.info.revision,
          operation: "drawSurface",
          resultCode: "webgpu_submit_failed",
          resultDetail,
        });
        this.releasePublishedSurface(
          surface,
          this.takeSurfaceFailureCause(surface.info) ?? `draw_failed:${resultDetail}`,
        );
      },
    );
  }

  private emitSubmittedSurfaceDrawn(
    workflow: WorkspaceRenderFrame["input"]["workflow"],
    surface: WorkspaceSurfaceInfo,
  ): void {
    this.drawnRevision = surface.revision;
    if (this.lastDrawnEventRevision === surface.revision) {
      return;
    }
    this.lastDrawnEventRevision = surface.revision;
    this.emitHostEvent({
      workflow,
      intent: "workspace.drawn",
      surfaceId: surface.surfaceId,
      revision: surface.revision,
      operation: "drawSurface",
      resultCode: "ok",
      resultDetail: "",
    });
  }

  private releasePublishedSurface(
    surface: PendingWorkspaceSurfaceRelease,
    releaseCause: string,
  ): void {
    this.emitReleaseResult(
      surface.workflow,
      surface.info,
      releaseWorkspaceSurface(surface.info),
      releaseCause,
    );
  }

  private destroyRetainedSurfaceAfterSubmittedWork(surface: RetainedWorkspaceSurface): void {
    const device = this.device;
    if (device === null) {
      surface.texture.destroy();
      return;
    }
    void device.queue.onSubmittedWorkDone().then(
      () => surface.texture.destroy(),
      () => surface.texture.destroy(),
    );
  }

  private releaseCurrentSurface(cause = "dispose"): void {
    if (this.currentSurface === null) {
      return;
    }
    const surface = this.currentSurface;
    this.currentSurface = null;
    void cause;
    surface.texture.destroy();
  }

  private releaseCurrentSurfaceForDeviceLoss(cause: string): void {
    if (this.currentSurface === null) {
      return;
    }
    const surface = this.currentSurface;
    this.currentSurface = null;
    this.captureBindGroup = null;
    this.captureBindGroupView = null;
    this.importedRevision = "";
    this.submittedRevision = "";
    this.drawnRevision = "";
    this.lastImportedEventRevision = "";
    this.lastDrawnEventRevision = "";
    this.markSurfaceFailure(surface.info, cause);
    this.markReleaseCause(surface.info, cause);
    this.releasePendingRevision = surface.info.revision;
    surface.texture.destroy();
    this.emitReleaseResult(
      surface.workflow,
      surface.info,
      releaseWorkspaceRendererTexture(surface.info),
      this.takeReleaseCause(surface.info),
    );
  }

  private releaseCauseKey(
    surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision">,
  ): string {
    return `${surface.surfaceId}\n${surface.revision}`;
  }

  private markReleaseCause(
    surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision"> | null,
    cause: string,
  ): void {
    if (surface === null) {
      return;
    }
    this.releasePendingCauses.set(this.releaseCauseKey(surface), cause);
  }

  private markSurfaceFailure(
    surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision"> | null,
    cause: string,
  ): void {
    if (surface === null || cause.length === 0) {
      return;
    }
    this.surfaceFailureCauses.set(this.releaseCauseKey(surface), cause);
  }

  private takeSurfaceFailureCause(
    surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision">,
  ): string | null {
    const key = this.releaseCauseKey(surface);
    const cause = this.surfaceFailureCauses.get(key) ?? null;
    this.surfaceFailureCauses.delete(key);
    return cause;
  }

  private takeReleaseCause(
    surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision">,
  ): string {
    const key = this.releaseCauseKey(surface);
    const cause = this.releasePendingCauses.get(key) ?? "unknown";
    this.releasePendingCauses.delete(key);
    return cause;
  }

  private emitReleaseResult(
    workflow: WorkspaceRenderFrame["input"]["workflow"],
    surface: Pick<WorkspaceSurfaceInfo, "surfaceId" | "revision">,
    result: WorkspaceSurfaceReleaseResult,
    releaseCause: string,
  ): void {
    if (this.releasePendingRevision === surface.revision) {
      this.releasePendingRevision = "";
    }
    if (!result.released) {
      this.lastDrawError =
        result.resultDetail.length > 0
          ? result.resultDetail
          : "workspace surface release rejected";
    }
    this.emitHostEvent({
      workflow,
      intent: result.released ? "workspace.release_complete" : "workspace.release_rejected",
      surfaceId: surface.surfaceId,
      revision: surface.revision,
      operation: result.operation,
      resultCode: result.resultCode ?? (result.released ? "ok" : "release_failure"),
      resultDetail: result.released
        ? `${result.resultDetail}${result.resultDetail.length > 0 ? "; " : ""}cause=${releaseCause}`
        : result.resultDetail,
    });
  }

  private emitHostEvent(event: WorkspaceRendererHostEvent): void {
    try {
      this.hostEventSink(event);
    } catch (error) {
      this.lastDrawError = errorTextFromUnknown(error);
    }
  }

  private emitWebGpuDiagnosticEvent(
    operation: string,
    resultCode: WorkspaceGpuBridgeResultCode,
    resultDetail: string,
  ): void {
    const surface = this.currentSurface?.info;
    if (surface !== undefined) {
      this.markSurfaceFailure(surface, resultDetail);
    }
    const eventKey = `${operation}\n${surface?.revision ?? ""}\n${resultCode}\n${resultDetail}`;
    if (this.lastWebGpuDiagnosticEventKey === eventKey) {
      return;
    }
    this.lastWebGpuDiagnosticEventKey = eventKey;
    this.emitHostEvent({
      workflow: this.currentSurface?.workflow ?? this.activeWorkflow,
      intent: "workspace.import_failed",
      surfaceId: surface?.surfaceId ?? "workspace",
      revision: surface?.revision ?? "",
      operation,
      resultCode,
      resultDetail,
    });
  }

  private displayStartupState(
    captureSource: WorkspaceRendererCaptureSource,
    surfaceInfo: WorkspaceSurfaceInfo | null,
  ): WorkspaceRendererDisplayStartupState {
    if (captureSource === "native") {
      return "native presented";
    }
    if (surfaceInfo === null) {
      return captureSource === "unsupported" ? "idle" : "capture running";
    }
    if (
      this.submittedRevision === surfaceInfo.revision ||
      this.drawnRevision === surfaceInfo.revision
    ) {
      return "draw submitted";
    }
    if (this.importedRevision === surfaceInfo.revision) {
      return "texture imported";
    }
    return "surface published";
  }

  private renderStats(
    captureSource: WorkspaceRendererCaptureSource,
    surfaceInfo: WorkspaceSurfaceInfo | null,
    statusText: string,
  ): WorkspaceWebGpuRenderStats {
    return {
      captureSource,
      overlayDrawCount: 0,
      surfaceInfo,
      statusText,
      acquiredRevision: this.acquiredRevision,
      importedRevision: this.importedRevision,
      submittedRevision: this.submittedRevision,
      drawnRevision: this.drawnRevision,
      releasePendingRevision: this.releasePendingRevision,
      lastDrawError: this.lastDrawError,
      displayStartupState: this.displayStartupState(captureSource, surfaceInfo),
    };
  }

  private statusTextFromRender(
    captureSource: WorkspaceRendererCaptureSource,
    blockedReason: CaptureBlockedReason | null,
    surfaceInfo: WorkspaceSurfaceInfo | null,
  ): string {
    if (captureSource === "native") {
      return "native-presented live workspace";
    }
    if (captureSource === "unsupported") {
      return this.lastStatus;
    }
    if (captureSource === "bridge" && surfaceInfo !== null) {
      return `webgpu ready · surface ${surfaceInfo.revision}`;
    }
    switch (blockedReason) {
      case "allocation_failure":
        return "webgpu blocked · workspace allocation failed";
      case "gpu_bridge_missing":
        return "webgpu blocked · workspace gpu bridge missing";
      case "invalid_dmabuf_metadata":
        return "webgpu blocked · invalid dma-buf metadata";
      case "unsupported_modifier":
        return "webgpu blocked · unsupported dma-buf modifier";
      case "missing_sync_metadata":
        return "webgpu blocked · missing sync metadata";
      case "shared_image_creation_failure":
        return "webgpu blocked · shared image creation failed";
      case "software_shared_image_rejected":
        return "webgpu blocked · software shared image rejected";
      case "shared_image_export_rejected":
        return "webgpu blocked · shared image export rejected";
      case "dawn_import_failure":
        return "webgpu blocked · dawn texture import failed";
      case "renderer_import_rejected":
        return "webgpu blocked · renderer texture import rejected";
      case "release_failure":
        return "webgpu blocked · workspace surface release failed";
      case "no_published_live_surface":
      default:
        return "webgpu blocked · no published live surface";
    }
  }
}
