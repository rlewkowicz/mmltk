class FakeImageData {
  readonly data: Uint8ClampedArray;
  readonly width: number;
  readonly height: number;

  constructor(
    dataOrWidth: Uint8ClampedArray | number,
    width?: number,
    height?: number,
  ) {
    if (dataOrWidth instanceof Uint8ClampedArray) {
      this.data = dataOrWidth;
      this.width = width ?? 0;
      this.height = height ?? 0;
      return;
    }

    this.width = dataOrWidth;
    this.height = width ?? 0;
    this.data = new Uint8ClampedArray(this.width * this.height * 4);
  }
}

class FakeCanvasContext2D {
  lineWidth = 0;
  fillStyle: string | null = null;
  strokeStyle: string | null = null;
  private imageData = new FakeImageData(1, 1);

  save(): void {}

  restore(): void {}

  setTransform(): void {}

  clearRect(): void {}

  setLineDash(): void {}

  beginPath(): void {}

  rect(): void {}

  fill(): void {}

  stroke(): void {}

  fillRect(): void {}

  strokeRect(): void {}

  moveTo(): void {}

  lineTo(): void {}

  closePath(): void {}

  arc(): void {}

  createImageData(width: number, height: number): FakeImageData {
    return new FakeImageData(width, height);
  }

  putImageData(imageData: FakeImageData): void {
    this.imageData = imageData;
  }

  getImageData(): { data: Uint8ClampedArray } {
    return {
      data: this.imageData.data,
    };
  }
}

class FakeOffscreenCanvas {
  width: number;
  height: number;
  private readonly context = new FakeCanvasContext2D();

  constructor(width: number, height: number) {
    this.width = width;
    this.height = height;
  }

  getContext(type?: string): FakeCanvasContext2D | null {
    if (type === undefined || type === "2d") {
      return this.context;
    }
    return null;
  }
}

export function installFakeCanvasSupport(): () => void {
  const previousOffscreenCanvas = Reflect.get(globalThis, "OffscreenCanvas");
  const previousImageData = Reflect.get(globalThis, "ImageData");

  Reflect.set(globalThis, "OffscreenCanvas", FakeOffscreenCanvas);
  Reflect.set(globalThis, "ImageData", FakeImageData);

  return () => {
    if (previousOffscreenCanvas === undefined) {
      Reflect.deleteProperty(globalThis, "OffscreenCanvas");
    } else {
      Reflect.set(globalThis, "OffscreenCanvas", previousOffscreenCanvas);
    }

    if (previousImageData === undefined) {
      Reflect.deleteProperty(globalThis, "ImageData");
    } else {
      Reflect.set(globalThis, "ImageData", previousImageData);
    }
  };
}
