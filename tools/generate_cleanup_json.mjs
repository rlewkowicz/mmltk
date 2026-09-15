#!/usr/bin/env node

import { spawn, spawnSync } from "node:child_process";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { availableParallelism, cpus, tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { pathToFileURL } from "node:url";

const REPO_ROOT = process.cwd();
const DUPLO_BINARY = "/bin/duplo";
const PMD_BINARY = process.env.PMD_BIN ?? "pmd";
const THIRD_PARTY_PREFIX = "third_party/";
const THREADS = Math.max(
  1,
  typeof availableParallelism === "function"
    ? availableParallelism()
    : cpus().length,
);
const CPP_SUFFIXES = [
  ".c",
  ".cc",
  ".cpp",
  ".cppm",
  ".cxx",
  ".c++",
  ".def",
  ".h",
  ".hh",
  ".hpp",
  ".hxx",
  ".h++",
  ".ipp",
  ".inl",
  ".inc",
  ".tcc",
  ".tpp",
  ".txx",
  ".cu",
  ".cuh",
];
const CPP_TEMPLATE_SUFFIXES = CPP_SUFFIXES.map((suffix) => `${suffix}.in`);

// These headers contain assembly, or otherwise cannot be tokenized as C++ by
// CPD. Both block-duplication detectors omit them so they cannot produce
// misleading matches or silently downgrade a CPD lexer failure.
const NON_CPP_PREFIXES = [
  "third_party/rapidgzip/src/external/isa-l/crc/aarch64/",
  "third_party/rapidgzip/src/external/isa-l/igzip/aarch64/",
  "third_party/rapidgzip/src/external/isa-l/include/aarch64_multibinary.h",
  "third_party/rapidgzip/src/external/isa-l/include/riscv64_multibinary.h",
];

export const INVENTORY_COMMANDS = Object.freeze({
  tracked: Object.freeze(["git", "ls-files", "-z"]),
  untracked: Object.freeze([
    "git",
    "ls-files",
    "--others",
    "--exclude-standard",
    "-z",
  ]),
  deleted: Object.freeze(["git", "ls-files", "--deleted", "-z"]),
});

function deepFreeze(value) {
  Object.freeze(value);
  for (const child of Object.values(value)) {
    if (
      child !== null &&
      typeof child === "object" &&
      !Object.isFrozen(child)
    ) {
      deepFreeze(child);
    }
  }
  return value;
}

const CPP_PROFILE = {
  name: "cpp",
  selector: "--cpp",
  output: "cleanup/cpp-code-deduplication.json",
  purpose:
    "Exhaustive first-party tracked C/C++/CUDA textual and structural block-duplication cleanup",
  includedDescription:
    "Git-tracked and untracked-unignored C/C++/CUDA source and header suffixes",
  exclusionRules: ["third_party/"],
  includedSuffixes: [...CPP_SUFFIXES, ...CPP_TEMPLATE_SUFFIXES],
  includedPrefix: "",
  excludedPrefixes: [THIRD_PARTY_PREFIX],
  detectorExcludedPrefixes: NON_CPP_PREFIXES,
  duplo: {
    binary: DUPLO_BINARY,
    minLines: 9,
    flags: ["-ip"],
    contentFilter:
      "comments, strings, and preprocessor-only lines excluded",
    filterNonCode: true,
  },
  cpd: {
    binary: PMD_BINARY,
    language: "cpp",
    minTokens: 100,
    flags: ["--ignore-identifiers", "--ignore-literal-sequences"],
  },
  inventoryLabels: {
    all: "tracked_cpp_file_count",
    deleted: "worktree_deleted_cpp_files",
    missing: "missing_tracked_cpp_files",
  },
};

const FRONTEND_PROFILE = {
  name: "frontend",
  selector: "--frontend",
  output: "cleanup/frontend-code-deduplication.json",
  purpose:
    "Exhaustive first-party Iced Rust textual and structural block-duplication cleanup",
  includedDescription:
    "Git-tracked and untracked-unignored src/frontend/iced/**/*.rs",
  exclusionRules: [
    "worktree-deleted paths",
    "Git-ignored paths",
    "build, target, dist, and generated output",
    "third_party",
  ],
  includedSuffixes: [".rs"],
  includedPrefix: "src/frontend/iced/",
  excludedPrefixes: [
    THIRD_PARTY_PREFIX,
    "build/",
    "src/frontend/iced/build/",
    "src/frontend/iced/target/",
    "src/frontend/iced/dist/",
    "src/frontend/iced/third_party/",
  ],
  detectorExcludedPrefixes: [],
  retainedGeneratedPaths: ["src/frontend/iced/src/generated.rs"],
  generatedOutputPatterns: [
    /(^|\/)(?:build|target|dist)(?:\/|$)/u,
    /(^|\/)generated(?:\/|$)/u,
  ],
  duplo: {
    binary: DUPLO_BINARY,
    minLines: 9,
    flags: [],
    contentFilter: "none",
    filterNonCode: false,
  },
  cpd: {
    binary: PMD_BINARY,
    language: "rust",
    minTokens: 75,
    flags: [],
  },
  inventoryLabels: {
    all: "candidate_rust_file_count",
    deleted: "worktree_deleted_rust_files",
    missing: "missing_candidate_rust_files",
  },
};

export const CLEANUP_PROFILES = deepFreeze({
  cpp: CPP_PROFILE,
  frontend: FRONTEND_PROFILE,
});

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: REPO_ROOT,
    encoding: "utf8",
    maxBuffer: 512 * 1024 * 1024,
    ...options,
  });
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0 && !options.acceptStatuses?.includes(result.status)) {
    throw new Error(
      `${command} exited with status ${result.status}\n${result.stderr}`,
    );
  }
  return result;
}

