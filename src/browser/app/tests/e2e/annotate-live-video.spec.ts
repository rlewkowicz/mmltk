import { existsSync } from "node:fs";
import { open, readdir, readFile } from "node:fs/promises";
import * as path from "node:path";
import { inflateSync } from "node:zlib";
import { expect, test, type Browser, type Locator, type Page, type TestInfo } from "@playwright/test";

import { connectToMmltkCef } from "./support/cef-cdp";
import {
  launchMmltkGui,
  readIntentTraceRecords,
  readMmltkGuiLogFiles,
  waitForIntentTraceRecord,
  type IntentTraceRecord,
  type MmltkGuiProcess,
} from "./support/mmltk-gui-process";

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function payloadSetsAnnotateVideoStream(payload: unknown): boolean {
  if (!isRecord(payload)) {
    return false;
  }
  const patch = payload["patch"];
  if (!isRecord(patch)) {
    return false;
  }
  const workflows = patch["workflows"];
  if (!isRecord(workflows) || !isRecord(workflows["annotate"])) {
    return false;
  }
  const source = workflows["annotate"]["source"];
  return (
    isRecord(source) &&
    (source["kind"] === "video_stream" || source["kind"] === 3 || source["kind"] === "3")
  );
}

function compactJson(value: unknown): string {
  return JSON.stringify(value, null, 2) ?? String(value);
}

function tailText(text: string, maxLength = 12_000): string {
  return text.length <= maxLength ? text : text.slice(text.length - maxLength);
}

async function readTextFileIfExists(filePath: string): Promise<string> {
  if (!existsSync(filePath)) {
    return "";
  }
  return await readFile(filePath, "utf8");
}

async function readTextFileTailIfExists(
  filePath: string,
  maxBytes = 192 * 1024,
): Promise<string> {
  if (!existsSync(filePath)) {
    return "";
  }
  const file = await open(filePath, "r");
  try {
    const stat = await file.stat();
    const bytesToRead = Math.min(maxBytes, stat.size);
    if (bytesToRead <= 0) {
      return "";
    }
    const buffer = Buffer.allocUnsafe(bytesToRead);
    await file.read(buffer, 0, bytesToRead, stat.size - bytesToRead);
    return buffer.toString("utf8");
  } finally {
    await file.close();
  }
}

function postStartRendererLoopRecords(
  records: readonly IntentTraceRecord[],
  startIndex: number,
): IntentTraceRecord[] {
  return records.filter(
    (record, index) =>
      index > startIndex &&
      (record.intent === "workspace.drawn" ||
        record.intent === "workspace.imported" ||
        record.intent === "workspace.import_failed" ||
        record.intent === "workspace.acquire_failed" ||
        record.intent === "workspace.release_complete" ||
        record.intent === "workspace.release_rejected"),
  );
}

const kForbiddenRuntimeFailurePattern =
  /browser live publication blocked|no_published_live_surface|retry exhausted|workspace renderer event:.*importTexture|workspace renderer event:.*releaseSurface|webgpu_uncaptured_error|webgpu_device_lost|device_lost|GPUValidationError|Invalid Texture|Failed to fulfill the promise texture|SharedImage mailbox not found|SharedImageManager::ProduceSkia|SharedImageBackingFactory|AssociateMailbox|WebGPUSwapBufferProvider|ExternalVkImageDawnImageRepresentation|Received signal|--ozone-platform=wayland is not compatible with Vulkan/;

function extractNativePresentRevisionNumbers(logText: string): Set<number> {
  const revisions = new Set<number>();
  for (const match of logText.matchAll(/workspace GPU bridge native present:.*?\brevision=(\d+)/g)) {
    const revision = Number(match[1] ?? 0);
    if (Number.isSafeInteger(revision) && revision > 0) {
      revisions.add(revision);
    }
  }
  for (const match of logText.matchAll(/workspace GPU bridge presented front slot .*?\brevision (\d+)/g)) {
    const revision = Number(match[1] ?? 0);
    if (Number.isSafeInteger(revision) && revision > 0) {
      revisions.add(revision);
    }
  }
  return revisions;
}

function extractNativePresentRevisions(logText: string): Set<string> {
  const revisions = new Set<string>();
  for (const revision of extractNativePresentRevisionNumbers(logText)) {
    revisions.add(String(revision));
  }
  return revisions;
}

