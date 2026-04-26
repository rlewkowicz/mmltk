export {};

declare global {
  type GPUTextureFormat = string;
  type GPUColor = { r: number; g: number; b: number; a: number };

  interface GPUDeviceLostInfo {
    message: string;
  }

  interface GPUTexture {
    createView(): GPUTextureView;
    destroy(): void;
  }

  interface GPUTextureView {}

  interface GPUBuffer {
    destroy(): void;
  }

  interface GPUSampler {}

  interface GPUBindGroup {}

  interface GPUBindGroupLayout {}

  interface GPUCommandBuffer {}

  interface GPURenderPassEncoder {
    setPipeline(pipeline: GPURenderPipeline): void;
    setBindGroup(index: number, bindGroup: GPUBindGroup): void;
    setVertexBuffer(slot: number, buffer: GPUBuffer, offset?: number, size?: number): void;
    draw(vertexCount: number, instanceCount?: number): void;
    end(): void;
  }

  interface GPUCommandEncoder {
    beginRenderPass(descriptor: unknown): GPURenderPassEncoder;
    finish(): GPUCommandBuffer;
  }

  interface GPUQueue {
    writeBuffer(
      buffer: GPUBuffer,
      bufferOffset: number,
      data: Float32Array,
      dataOffset?: number,
      size?: number,
    ): void;
    writeTexture(
      destination: unknown,
      data: Uint8Array<ArrayBuffer>,
      dataLayout: unknown,
      size: unknown,
    ): void;
    copyExternalImageToTexture(
      source: unknown,
      destination: unknown,
      size: unknown,
    ): void;
    submit(commandBuffers: GPUCommandBuffer[]): void;
    onSubmittedWorkDone(): Promise<undefined>;
  }

  interface GPUShaderModule {
    getCompilationInfo(): Promise<GPUCompilationInfo>;
  }

  interface GPUCompilationMessage {
    type: string;
  }

  interface GPUCompilationInfo {
    messages: GPUCompilationMessage[];
  }

  interface GPURenderPipeline {
    getBindGroupLayout(index: number): GPUBindGroupLayout;
  }

  interface GPUDevice {
    lost: Promise<GPUDeviceLostInfo>;
    queue: GPUQueue;
    createShaderModule(descriptor: unknown): GPUShaderModule;
    createRenderPipeline(descriptor: unknown): GPURenderPipeline;
    createBuffer(descriptor: unknown): GPUBuffer;
    createSampler(descriptor: unknown): GPUSampler;
    createTexture(descriptor: unknown): GPUTexture;
    createBindGroup(descriptor: unknown): GPUBindGroup;
    createCommandEncoder(descriptor?: unknown): GPUCommandEncoder;
  }

  interface GPUAdapter {
    requestDevice(descriptor?: unknown): Promise<GPUDevice>;
  }

  interface GPUCanvasContext {
    configure(configuration: unknown): void;
    unconfigure(): void;
    getCurrentTexture(): GPUTexture;
  }

  interface HTMLCanvasElement {
    getContext(contextId: "webgpu"): GPUCanvasContext | null;
  }

  interface Navigator {
    gpu: {
      requestAdapter(): Promise<GPUAdapter | null>;
      getPreferredCanvasFormat(): GPUTextureFormat;
    };
  }

  const GPUBufferUsage: {
    UNIFORM: number;
    COPY_DST: number;
    VERTEX: number;
  };

  const GPUTextureUsage: {
    TEXTURE_BINDING: number;
    COPY_DST: number;
  };
}

if (!("GPUBufferUsage" in globalThis)) {
  Object.defineProperty(globalThis, "GPUBufferUsage", {
    value: {
      UNIFORM: 1 << 0,
      COPY_DST: 1 << 1,
      VERTEX: 1 << 2,
    },
    configurable: true,
    writable: true,
  });
}

if (!("GPUTextureUsage" in globalThis)) {
  Object.defineProperty(globalThis, "GPUTextureUsage", {
    value: {
      TEXTURE_BINDING: 1 << 0,
      COPY_DST: 1 << 1,
    },
    configurable: true,
    writable: true,
  });
}