function runAsync(command, args, options = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { cwd: REPO_ROOT });
    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8").on("data", (chunk) => {
      stdout += chunk;
    });
    child.stderr.setEncoding("utf8").on("data", (chunk) => {
      stderr += chunk;
    });
    child.on("error", reject);
    child.on("close", (status) => {
      if (status !== 0 && !options.acceptStatuses?.includes(status)) {
        reject(new Error(`${command} exited with status ${status}\n${stderr}`));
        return;
      }
      resolve({ stdout, stderr, status });
    });
    if (options.input !== undefined) {
      child.stdin.write(options.input);
    }
    child.stdin.end();
  });
}

export function usageText() {
  return [
    "Usage:",
    "  node tools/generate_cleanup_json.mjs [--cpp|--frontend]",
    "",
    "With no profile selector, --cpp is used. Each invocation scans the",
    "selected first-party source inventory with Duplo and PMD CPD, then writes",
    "the corresponding cleanup/*-code-deduplication.json report.",
    "",
    "  --cpp       C/C++/CUDA: Duplo 7 lines; C++ CPD 69 tokens",
    "  --frontend  Iced Rust: Duplo 9 lines; Rust CPD 75 tokens",
    "  -h, --help  Show this help",
  ].join("\n");
}

export function parseArgs(args) {
  if (args.length === 0) {
    return { kind: "profile", profile: CLEANUP_PROFILES.cpp };
  }
  if (args.length !== 1) {
    throw new Error(
      "expected zero arguments or exactly one of --cpp and --frontend",
    );
  }
  const [argument] = args;
  if (argument === "--help" || argument === "-h") {
    return { kind: "help" };
  }
  const profile = Object.values(CLEANUP_PROFILES).find(
    (candidate) => candidate.selector === argument,
  );
  if (profile === undefined) {
    throw new Error(`unknown argument: ${argument}`);
  }
  return { kind: "profile", profile };
}

function isCppPath(path) {
  const normalized = path.toLowerCase();
  return [...CPP_SUFFIXES, ...CPP_TEMPLATE_SUFFIXES].some((suffix) =>
    normalized.endsWith(suffix),
  );
}

function isGeneratedFrontendOutput(path, profile) {
  return (
    !profile.retainedGeneratedPaths?.includes(path) &&
    profile.generatedOutputPatterns?.some((pattern) => pattern.test(path))
  );
}

export function profileIncludesPath(profile, path) {
  if (profile.name === "cpp") {
    return isCppPath(path);
  }
  return (
    path.startsWith(profile.includedPrefix) &&
    path.toLowerCase().endsWith(".rs")
  );
}

export function detectorArguments(profile, { threads = THREADS, fileList } = {}) {
  return {
    duplo: [
      "-j",
      String(threads),
      "-ml",
      String(profile.duplo.minLines),
      ...profile.duplo.flags,
      "-json",
      "-",
      "-",
    ],
    cpd: [
      "cpd",
      "--file-list",
      fileList ?? "<temporary-file-list>",
      "--language",
      profile.cpd.language,
      "--minimum-tokens",
      String(profile.cpd.minTokens),
      ...profile.cpd.flags,
      "--format",
      "xml",
      "--no-fail-on-violation",
    ],
  };
}

function splitGitPaths(stdout) {
  return stdout.split("\0").filter(Boolean);
}

function runInventoryCommand(command) {
  return splitGitPaths(run(command[0], command.slice(1)).stdout);
}

function collectInventoryInputs() {
  return {
    tracked: runInventoryCommand(INVENTORY_COMMANDS.tracked),
    untracked: runInventoryCommand(INVENTORY_COMMANDS.untracked),
    deleted: runInventoryCommand(INVENTORY_COMMANDS.deleted),
  };
}