function maxNativePresentRevision(logText: string): number {
  let maxRevision = 0;
  for (const revision of extractNativePresentRevisionNumbers(logText)) {
    maxRevision = Math.max(maxRevision, revision);
  }
  return maxRevision;
}

function countNativePresentRevisionsAfter(logText: string, baselineRevision: number): number {
  let count = 0;
  for (const revision of extractNativePresentRevisionNumbers(logText)) {
    if (revision > baselineRevision) {
      count += 1;
    }
  }
  return count;
}

async function waitForNativePresentEvidence(
  gui: MmltkGuiProcess,
  startIndex: number,
  minRevisionCount: number,
): Promise<string> {
  const deadline = Date.now() + 60_000;
  while (Date.now() < deadline) {
    const records = await readIntentTraceRecords(gui.intentTracePath);
    const rendererLoopRecord = postStartRendererLoopRecords(records, startIndex)[0];
    if (rendererLoopRecord !== undefined) {
      throw new Error(
        [
          "Live annotate used the retired browser renderer import/release loop after annotate.live.start.",
          `Renderer-loop record: ${compactJson(rendererLoopRecord)}`,
        ].join("\n\n"),
      );
    }
    const logText = `${await readTextFileTailIfExists(gui.browserHostLogPath)}\n${gui.output()}`;
    const configured =
      /workspace GPU bridge native swapchain configured|workspace GPU bridge configured swapchain/.test(
        logText,
      );
    const revisions = extractNativePresentRevisions(logText);
    if (configured && revisions.size >= minRevisionCount) {
      return logText;
    }
    await new Promise((resolve) => {
      setTimeout(resolve, 150);
    });
  }
  const records = await readIntentTraceRecords(gui.intentTracePath);
  const logText = `${await readTextFileTailIfExists(gui.browserHostLogPath)}\n${gui.output()}`;
  throw new Error(
    [
      `Timed out waiting for ${minRevisionCount} native workspace presents after annotate.live.start.`,
      `Intent trace tail: ${compactJson(records.slice(-20))}`,
      `Log file tail:\n${tailText(logText)}`,
      `GUI output tail:\n${tailText(gui.output())}`,
    ].join("\n\n"),
  );
}

function paethPredictor(left: number, above: number, upperLeft: number): number {
  const estimate = left + above - upperLeft;
  const leftDistance = Math.abs(estimate - left);
  const aboveDistance = Math.abs(estimate - above);
  const upperLeftDistance = Math.abs(estimate - upperLeft);
  if (leftDistance <= aboveDistance && leftDistance <= upperLeftDistance) {
    return left;
  }
  return aboveDistance <= upperLeftDistance ? above : upperLeft;
}

