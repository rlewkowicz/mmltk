import { spawn, type ChildProcess } from "node:child_process";
import { constants as fsConstants, createWriteStream, existsSync } from "node:fs";
import {
  access,
  chmod,
  mkdir,
  mkdtemp,
  readdir,
  readFile,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import * as path from "node:path";

export interface IntentTraceRecord {
  request_id?: number;
  workflow?: string;
  intent?: string;
  payload?: unknown;
}

export interface MmltkGuiProcess {
  readonly cdpPort: number;
  readonly artifactsDir: string;
  readonly logDir: string;
  readonly browserHostLogPath: string;
  readonly cefLogPath: string;
  readonly outputPath: string;
  readonly intentTracePath: string;
  readonly child: ChildProcess;
  readonly output: () => string;
  stop(): Promise<void>;
}

export interface LaunchMmltkGuiOptions {
  readonly workerIndex: number;
  readonly cdpPort?: number;
}

const kOutputTailLimit = 96 * 1024;
const kContainerPrepareTimeoutMs = 120_000;

function repoRoot(): string {
  if (process.env.MMLTK_REPO_ROOT !== undefined && process.env.MMLTK_REPO_ROOT.length > 0) {
    return process.env.MMLTK_REPO_ROOT;
  }
  return path.resolve(process.cwd(), "../../..");
}

function containerPath(hostPath: string): string {
  return hostPath.startsWith("/host/") ? hostPath : `/host${hostPath}`;
}

function appendTail(current: string, next: Buffer | string): string {
  const appended = current + next.toString();
  return appended.length <= kOutputTailLimit
    ? appended
    : appended.slice(appended.length - kOutputTailLimit);
}

function processOutputTail(output: string): string {
  return output.length <= kOutputTailLimit
    ? output
    : output.slice(output.length - kOutputTailLimit);
}

async function runProcess(
  command: string,
  args: readonly string[],
  options: {
    readonly cwd: string;
    readonly env: NodeJS.ProcessEnv;
    readonly timeoutMs: number;
  },
): Promise<string> {
  return await new Promise((resolve, reject) => {
    const child = spawn(command, args, {
      cwd: options.cwd,
      env: options.env,
      stdio: ["ignore", "pipe", "pipe"],
    });
    let output = "";
    let completed = false;
    const timeout = setTimeout(() => {
      if (completed) {
        return;
      }
      completed = true;
      child.kill("SIGTERM");
      reject(
        new Error(
          `${path.basename(command)} ${args.join(" ")} timed out after ${options.timeoutMs}ms\n\n${processOutputTail(output)}`,
        ),
      );
    }, options.timeoutMs);
    const append = (chunk: Buffer | string): void => {
      output = appendTail(output, chunk);
    };
    child.stdout?.on("data", append);
    child.stderr?.on("data", append);
    child.once("error", (error) => {
      if (completed) {
        return;
      }
      completed = true;
      clearTimeout(timeout);
      reject(error);
    });
    child.once("exit", (code, signal) => {
      if (completed) {
        return;
      }
      completed = true;
      clearTimeout(timeout);
      if (code === 0) {
        resolve(output);
        return;
      }
      reject(
        new Error(
          `${path.basename(command)} ${args.join(" ")} exited with ${signal ?? code}\n\n${processOutputTail(output)}`,
        ),
      );
    });
  });
}

async function waitForExit(child: ChildProcess, timeoutMs: number): Promise<boolean> {
  if (child.exitCode !== null || child.signalCode !== null) {
    return true;
  }
  return await new Promise((resolve) => {
    const timeout = setTimeout(() => {
      child.off("exit", onExit);
      resolve(false);
    }, timeoutMs);
    const onExit = (): void => {
      clearTimeout(timeout);
      resolve(true);
    };
    child.once("exit", onExit);
  });
}

function signalProcessGroup(child: ChildProcess, signal: NodeJS.Signals): void {
  if (child.pid === undefined) {
    return;
  }
  try {
    process.kill(-child.pid, signal);
  } catch {
    try {
      child.kill(signal);
    } catch {
      // The process may already be gone.
    }
  }
}

function sleep(timeoutMs: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, timeoutMs);
  });
}

async function pkillByPattern(pattern: string, signal: "TERM" | "KILL"): Promise<void> {
  await new Promise<void>((resolve) => {
    const child = spawn("pkill", [`-${signal}`, "-f", pattern], {
      stdio: "ignore",
    });
    child.once("exit", () => resolve());
    child.once("error", () => resolve());
  });
}

async function terminateArtifactGuiProcesses(artifactsDir: string): Promise<void> {
  const pattern = containerPath(artifactsDir);
  await pkillByPattern(pattern, "TERM");
  await sleep(1_000);
  await pkillByPattern(pattern, "KILL");
}

async function prepareMmltkGuiContainer(root: string, env: NodeJS.ProcessEnv): Promise<string> {
  const wrapper = path.join(root, "mmltk");
  await access(wrapper, fsConstants.X_OK);
  return await runProcess(wrapper, ["--prepare-gui-container"], {
    cwd: root,
    env,
    timeoutMs: kContainerPrepareTimeoutMs,
  });
}