export function buildInventory(
  profile,
  { tracked, untracked, deleted },
  pathAvailable = (path) => {
    const absolute = join(REPO_ROOT, path);
    return existsSync(absolute) && statSync(absolute).isFile();
  },
) {
  const deletedSet = new Set(deleted);
  const candidates = [...new Set([...tracked, ...untracked])]
    .filter((path) => profileIncludesPath(profile, path))
    .filter((path) => !deletedSet.has(path))
    .sort();
  const excludedByPrefix = candidates.filter((path) =>
    profile.excludedPrefixes.some((prefix) => path.startsWith(prefix)),
  );
  const excludedByPrefixSet = new Set(excludedByPrefix);
  const excludedGeneratedOutput =
    profile.generatedOutputPatterns === undefined
      ? []
      : candidates.filter(
          (path) =>
            !excludedByPrefixSet.has(path) &&
            isGeneratedFrontendOutput(path, profile),
        );
  const excluded = new Set([
    ...excludedByPrefix,
    ...excludedGeneratedOutput,
  ]);
  const scannedFiles = candidates.filter((path) => !excluded.has(path));

  const pathsWithNewlines = scannedFiles.filter((path) => path.includes("\n"));
  if (pathsWithNewlines.length !== 0) {
    throw new Error(
      `Duplo/CPD file lists cannot represent these paths:\n${pathsWithNewlines.join(
        "\n",
      )}`,
    );
  }

  const unavailable = scannedFiles.filter((path) => !pathAvailable(path));
  if (unavailable.length !== 0) {
    throw new Error(
      `${profile.name} inventory paths are unavailable:\n${unavailable.join("\n")}`,
    );
  }

  if (excluded.size + scannedFiles.length !== candidates.length) {
    throw new Error(`${profile.name} inventory is incomplete`);
  }

  return {
    candidates,
    excludedByPrefix,
    excludedGeneratedOutput,
    scannedFiles,
    deletedFiles: [...deletedSet]
      .filter((path) => profileIncludesPath(profile, path))
      .sort(),
    trackedCandidateCount: tracked.filter((path) =>
      profileIncludesPath(profile, path),
    ).length,
    untrackedCandidateCount: untracked.filter((path) =>
      profileIncludesPath(profile, path),
    ).length,
  };
}

async function runDuplo(profile, paths) {
  const args = detectorArguments(profile).duplo;
  if (paths.length === 0) {
    return { hits: [], arguments: args, status: 0, stderr: "" };
  }
  const result = await runAsync(profile.duplo.binary, args, {
    input: paths.join("\n"),
    acceptStatuses: [1],
  });
  const stderr = result.stderr.trim();
  if (/(^|\n)\s*(error|fatal):/iu.test(stderr)) {
    throw new Error(`Duplo reported a scan failure:\n${stderr}`);
  }
  return {
    hits: parseDuploJson(result.stdout),
    arguments: args,
    status: result.status,
    stderr,
  };
}

export function decodeXmlAttribute(value) {
  return value.replace(
    /&(lt|gt|amp|quot|apos|#x[0-9a-f]+|#\d+);/giu,
    (_entity, name) => {
      switch (name.toLowerCase()) {
        case "lt":
          return "<";
        case "gt":
          return ">";
        case "amp":
          return "&";
        case "quot":
          return '"';
        case "apos":
          return "'";
        default:
          return String.fromCodePoint(
            name[1]?.toLowerCase() === "x"
              ? Number.parseInt(name.slice(2), 16)
              : Number.parseInt(name.slice(1), 10),
          );
      }
    },
  );
}

export function parseXmlAttributes(tag) {
  const attributes = {};
  for (const match of tag.matchAll(/([\w:-]+)="([^"]*)"/gu)) {
    attributes[match[1]] = decodeXmlAttribute(match[2]);
  }
  return attributes;
}

export function parseCpdXml(xml) {
  const duplications = [];
  for (const match of xml.matchAll(
    /<duplication\b([^>]*)>([\s\S]*?)<\/duplication>/gu,
  )) {
    const header = parseXmlAttributes(match[1]);
    const lineCount = Number(header.lines);
    const tokenCount = Number(header.tokens);
    if (
      !Number.isInteger(lineCount) ||
      lineCount < 1 ||
      !Number.isInteger(tokenCount) ||
      tokenCount < 1
    ) {
      throw new Error(
        `CPD returned an invalid duplication header: ${match[0].slice(0, 200)}`,
      );
    }

    const body = match[2].replace(
      /<codefragment>[\s\S]*?<\/codefragment>/gu,
      "",
    );
    const occurrences = [];
    for (const fileMatch of body.matchAll(/<file\b([^>]*)\/>/gu)) {
      const attributes = parseXmlAttributes(fileMatch[1]);
      const start = Number(attributes.line);
      const end = Number(attributes.endline);
      if (
        !attributes.path ||
        !Number.isInteger(start) ||
        !Number.isInteger(end) ||
        start < 1 ||
        end < start
      ) {
        throw new Error(
          `CPD returned an invalid source range: ${fileMatch[0]}`,
        );
      }
      occurrences.push({ path: attributes.path, start, end });
    }
    if (occurrences.length < 2) {
      throw new Error(
        `CPD returned fewer than two occurrences: ${match[0].slice(0, 200)}`,
      );
    }
    duplications.push({ lineCount, tokenCount, occurrences });
  }
  return duplications;
}

