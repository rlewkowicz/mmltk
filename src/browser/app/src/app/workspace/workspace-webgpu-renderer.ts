import type { WorkspaceRenderFrame } from "../../workspace_renderer";
import type { WorkspaceSurfaceInfo } from "../../host_api";
import {
  acquireWorkspaceSurface,
  importWorkspaceSurfaceTexture,
  releaseWorkspaceSurface,
  workspaceGpuBridgeAvailable,
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
type CaptureBlockedReason =
  | "gpu_bridge_missing"
  | "workspace_surface_unavailable"
  | "workspace_surface_import_failed";

interface ImportedWorkspaceSurface {
  info: WorkspaceSurfaceInfo;
  texture: GPUTexture;
  view: GPUTextureView;
}

export interface WorkspaceWebGpuRenderStats {
  captureSource: CaptureSource;
  overlayDrawCount: number;
  surfaceInfo: WorkspaceSurfaceInfo | null;
  statusText: string;
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
  private currentSurface: ImportedWorkspaceSurface | null = null;
  private initialization: Promise<boolean> | null = null;
  private disposed = false;
  private lastStatus = "webgpu pending";

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
    if (this.disposed) {
      return {
        captureSource: "unsupported",
        overlayDrawCount: 0,
        surfaceInfo: null,
        statusText: "webgpu disposed",
      };
    }
    const ready = await this.ensureReady();
    if (!ready || this.device === null || this.context === null) {
      return {
        captureSource: "unsupported",
        overlayDrawCount: 0,
        surfaceInfo: null,
        statusText: this.lastStatus,
      };
    }

    const {
      captureSource,
      captureView,
      blockedReason,
      surfaceInfo,
      releaseAfterSubmit,
    } = this.prepareCaptureSurface();
    if (captureSource === "bridge") {
      this.writeCaptureUniforms(frame);
    }
    const currentTextureView = this.context.getCurrentTexture().createView();

    const encoder = this.device.createCommandEncoder({
      label: "workspace-webgpu-renderer",
    });
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
    if (releaseAfterSubmit !== null) {
      const releaseSurface = releaseAfterSubmit;
      void this.device.queue.onSubmittedWorkDone().then(
        () => this.releaseImportedSurface(releaseSurface),
        () => this.releaseImportedSurface(releaseSurface),
      );
    }

    const statusText = this.statusTextFromRender(
      captureSource,
      blockedReason,
      surfaceInfo,
    );
    this.lastStatus = statusText;
    return {
      captureSource,
      overlayDrawCount: 0,
      surfaceInfo,
      statusText,
    };
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
    device.lost.then((info) => {
      if (this.disposed) {
        return;
      }
      this.releaseCurrentSurface();
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

  private prepareCaptureSurface(): {
    captureView: GPUTextureView | null;
    captureSource: CaptureSource;
    blockedReason: CaptureBlockedReason | null;
    surfaceInfo: WorkspaceSurfaceInfo | null;
    releaseAfterSubmit: ImportedWorkspaceSurface | null;
  } {
    const bridgeAvailable = workspaceGpuBridgeAvailable();
    const acquiredSurface = acquireWorkspaceSurface();
    let releaseAfterSubmit: ImportedWorkspaceSurface | null = null;
    let blockedReason: CaptureBlockedReason | null = null;

    if (acquiredSurface === null) {
      if (this.currentSurface !== null) {
        releaseAfterSubmit = this.currentSurface;
        this.currentSurface = null;
      }
      blockedReason = bridgeAvailable
        ? "workspace_surface_unavailable"
        : "gpu_bridge_missing";
    } else if (this.currentSurface?.info.revision !== acquiredSurface.revision) {
      const importedTexture =
        this.device === null
          ? null
          : importWorkspaceSurfaceTexture(this.device, acquiredSurface);
      if (importedTexture === null) {
        releaseWorkspaceSurface(acquiredSurface);
        if (this.currentSurface !== null) {
          releaseAfterSubmit = this.currentSurface;
          this.currentSurface = null;
        }
        blockedReason = "workspace_surface_import_failed";
      } else {
        releaseAfterSubmit = this.currentSurface;
        this.currentSurface = {
          info: acquiredSurface,
          texture: importedTexture,
          view: importedTexture.createView(),
        };
      }
    }

    const surface = this.currentSurface;
    return {
      captureView: surface?.view ?? null,
      captureSource: surface === null ? "blocked" : "bridge",
      blockedReason,
      surfaceInfo: surface?.info ?? null,
      releaseAfterSubmit,
    };
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

  private releaseImportedSurface(surface: ImportedWorkspaceSurface): void {
    releaseWorkspaceSurface(surface.info);
  }

  private releaseCurrentSurface(): void {
    if (this.currentSurface === null) {
      return;
    }
    this.releaseImportedSurface(this.currentSurface);
    this.currentSurface = null;
  }

  private statusTextFromRender(
    captureSource: CaptureSource,
    blockedReason: CaptureBlockedReason | null,
    surfaceInfo: WorkspaceSurfaceInfo | null,
  ): string {
    if (captureSource === "unsupported") {
      return this.lastStatus;
    }
    if (captureSource === "bridge" && surfaceInfo !== null) {
      return `webgpu ready · surface ${surfaceInfo.revision}`;
    }
    switch (blockedReason) {
      case "gpu_bridge_missing":
        return "webgpu blocked · workspace gpu bridge missing";
      case "workspace_surface_import_failed":
        return "webgpu blocked · workspace surface import failed";
      case "workspace_surface_unavailable":
      default:
        return "webgpu blocked · workspace surface unavailable";
    }
  }
}