export async function launchMmltkGui(options: LaunchMmltkGuiOptions): Promise<MmltkGuiProcess> {
  const root = repoRoot();
  const wrapper = path.join(root, "mmltk");
  await access(wrapper, fsConstants.X_OK);
  const baseEnv = { ...process.env };

  const artifactsDir = await mkdtemp(
    path.join(tmpdir(), `mmltk-gui-e2e-${options.workerIndex}-`),
  );
  await chmod(artifactsDir, 0o777);
  const settingsPath = path.join(artifactsDir, "gui-settings.json");
  const intentTracePath = path.join(artifactsDir, "intent-trace.jsonl");
  const cefCacheDir = path.join(artifactsDir, "cef-cache");
  const logDir = path.join(artifactsDir, "logs");
  await mkdir(cefCacheDir, { recursive: true });
  await mkdir(logDir, { recursive: true });
  await chmod(cefCacheDir, 0o777);
  await chmod(logDir, 0o777);
  await writeFile(settingsPath, "{\"schema_version\":3}\n", "utf8");
  await chmod(settingsPath, 0o666);
  const browserHostLogPath = path.join(logDir, "mmltk_browser_host.log");
  const cefLogPath = path.join(logDir, "cef.log");
  const outputPath = path.join(logDir, "mmltk_gui_process_output.log");
  const outputFile = createWriteStream(outputPath, { flags: "a" });

  const cdpPort = options.cdpPort ?? 19_200 + options.workerIndex;
  const guiEnv: NodeJS.ProcessEnv = {
    ...baseEnv,
    MMLTK_GUI_SETTINGS_PATH: settingsPath,
    MMLTK_CEF_REMOTE_DEBUGGING_PORT: String(cdpPort),
    MMLTK_GUI_E2E_INTENT_TRACE_PATH: intentTracePath,
    MMLTK_CEF_ROOT_CACHE_PATH: containerPath(cefCacheDir),
    MMLTK_CEF_LOG_FILE: containerPath(cefLogPath),
    MMLTK_CEF_VERBOSE_LOGGING: "1",
    MMLTK_CEF_LOG_VERBOSITY: "1",
    MMLTK_CEF_VMODULE:
      "workspace_gpu_bridge*=4,webgpu*=3,gpu_device*=3,dawn*=2,gpu_memory_buffer*=3,native_pixmap*=3,gbm_buffer*=3",
    MMLTK_LOG_FILE: containerPath(browserHostLogPath),
    MMLTK_LOG_DIR: containerPath(logDir),
  };
  const prepareOutput = await prepareMmltkGuiContainer(root, guiEnv);
  if (prepareOutput.length > 0) {
    outputFile.write(prepareOutput);
  }
  const child = spawn(wrapper, ["--gui"], {
    cwd: root,
    detached: true,
    env: guiEnv,
    stdio: ["ignore", "pipe", "pipe"],
  });

  let output = "";
  if (prepareOutput.length > 0) {
    output = appendTail(output, prepareOutput);
  }
  child.stdout?.on("data", (chunk) => {
    output = appendTail(output, chunk);
    outputFile.write(chunk);
  });
  child.stderr?.on("data", (chunk) => {
    output = appendTail(output, chunk);
    outputFile.write(chunk);
  });

  const stop = async (): Promise<void> => {
    if (child.exitCode === null && child.signalCode === null) {
      signalProcessGroup(child, "SIGTERM");
      if (!(await waitForExit(child, 5_000))) {
        signalProcessGroup(child, "SIGKILL");
        await waitForExit(child, 5_000);
      }
    }
    await terminateArtifactGuiProcesses(artifactsDir);
    const prepareOutputAfterStop = await prepareMmltkGuiContainer(root, guiEnv).catch((error: unknown) => {
      outputFile.write(`\ncontainer cleanup failed: ${String(error)}\n`);
      return "";
    });
    if (prepareOutputAfterStop.length > 0) {
      output = appendTail(output, prepareOutputAfterStop);
      outputFile.write(prepareOutputAfterStop);
    }
    await new Promise<void>((resolve) => {
      outputFile.end(() => resolve());
    });
  };

  return {
    cdpPort,
    artifactsDir,
    logDir,
    browserHostLogPath,
    cefLogPath,
    outputPath,
    intentTracePath,
    child,
    output: () => output,
    stop,
  };
}

export async function readIntentTraceRecords(tracePath: string): Promise<IntentTraceRecord[]> {
  if (!existsSync(tracePath)) {
    return [];
  }
  const text = await readFile(tracePath, "utf8");
  const records: IntentTraceRecord[] = [];
  const lines = text.split("\n");
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index];
    if (line.trim().length === 0) {
      continue;
    }
    try {
      records.push(JSON.parse(line) as IntentTraceRecord);
    } catch (error) {
      if (index === lines.length - 1) {
        continue;
      }
      throw error;
    }
  }
  return records;
}

export async function readMmltkGuiLogFiles(gui: MmltkGuiProcess): Promise<string> {
  if (!existsSync(gui.logDir)) {
    return "";
  }
  const entries = await readdir(gui.logDir, { withFileTypes: true });
  const chunks: string[] = [];
  for (const entry of entries.filter((item) => item.isFile()).sort((a, b) => a.name.localeCompare(b.name))) {
    const filePath = path.join(gui.logDir, entry.name);
    chunks.push(`--- ${entry.name} ---\n${await readFile(filePath, "utf8")}`);
  }
  return chunks.join("\n\n");
}

export async function waitForIntentTraceRecord(
  tracePath: string,
  predicate: (record: IntentTraceRecord, index: number, records: readonly IntentTraceRecord[]) => boolean,
  timeoutMs = 15_000,
): Promise<IntentTraceRecord> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const records = await readIntentTraceRecords(tracePath);
    const match = records.find((record, index) => predicate(record, index, records));
    if (match !== undefined) {
      return match;
    }
    await new Promise((resolve) => {
      setTimeout(resolve, 50);
    });
  }
  throw new Error(`Timed out waiting for intent trace record in ${tracePath}`);
}