async function runCpd(profile, paths) {
  const lexable = paths.filter(
    (path) =>
      !profile.detectorExcludedPrefixes.some((prefix) =>
        path.startsWith(prefix),
      ),
  );
  if (lexable.length === 0) {
    return {
      duplications: [],
      arguments: detectorArguments(profile).cpd,
      excludedLexerCount: 0,
      status: 0,
      stderr: "",
    };
  }

  const temporaryDirectory = mkdtempSync(join(tmpdir(), "cpd-file-list-"));
  const fileList = join(temporaryDirectory, "files.txt");
  const args = detectorArguments(profile, { fileList }).cpd;
  let result;
  try {
    writeFileSync(fileList, `${lexable.join("\n")}\n`, "utf8");
    result = await runAsync(
      profile.cpd.binary,
      args,
      { acceptStatuses: [4, 5] },
    );
  } catch (error) {
    if (error?.code === "ENOENT") {
      throw new Error(
        `${profile.cpd.binary} is not installed; install PMD 7.7.0+ or set PMD_BIN`,
      );
    }
    throw error;
  } finally {
    rmSync(temporaryDirectory, { recursive: true, force: true });
  }

  const stderr = result.stderr.trim();
  if (/(^|\n)\s*\[ERROR\]/u.test(stderr)) {
    throw new Error(
      `CPD could not lex a ${profile.name} source outside the configured exclusions:\n${stderr}`,
    );
  }
  return {
    duplications: parseCpdXml(result.stdout),
    arguments: args,
    excludedLexerCount: paths.length - lexable.length,
    status: result.status,
    stderr,
  };
}

export function repoRelativePath(path, repoRoot = REPO_ROOT) {
  const prefix = `${repoRoot}/`;
  if (path.startsWith(prefix)) {
    return path.slice(prefix.length);
  }
  if (!path.startsWith("/")) {
    return path;
  }
  throw new Error(`CPD returned a path outside the repository: ${path}`);
}

export function mergeOverlappingRanges(ranges) {
  const sorted = [...ranges].sort(
    (left, right) => left[0] - right[0] || left[1] - right[1],
  );
  const merged = [];
  for (const [start, end] of sorted) {
    const previous = merged.at(-1);
    if (previous && start <= previous[1]) {
      previous[1] = Math.max(previous[1], end);
    } else {
      merged.push([start, end]);
    }
  }
  return merged;
}

export function commonDirectory(paths) {
  const directories = paths.map((path) => {
    const directory = dirname(path);
    return directory === "." ? [] : directory.split("/");
  });
  const common = [];
  const shortest = Math.min(...directories.map((parts) => parts.length));
  for (let index = 0; index < shortest; index += 1) {
    const part = directories[0][index];
    if (!directories.every((parts) => parts[index] === part)) {
      break;
    }
    common.push(part);
  }
  return common.join("/");
}

export function makeHit(lineCount, tokenCount, rangesByFile, mergeRanges) {
  const files = Array.from(rangesByFile, ([path, ranges]) => ({
    path,
    lines: mergeRanges
      ? mergeOverlappingRanges(ranges)
      : [...ranges].sort(
          (left, right) => left[0] - right[0] || left[1] - right[1],
        ),
  })).sort((left, right) => left.path.localeCompare(right.path));
  const occurrenceCount = files.reduce(
    (count, file) => count + file.lines.length,
    0,
  );
  const crossFile = files.length > 1;
  const sharedDirectory = commonDirectory(files.map((file) => file.path));
  return {
    kind: crossFile ? "cross-file" : "within-file",
    line_count: lineCount,
    ...(tokenCount === undefined ? {} : { token_count: tokenCount }),
    file_count: files.length,
    occurrence_count: occurrenceCount,
    removable_duplicate_lines: lineCount * (occurrenceCount - 1),
    ...(crossFile
      ? {
          highest_shared_directory: sharedDirectory || ".",
          shared_code_directory: sharedDirectory || "shared",
        }
      : {}),
    files,
  };
}

export function structuralHits(
  profile,
  duplications,
  scannedPaths,
  repoRoot = REPO_ROOT,
) {
  const scanned = new Set(scannedPaths);
  return duplications
    .map((duplication) => {
      const rangesByFile = new Map();
      for (const occurrence of duplication.occurrences) {
        const path = repoRelativePath(occurrence.path, repoRoot);
        if (!scanned.has(path) || !profileIncludesPath(profile, path)) {
          throw new Error(`CPD returned a path outside its scope: ${path}`);
        }
        const ranges = rangesByFile.get(path) ?? [];
        ranges.push([occurrence.start, occurrence.end]);
        rangesByFile.set(path, ranges);
      }
      return makeHit(
        duplication.lineCount,
        duplication.tokenCount,
        rangesByFile,
        true,
      );
    })
    .filter((hit) => hit.occurrence_count >= 2);
}

export function duploLineCount(hit) {
  const explicit = Number(hit.LineCount);
  if (Number.isInteger(explicit) && explicit > 0) {
    return explicit;
  }
  const start = Number(hit.StartLineNumber1);
  const end = Number(hit.EndLineNumber1);
  return Number.isInteger(start) && Number.isInteger(end) && end >= start
    ? end - start + 1
    : 0;
}