function analyzePngPixels(buffer: Buffer): {
  meanLuma: number;
  brightPixels: number;
  pixelHash: number;
} {
  const signature = "89504e470d0a1a0a";
  if (buffer.subarray(0, 8).toString("hex") !== signature) {
    throw new Error("workspace screenshot was not a PNG image");
  }
  let offset = 8;
  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  const idat: Buffer[] = [];
  while (offset + 12 <= buffer.length) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.subarray(offset + 4, offset + 8).toString("ascii");
    const data = buffer.subarray(offset + 8, offset + 8 + length);
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8] ?? 0;
      colorType = data[9] ?? 0;
    } else if (type === "IDAT") {
      idat.push(Buffer.from(data));
    } else if (type === "IEND") {
      break;
    }
    offset += length + 12;
  }
  const bytesPerPixel = colorType === 6 ? 4 : colorType === 2 ? 3 : 0;
  if (width <= 0 || height <= 0 || bitDepth !== 8 || bytesPerPixel === 0) {
    throw new Error(
      `unsupported workspace screenshot PNG format: ${width}x${height} bitDepth=${bitDepth} colorType=${colorType}`,
    );
  }
  const rowBytes = width * bytesPerPixel;
  const inflated = inflateSync(Buffer.concat(idat));
  const previous = Buffer.alloc(rowBytes);
  const current = Buffer.alloc(rowBytes);
  let sourceOffset = 0;
  let lumaSum = 0;
  let brightPixels = 0;
  let pixelHash = 0x811c9dc5;
  for (let y = 0; y < height; y += 1) {
    const filter = inflated[sourceOffset] ?? 0;
    sourceOffset += 1;
    for (let x = 0; x < rowBytes; x += 1) {
      const raw = inflated[sourceOffset + x] ?? 0;
      const left = x >= bytesPerPixel ? current[x - bytesPerPixel] ?? 0 : 0;
      const above = previous[x] ?? 0;
      const upperLeft = x >= bytesPerPixel ? previous[x - bytesPerPixel] ?? 0 : 0;
      switch (filter) {
        case 0:
          current[x] = raw;
          break;
        case 1:
          current[x] = (raw + left) & 0xff;
          break;
        case 2:
          current[x] = (raw + above) & 0xff;
          break;
        case 3:
          current[x] = (raw + Math.floor((left + above) / 2)) & 0xff;
          break;
        case 4:
          current[x] = (raw + paethPredictor(left, above, upperLeft)) & 0xff;
          break;
        default:
          throw new Error(`unsupported workspace screenshot PNG filter ${filter}`);
      }
    }
    sourceOffset += rowBytes;
    for (let x = 0; x < rowBytes; x += bytesPerPixel) {
      const luma =
        0.2126 * (current[x] ?? 0) +
        0.7152 * (current[x + 1] ?? 0) +
        0.0722 * (current[x + 2] ?? 0);
      lumaSum += luma;
      if (luma > 24) {
        brightPixels += 1;
      }
      pixelHash ^= current[x] ?? 0;
      pixelHash = Math.imul(pixelHash, 0x01000193) >>> 0;
      pixelHash ^= current[x + 1] ?? 0;
      pixelHash = Math.imul(pixelHash, 0x01000193) >>> 0;
      pixelHash ^= current[x + 2] ?? 0;
      pixelHash = Math.imul(pixelHash, 0x01000193) >>> 0;
    }
    previous.set(current);
  }
  return {
    meanLuma: lumaSum / Math.max(1, width * height),
    brightPixels,
    pixelHash,
  };
}

async function expectVisibleNativeWorkspaceFramesAdvancing(
  page: Page,
  workspaceCanvas: Locator,
  gui: MmltkGuiProcess,
): Promise<void> {
  const baselineRevision = maxNativePresentRevision(
    await readTextFileTailIfExists(gui.browserHostLogPath),
  );
  await expect
    .poll(
      async () => {
        const bounds = await workspaceCanvas.boundingBox();
        if (bounds === null || bounds.width < 1 || bounds.height < 1) {
          return 0;
        }
        const screenshot = await page.screenshot({
          clip: {
            x: Math.floor(bounds.x),
            y: Math.floor(bounds.y),
            width: Math.max(1, Math.floor(bounds.width)),
            height: Math.max(1, Math.floor(bounds.height)),
          },
        });
        const stats = analyzePngPixels(screenshot);
        const visible = Math.max(stats.meanLuma, stats.brightPixels > 0 ? 25 : 0) > 12;
        const revisionsAfterBaseline = countNativePresentRevisionsAfter(
          await readTextFileTailIfExists(gui.browserHostLogPath),
          baselineRevision,
        );
        return visible && revisionsAfterBaseline >= 2 ? 1 : 0;
      },
      {
        timeout: 20_000,
        message:
          "Native-presented workspace screenshots should be visible while native present revisions advance.",
      },
    )
    .toBe(1);
}

async function bestEffortWithTimeout(
  label: string,
  timeoutMs: number,
  operation: () => Promise<void>,
  errors: string[],
): Promise<void> {
  let timeout: ReturnType<typeof setTimeout> | undefined;
  const completed = operation().catch((error: unknown) => {
    errors.push(`${label} failed: ${String(error)}`);
  });
  const timedOut = new Promise<"timeout">((resolve) => {
    timeout = setTimeout(() => resolve("timeout"), timeoutMs);
  });
  const result = await Promise.race([completed.then(() => "done" as const), timedOut]);
  if (timeout !== undefined) {
    clearTimeout(timeout);
  }
  if (result === "timeout") {
    errors.push(`${label} timed out after ${timeoutMs}ms`);
  }
}

async function delayWithAbort(timeoutMs: number, signal: AbortSignal): Promise<void> {
  if (signal.aborted) {
    return;
  }
  await new Promise<void>((resolve) => {
    const timeout = setTimeout(resolve, timeoutMs);
    signal.addEventListener(
      "abort",
      () => {
        clearTimeout(timeout);
        resolve();
      },
      { once: true },
    );
  });
}