export function duploOccurrence(hit, side) {
  const path = String(hit[`SourceFile${side}`] ?? "");
  const start = Number(hit[`StartLineNumber${side}`]);
  const end = Number(hit[`EndLineNumber${side}`]);
  if (
    !path ||
    !Number.isInteger(start) ||
    !Number.isInteger(end) ||
    start < 1 ||
    end < start
  ) {
    throw new Error(
      `Duplo returned an invalid source range: ${JSON.stringify(hit)}`,
    );
  }
  return { path, start, end };
}

// Comments and string/character literals are blanked while newlines remain in
// place, preserving every detector line number.
export function stripCommentsAndStrings(source) {
  let result = "";
  let index = 0;
  while (index < source.length) {
    const current = source[index];
    const next = source[index + 1];
    if (current === "/" && next === "/") {
      while (index < source.length && source[index] !== "\n") {
        index += 1;
      }
    } else if (current === "/" && next === "*") {
      index += 2;
      while (
        index < source.length &&
        !(source[index] === "*" && source[index + 1] === "/")
      ) {
        if (source[index] === "\n") {
          result += "\n";
        }
        index += 1;
      }
      index = Math.min(source.length, index + 2);
    } else if (
      current === '"' &&
      /(?:^|[^A-Za-z0-9_])(?:u8|[uUL])?R$/.test(result.slice(-3))
    ) {
      index += 1;
      let delimiter = "";
      while (index < source.length && source[index] !== "(") {
        delimiter += source[index];
        index += 1;
      }
      index += 1;
      const closer = `)${delimiter}"`;
      const end = source.indexOf(closer, index);
      const body = source.slice(index, end === -1 ? source.length : end);
      result += "\n".repeat((body.match(/\n/g) ?? []).length);
      index = end === -1 ? source.length : end + closer.length;
      result += " ";
    } else if (current === '"' || current === "'") {
      index += 1;
      while (
        index < source.length &&
        source[index] !== current &&
        source[index] !== "\n"
      ) {
        if (source[index] === "\\") {
          index += 1;
        }
        index += 1;
      }
      if (index < source.length && source[index] === current) {
        index += 1;
      }
      result += " ";
    } else {
      result += current;
      index += 1;
    }
  }
  return result;
}

export function filterDuploCandidates(
  profile,
  rawHits,
  sourceReader = (path) => readFileSync(join(REPO_ROOT, path), "utf8"),
) {
  if (!profile.duplo.filterNonCode) {
    return { hits: rawHits, filteredCount: 0 };
  }
  const cache = new Map();
  const sourceLines = (path) => {
    let lines = cache.get(path);
    if (lines === undefined) {
      lines = stripCommentsAndStrings(sourceReader(path)).split("\n");
      cache.set(path, lines);
    }
    return lines;
  };
  const codeCount = (occurrence) => {
    const lines = sourceLines(occurrence.path);
    let count = 0;
    for (
      let index = occurrence.start - 1;
      index < occurrence.end;
      index += 1
    ) {
      const line = (lines[index] ?? "").trim();
      if (line !== "" && !line.startsWith("#")) {
        count += 1;
      }
    }
    return count;
  };
  const hits = rawHits.filter((hit) =>
    [duploOccurrence(hit, 1), duploOccurrence(hit, 2)].every(
      (occurrence) =>
        codeCount(occurrence) >= profile.duplo.minLines,
    ),
  );
  return { hits, filteredCount: rawHits.length - hits.length };
}

function duploDuplicateKey(hit) {
  if (Array.isArray(hit.Lines) && hit.Lines.length !== 0) {
    return `${duploLineCount(hit)}\0${hit.Lines.join("\n")}`;
  }
  return [
    "locations",
    duploLineCount(hit),
    hit.SourceFile1,
    hit.StartLineNumber1,
    hit.EndLineNumber1,
    hit.SourceFile2,
    hit.StartLineNumber2,
    hit.EndLineNumber2,
  ].join("\0");
}

export function compactDuploHits(profile, rawHits, scannedPaths) {
  const scanned = new Set(scannedPaths);
  const groups = new Map();
  for (const hit of rawHits) {
    const occurrences = [duploOccurrence(hit, 1), duploOccurrence(hit, 2)];
    for (const occurrence of occurrences) {
      if (
        !scanned.has(occurrence.path) ||
        !profileIncludesPath(profile, occurrence.path)
      ) {
        throw new Error(
          `Duplo returned a path outside its scope: ${occurrence.path}`,
        );
      }
    }

    const key = duploDuplicateKey(hit);
    const group = groups.get(key) ?? {
      lineCount: 0,
      occurrences: new Map(),
    };
    group.lineCount = Math.max(group.lineCount, duploLineCount(hit));
    for (const occurrence of occurrences) {
      group.occurrences.set(
        `${occurrence.path}\0${occurrence.start}\0${occurrence.end}`,
        occurrence,
      );
    }
    groups.set(key, group);
  }

  return Array.from(groups.values(), (group) => {
    const rangesByFile = new Map();
    for (const occurrence of group.occurrences.values()) {
      const ranges = rangesByFile.get(occurrence.path) ?? [];
      ranges.push([occurrence.start, occurrence.end]);
      rangesByFile.set(occurrence.path, ranges);
    }
    return makeHit(group.lineCount, undefined, rangesByFile, false);
  });
}

export function compareHits(left, right) {
  if (left.kind !== right.kind) {
    return left.kind === "cross-file" ? -1 : 1;
  }
  return (
    right.file_count - left.file_count ||
    right.occurrence_count - left.occurrence_count ||
    right.line_count - left.line_count ||
    left.files[0].path.localeCompare(right.files[0].path) ||
    left.files[0].lines[0][0] - right.files[0].lines[0][0]
  );
}

export function summarize(hits) {
  const affectedFiles = new Set();
  let removableDuplicateLines = 0;
  for (const hit of hits) {
    removableDuplicateLines += hit.removable_duplicate_lines;
    for (const file of hit.files) {
      affectedFiles.add(file.path);
    }
  }
  return {
    hit_count: hits.length,
    cross_file_hits: hits.filter((hit) => hit.kind === "cross-file").length,
    within_file_hits: hits.filter((hit) => hit.kind === "within-file").length,
    affected_file_count: affectedFiles.size,
    removable_duplicate_lines: removableDuplicateLines,
  };
}

export function parseInlineSuppressions(path, source) {
  const byPath = new Map();
  const lines = source.split("\n");
  const suppressions = [];
  let openRange = null;
  for (let index = 0; index < lines.length; index += 1) {
    const ignore = lines[index].match(
      /\/\/\s*CLEANUP-IGNORE:\s*(\S(?:.*\S)?)\s*$/u,
    );
    if (ignore) {
      const prefix = lines[index].slice(0, ignore.index).trim();
      let targetLine = index + 1;
      if (prefix === "") {
        let targetIndex = index + 1;
        while (
          targetIndex < lines.length &&
          (lines[targetIndex].trim() === "" ||
            lines[targetIndex].trim().startsWith("//"))
        ) {
          targetIndex += 1;
        }
        targetLine = targetIndex + 1;
      }
      suppressions.push({
        kind: "single",
        detectors: ["duplo", "cpd"],
        marker_line: index + 1,
        start_line: targetLine,
        end_line: targetLine,
        reason: ignore[1],
      });
      continue;
    }

    const off = lines[index].match(
      /\/\/\s*(CLEANUP|CPD)-OFF:\s*(\S(?:.*\S)?)\s*$/u,
    );
    if (off) {
      if (openRange !== null) {
        throw new Error(`nested cleanup suppression at ${path}:${index + 1}`);
      }
      openRange = {
        family: off[1],
        detectors: off[1] === "CPD" ? ["cpd"] : ["duplo", "cpd"],
        marker_line: index + 1,
        reason: off[2],
      };
      continue;
    }

    const on = lines[index].match(/\/\/\s*(CLEANUP|CPD)-ON\b/u);
    if (on) {
      if (openRange === null || openRange.family !== on[1]) {
        throw new Error(
          `unmatched cleanup suppression terminator at ${path}:${index + 1}`,
        );
      }
      suppressions.push({
        kind: "range",
        detectors: openRange.detectors,
        marker_line: openRange.marker_line,
        start_line: openRange.marker_line,
        end_line: index + 1,
        reason: openRange.reason,
      });
      openRange = null;
    }
  }
  if (openRange !== null) {
    throw new Error(
      `unterminated cleanup suppression at ${path}:${openRange.marker_line}`,
    );
  }
  suppressions.sort(
    (left, right) =>
      left.start_line - right.start_line || left.end_line - right.end_line,
  );
  for (let index = 1; index < suppressions.length; index += 1) {
    if (suppressions[index].start_line <= suppressions[index - 1].end_line) {
      throw new Error(
        `overlapping atomic cleanup suppressions at ${path}:${suppressions[index].marker_line}`,
      );
    }
  }
  if (suppressions.length !== 0) {
    byPath.set(path, suppressions);
  }
  return byPath;
}

function loadInlineSuppressions(paths) {
  const byPath = new Map();
  for (const path of paths) {
    for (const [suppressedPath, suppressions] of parseInlineSuppressions(
      path,
      readFileSync(join(REPO_ROOT, path), "utf8"),
    )) {
      byPath.set(suppressedPath, suppressions);
    }
  }
  return byPath;
}