function runtimeFailureMessage(source: string, text: string): string | null {
  const match = text.match(kForbiddenRuntimeFailurePattern);
  if (match === null) {
    return null;
  }
  return [
    "Live annotate runtime logs reported a failure before the E2E completed.",
    `Source: ${source}`,
    `Matched: ${match[0]}`,
    `Log tail:\n${tailText(text)}`,
  ].join("\n\n");
}

async function detectRuntimeLogFailure(gui: MmltkGuiProcess): Promise<string | null> {
  const hostLog = await readTextFileTailIfExists(gui.browserHostLogPath);
  const guiOutput = gui.output();
  const cefLog = await readTextFileTailIfExists(gui.cefLogPath);
  for (const [source, text] of [
    ["mmltk browser host log", hostLog],
    ["mmltk gui output", guiOutput],
    ["mmltk cef log", cefLog],
  ] as const) {
    const message = runtimeFailureMessage(source, text);
    if (message !== null) {
      return message;
    }
  }
  return null;
}

async function watchRuntimeLogFailures(
  gui: MmltkGuiProcess,
  signal: AbortSignal,
): Promise<void> {
  while (!signal.aborted) {
    const message = await detectRuntimeLogFailure(gui);
    if (message !== null) {
      throw new Error(message);
    }
    await delayWithAbort(150, signal);
  }
}

async function runWithRuntimeLogFailureWatch<T>(
  gui: MmltkGuiProcess,
  operation: () => Promise<T>,
): Promise<T> {
  const controller = new AbortController();
  const watcher = watchRuntimeLogFailures(gui, controller.signal);
  try {
    return await Promise.race([
      operation(),
      watcher.then(() => new Promise<T>(() => undefined)),
    ]);
  } finally {
    controller.abort();
    await watcher.catch(() => undefined);
  }
}

async function attachTextFile(
  testInfo: TestInfo,
  name: string,
  filePath: string,
): Promise<void> {
  if (!existsSync(filePath)) {
    return;
  }
  await testInfo.attach(name, {
    body: await readFile(filePath),
    contentType: "text/plain",
  });
}

async function attachGuiArtifacts(
  testInfo: TestInfo,
  gui: MmltkGuiProcess,
): Promise<void> {
  await testInfo.attach("mmltk gui artifact paths", {
    body: [
      `artifacts=${gui.artifactsDir}`,
      `log_dir=${gui.logDir}`,
      `browser_host_log=${gui.browserHostLogPath}`,
      `cef_log=${gui.cefLogPath}`,
      `gui_output=${gui.outputPath}`,
      `intent_trace=${gui.intentTracePath}`,
    ].join("\n"),
    contentType: "text/plain",
  });
  await testInfo.attach("mmltk gui output", {
    body: gui.output(),
    contentType: "text/plain",
  });
  if (existsSync(gui.logDir)) {
    const entries = await readdir(gui.logDir, { withFileTypes: true });
    for (const entry of entries.filter((item) => item.isFile()).sort((a, b) => a.name.localeCompare(b.name))) {
      await attachTextFile(testInfo, `mmltk log ${entry.name}`, path.join(gui.logDir, entry.name));
    }
  }
  await attachTextFile(testInfo, "mmltk intent trace", gui.intentTracePath);
  await attachTextFile(testInfo, "mmltk cef log", gui.cefLogPath);
}