export function applyInlineSuppressions(hits, suppressions, detector) {
  const remaining = [];
  const suppressed = [];
  for (const hit of hits) {
    const rangesByFile = new Map();
    for (const file of hit.files) {
      const retained = [];
      const candidates = (suppressions.get(file.path) ?? []).filter(
        (candidate) => candidate.detectors.includes(detector),
      );
      for (const [start, end] of file.lines) {
        const suppression = candidates.find(
          (candidate) =>
            (candidate.kind === "single" &&
              candidate.start_line === start) ||
            (candidate.kind === "range" &&
              candidate.start_line <= start &&
              end <= candidate.end_line),
        );
        if (suppression === undefined) {
          retained.push([start, end]);
        } else {
          suppressed.push({
            detector,
            path: file.path,
            lines: [start, end],
            marker_line: suppression.marker_line,
            suppression_kind: suppression.kind,
            suppression_lines: [
              suppression.start_line,
              suppression.end_line,
            ],
            reason: suppression.reason,
          });
        }
      }
      if (retained.length !== 0) {
        rangesByFile.set(file.path, retained);
      }
    }
    const occurrenceCount = [...rangesByFile.values()].reduce(
      (count, ranges) => count + ranges.length,
      0,
    );
    if (occurrenceCount >= 2) {
      remaining.push(
        makeHit(
          hit.line_count,
          hit.token_count,
          rangesByFile,
          false,
        ),
      );
    }
  }
  return { hits: remaining, suppressed };
}

export function requireAtomicSuppressionUse(suppressedOccurrences) {
  const uses = new Map();
  for (const occurrence of suppressedOccurrences) {
    const key = [
      occurrence.detector,
      occurrence.path,
      occurrence.marker_line,
    ].join("\0");
    const sourceStarts = uses.get(key) ?? new Set();
    sourceStarts.add(occurrence.lines[0]);
    if (sourceStarts.size > 1) {
      throw new Error(
        `cleanup suppression at ${occurrence.path}:${occurrence.marker_line} hides multiple local ${occurrence.detector} starts; split it into atomic inline suppressions`,
      );
    }
    // CPD can emit several clone groups for one physical block when the same
    // local start matches differently sized or differently grouped peers.
    // That remains one source occurrence; atomicity is violated only when the
    // marker reaches a distinct local start.
    uses.set(key, sourceStarts);
  }
}

export function parseDuploJson(json) {
  const hits = json.trim() ? (JSON.parse(json) ?? []) : [];
  if (!Array.isArray(hits)) {
    throw new Error("Duplo JSON output is not an array");
  }
  return hits;
}

function scopeMetadata(profile, files) {
  const shared = {
    inventory_commands: {
      tracked: [...INVENTORY_COMMANDS.tracked],
      untracked_unignored: [...INVENTORY_COMMANDS.untracked],
      worktree_deleted: [...INVENTORY_COMMANDS.deleted],
    },
    inclusion_rule: profile.includedDescription,
    exclusion_rules: profile.exclusionRules,
    excluded_path_prefixes: profile.excludedPrefixes,
    tracked_candidate_file_count: files.trackedCandidateCount,
    untracked_unignored_candidate_file_count: files.untrackedCandidateCount,
    worktree_deleted_candidate_file_count: files.deletedFiles.length,
    excluded_by_prefix_file_count: files.excludedByPrefix.length,
    excluded_generated_output_file_count:
      files.excludedGeneratedOutput.length,
    excluded_generated_output_files: files.excludedGeneratedOutput,
    scanned_file_count: files.scannedFiles.length,
    [profile.inventoryLabels.deleted]: files.deletedFiles,
    [profile.inventoryLabels.missing]: [],
  };
  if (profile.name === "cpp") {
    return {
      tracked_file_source: [...INVENTORY_COMMANDS.tracked],
      included_suffixes: profile.includedSuffixes,
      excluded_path_prefixes: profile.excludedPrefixes,
      tracked_cpp_file_count: files.candidates.length,
      excluded_third_party_file_count: files.excludedByPrefix.length,
      scanned_file_count: files.scannedFiles.length,
      worktree_deleted_cpp_files: files.deletedFiles,
      missing_tracked_cpp_files: [],
      inventory_commands: shared.inventory_commands,
      tracked_candidate_file_count: shared.tracked_candidate_file_count,
      untracked_unignored_candidate_file_count:
        shared.untracked_unignored_candidate_file_count,
      excluded_generated_output_files: [],
    };
  }
  return {
    ...shared,
    included_prefix: profile.includedPrefix,
    included_suffixes: profile.includedSuffixes,
    retained_generated_paths: profile.retainedGeneratedPaths,
    generated_output_exclusion_patterns: profile.generatedOutputPatterns.map(
      (pattern) => pattern.source,
    ),
    excluded_by_prefix_files: files.excludedByPrefix,
    candidate_rust_file_count: files.candidates.length,
  };
}

export function buildReport({
  profile,
  files,
  duplo,
  cpd,
  codeOnlyDuplo,
  hits,
  structuralHitsAfterSuppression,
  duploSuppression,
  cpdSuppression,
  clock = () => new Date(),
  threads = THREADS,
}) {
  return {
    generated_at: clock().toISOString(),
    profile: profile.name,
    purpose: profile.purpose,
    scope: scopeMetadata(profile, files),
    duplo: {
      binary: profile.duplo.binary,
      arguments: duplo.arguments,
      min_lines: profile.duplo.minLines,
      content_filter: profile.duplo.contentFilter,
      non_code_hits_filtered: codeOnlyDuplo.filteredCount,
      threads,
      status: duplo.status,
      stderr: duplo.stderr,
    },
    cpd: {
      binary: profile.cpd.binary,
      arguments: cpd.arguments,
      language: profile.cpd.language,
      min_tokens: profile.cpd.minTokens,
      flags: profile.cpd.flags,
      ...(profile.name === "cpp"
        ? {
            excluded_non_cpp_prefixes: profile.detectorExcludedPrefixes,
            excluded_non_cpp_file_count: cpd.excludedLexerCount,
          }
        : {
            excluded_lexer_prefixes: profile.detectorExcludedPrefixes,
            excluded_lexer_file_count: cpd.excludedLexerCount,
          }),
      status: cpd.status,
      stderr: cpd.stderr,
    },
    hit_order: [
      "cross-file before within-file",
      "higher file count",
      "higher occurrence count",
      "higher line count",
    ],
    summary: summarize(hits),
    structural_summary: summarize(structuralHitsAfterSuppression),
    inline_duplication_suppression: {
      markers: [
        "// CLEANUP-IGNORE: <false-positive reason>",
        "// CLEANUP-OFF: <false-positive reason> ... // CLEANUP-ON",
        "// CPD-OFF: <false-positive reason> ... // CPD-ON",
      ],
      policy:
        "Inline and atomic: one comment targets one occurrence, and one bounded range encloses exactly one multi-line occurrence.",
      suppressed_duplo_occurrences: duploSuppression.suppressed,
      suppressed_cpd_occurrences: cpdSuppression.suppressed,
    },
    hits,
    structural_hits: structuralHitsAfterSuppression,
  };
}

export function serializeReport(report) {
  return `${JSON.stringify(report, null, 2)}\n`;
}

export async function generateReport(
  profile,
  {
    clock = () => new Date(),
    inventoryInputs = collectInventoryInputs(),
    pathAvailable,
  } = {},
) {
  const startedAt = Date.now();
  const files = buildInventory(profile, inventoryInputs, pathAvailable);
  const duploPaths = files.scannedFiles.filter(
    (path) =>
      !profile.detectorExcludedPrefixes.some((prefix) =>
        path.startsWith(prefix),
      ),
  );

  const externalStarted = Date.now();
  const [duplo, cpd] = await Promise.all([
    runDuplo(profile, duploPaths),
    runCpd(profile, files.scannedFiles),
  ]);
  const externalSeconds = (Date.now() - externalStarted) / 1000;

  const codeOnlyDuplo = filterDuploCandidates(profile, duplo.hits);
  const rawDuploHits = compactDuploHits(
    profile,
    codeOnlyDuplo.hits,
    files.scannedFiles,
  ).sort(compareHits);
  const rawCpdHits = structuralHits(
    profile,
    cpd.duplications,
    files.scannedFiles,
  ).sort(compareHits);
  const suppressions = loadInlineSuppressions(files.scannedFiles);
  const duploSuppression = applyInlineSuppressions(
    rawDuploHits,
    suppressions,
    "duplo",
  );
  const cpdSuppression = applyInlineSuppressions(
    rawCpdHits,
    suppressions,
    "cpd",
  );
  requireAtomicSuppressionUse([
    ...duploSuppression.suppressed,
    ...cpdSuppression.suppressed,
  ]);
  const hits = duploSuppression.hits.sort(compareHits);
  const structuralHitsAfterSuppression =
    cpdSuppression.hits.sort(compareHits);

  const report = buildReport({
    profile,
    files,
    duplo,
    cpd,
    codeOnlyDuplo,
    hits,
    structuralHitsAfterSuppression,
    duploSuppression,
    cpdSuppression,
    clock,
  });

  const outputPath = join(REPO_ROOT, profile.output);
  mkdirSync(dirname(outputPath), { recursive: true });
  rmSync(outputPath, { force: true });
  writeFileSync(outputPath, serializeReport(report), "utf8");

  const inventoryMessages =
    profile.name === "cpp"
      ? [
          `scanned ${files.scannedFiles.length} tracked C/C++/CUDA files`,
          `excluded ${files.excludedByPrefix.length} tracked third-party files`,
        ]
      : [
          `scanned ${files.scannedFiles.length} Iced Rust files`,
          `excluded ${files.excludedByPrefix.length + files.excludedGeneratedOutput.length} out-of-scope files`,
        ];
  console.log(
    [
      `wrote ${profile.output}`,
      ...inventoryMessages,
      `Duplo: ${report.summary.cross_file_hits} cross-file and ${report.summary.within_file_hits} within-file groups`,
      `CPD: ${report.structural_summary.cross_file_hits} cross-file and ${report.structural_summary.within_file_hits} within-file groups`,
      `timing: detectors ${externalSeconds.toFixed(1)}s, total ${((Date.now() - startedAt) / 1000).toFixed(1)}s`,
    ].join("\n"),
  );
  return report;
}

async function main() {
  const selection = parseArgs(process.argv.slice(2));
  if (selection.kind === "help") {
    console.log(usageText());
    return;
  }
  await generateReport(selection.profile);
}

const invokedPath = process.argv[1]
  ? pathToFileURL(process.argv[1]).href
  : undefined;
if (invokedPath === import.meta.url) {
  main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