test("starts live annotate from a video stream source in the CEF UI", async ({}, testInfo) => {
  const gui: MmltkGuiProcess = await launchMmltkGui({
    workerIndex: testInfo.workerIndex,
  });
  const browserRef: { current: Browser | null } = { current: null };

  try {
    await runWithRuntimeLogFailureWatch(gui, async () => {
      const connected = await connectToMmltkCef(gui.cdpPort, 90_000, gui.output);
      browserRef.current = connected.browser;
      const page = connected.page;

      await page
        .getByRole("navigation", { name: "Workflow selection" })
        .getByRole("button", { name: "Annotate", exact: true })
        .click();
      const workflowMain = page.locator("main.workflow-main");
      await expect(workflowMain).toHaveAttribute("data-workflow", "annotate");

      await page.getByLabel("Source Kind").selectOption("video_stream");
      const workspaceCanvas = page.locator("canvas#workspace-canvas");
      await expect(workspaceCanvas).toBeVisible();
      await expect
        .poll(
          async () =>
            await workspaceCanvas.evaluate((canvas) =>
              Math.round(canvas.getBoundingClientRect().height),
            ),
          {
            timeout: 5_000,
            message: "Workspace canvas should have a stable rendered height before live annotate starts.",
          },
        )
        .toBeGreaterThan(300);

      const primaryWorkspace = page.locator("app-workspace-host");
      const primaryWorkspaceText = await primaryWorkspace.innerText({ timeout: 5_000 });
      expect(primaryWorkspaceText).not.toContain("Workspace Path");
      expect(primaryWorkspaceText).not.toContain("Overlay");
      expect(primaryWorkspaceText).not.toContain("Render");
      expect(primaryWorkspaceText).not.toContain("Canvas");
      expect(primaryWorkspaceText).not.toContain("bridge pending");
      expect(primaryWorkspaceText).not.toContain("webgpu adapter unavailable");
      expect(primaryWorkspaceText).not.toContain("workspace surface bridge pending");
      expect(primaryWorkspaceText).not.toContain("webgpu canvas overlay");

      await page.getByRole("button", { name: "Start Live Annotate" }).first().click();

      const sourceRecord = await waitForIntentTraceRecord(
        gui.intentTracePath,
        (record) => record.intent === "settings.update" && payloadSetsAnnotateVideoStream(record.payload),
      );
      const startRecord = await waitForIntentTraceRecord(
        gui.intentTracePath,
        (record) => record.workflow === "annotate" && record.intent === "annotate.live.start",
      );
      const records = await readIntentTraceRecords(gui.intentTracePath);
      const sourceIndex = records.findIndex(
        (record) => record.request_id === sourceRecord.request_id,
      );
      const startIndex = records.findIndex(
        (record) => record.request_id === startRecord.request_id,
      );
      expect(sourceIndex).toBeGreaterThanOrEqual(0);
      expect(startIndex).toBeGreaterThanOrEqual(0);
      expect(sourceIndex).toBeLessThan(startIndex);

      const nativePresentLog = await waitForNativePresentEvidence(gui, startIndex, 2);
      await expectVisibleNativeWorkspaceFramesAdvancing(page, workspaceCanvas, gui);
      await page.waitForTimeout(1_500);
      const recordsAfterHold = await readIntentTraceRecords(gui.intentTracePath);

      const hostLog = await readMmltkGuiLogFiles(gui);
      const cefLog = await readTextFileIfExists(gui.cefLogPath);
      const combinedHostLog = `${hostLog}\n${gui.output()}`;
      expect(nativePresentLog).toMatch(
        /workspace GPU bridge native swapchain configured|workspace GPU bridge configured swapchain/,
      );
      expect(extractNativePresentRevisions(nativePresentLog).size).toBeGreaterThanOrEqual(2);
      expect(postStartRendererLoopRecords(recordsAfterHold, startIndex)).toEqual([]);
      expect(combinedHostLog).not.toMatch(kForbiddenRuntimeFailurePattern);
      expect(gui.output()).not.toMatch(kForbiddenRuntimeFailurePattern);
      expect(cefLog).not.toMatch(kForbiddenRuntimeFailurePattern);
      expect(combinedHostLog).not.toMatch(/filesystem error|terminate called|Received signal|Not implemented reached/);
      expect(gui.output()).not.toMatch(/filesystem error|terminate called|Received signal|Not implemented reached/);
      expect(cefLog).not.toMatch(/GetNativeViewAccessible|browser live publication blocked|filesystem error/);
    });
  } finally {
    const cleanupErrors: string[] = [];
    const activeBrowser = browserRef.current;
    if (activeBrowser !== null) {
      await bestEffortWithTimeout(
        "browser.close",
        2_500,
        () => activeBrowser.close(),
        cleanupErrors,
      );
    }
    try {
      await gui.stop();
    } catch (error) {
      cleanupErrors.push(`gui.stop failed: ${String(error)}`);
    }
    if (cleanupErrors.length > 0) {
      await testInfo.attach("mmltk gui cleanup errors", {
        body: cleanupErrors.join("\n"),
        contentType: "text/plain",
      });
    }
    await attachGuiArtifacts(testInfo, gui);
  }
});
