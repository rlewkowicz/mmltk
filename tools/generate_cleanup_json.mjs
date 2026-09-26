#!/usr/bin/env node

import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { availableParallelism, cpus, tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { classifyDeclarations, inventoryMacroConflicts, sourceOccurrence, lineStarts } from "./cleanup/declaration_patterns.mjs";
import { filterCpdCandidates, MAX_CPD_FILTER_TOKENS, sourceComments } from "./cleanup/cpd_patterns.mjs";
import { buildReview, renderReviewMarkdown } from "./cleanup/review_patterns.mjs";

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
    minLines: 6,
    flags: ["-ip"],
    contentFilter:
      "comments, strings, and preprocessor-only lines excluded",
    filterNonCode: true,
  },
  cpd: {
    binary: PMD_BINARY,
    language: "cpp",
    minTokens: 39,
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
    `  --cpp       C/C++/CUDA: Duplo ${CPP_PROFILE.duplo.minLines} lines; C++ CPD ${CPP_PROFILE.cpd.minTokens} tokens`,
    `  --frontend  Iced Rust: Duplo ${FRONTEND_PROFILE.duplo.minLines} lines; Rust CPD ${FRONTEND_PROFILE.cpd.minTokens} tokens`,
    "  --raw-cpd [--review] [--min-tokens N] [--output PATH]  Standalone unfiltered C++ CPD (default 12 tokens)",
    "  Raw mode retains every match, ignores no literal sequences/blocks or inline suppressions,",
    "  and reports original source spans separately from declaration classification.",
    "  --review adds authored-file triage and unreviewed lexical families, plus a Markdown sibling.",
    "  -h, --help  Show this help",
  ].join("\n");
}

export function rawCpdProfile(minTokens = 12) {
  if (!Number.isSafeInteger(minTokens) || minTokens < 2) throw new Error("--min-tokens must be an integer >= 2");
  return { ...CPP_PROFILE, cpd: { ...CPP_PROFILE.cpd, minTokens,
    raw: true, flags: ["--skip-blocks-pattern", ""] } };
}

export function parseArgs(args) {
  if (args.length === 2 && args[0] === "--raw-cpd" && ["--help", "-h"].includes(args[1])) return { kind: "help" };
  if (args[0] === "--raw-cpd") {
    let minTokens = 12;
    let output, review = false;
    const seen = new Set();
    for (let index = 1; index < args.length; ++index) {
      const option = args[index];
      if (!["--review", "--min-tokens", "--output"].includes(option) || seen.has(option)) {
        throw new Error("--raw-cpd accepts --review, --min-tokens N and --output PATH once each");
      }
      seen.add(option);
      if (option === "--review") { review = true; continue; }
      const value = args[++index];
      if (!value || value.startsWith("--")) throw new Error(`missing value for ${option}`);
      if (option === "--min-tokens") minTokens = Number(value);
      else output = value;
    }
    return { kind: "raw-cpd", profile: rawCpdProfile(minTokens), review,
      output: output ?? `cleanup/declarations-${review ? "review" : "raw"}.json` };
  }
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

export function authoredLanguage(path) {
  if (isCppPath(path)) return "cpp";
  if (path === "mmltk" || /\.(?:sh|bash)$/u.test(path)) return "shell";
  if (/(?:^|\/)(?:CMakeLists\.txt|[^/]+\.cmake(?:\.in)?)$/u.test(path)) return "cmake";
  if (/(?:^|\/)Dockerfile(?:\.[^/]+)?$/u.test(path)) return "dockerfile";
  return ({ rs: "rust", js: "javascript", mjs: "javascript", cjs: "javascript", jsx: "javascript", ts: "typescript", tsx: "typescript",
    py: "python", html: "html", css: "css", scss: "css" })[path.split(".").at(-1)] ?? null;
}

export const AUTHORED_PROFILE = {
  name: "authored", excludedPrefixes: [THIRD_PARTY_PREFIX],
  retainedGeneratedPaths: FRONTEND_PROFILE.retainedGeneratedPaths,
  generatedOutputPatterns: [/^(?:cleanup|output|outputs|compiled)(?:\/|$)/u,
    /(^|\/)(?:third_party|build|target|dist|generated|__pycache__|node_modules|\.cache|\.git)(?:\/|$)/u],
};

function isGeneratedFrontendOutput(path, profile) {
  return (
    !profile.retainedGeneratedPaths?.includes(path) &&
    profile.generatedOutputPatterns?.some((pattern) => pattern.test(path))
  );
}

export function profileIncludesPath(profile, path) {
  if (profile.name === "authored") return authoredLanguage(path) !== null;
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

export function collectInventoryInputs() {
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
  const residual = tag.replace(/\s+([\w:-]+)=(?:"([^"<]*)"|'([^'<]*)')/gu, (_match, name, double, single) => {
    const value = double ?? single;
    if (Object.hasOwn(attributes, name) || /&(?!(?:lt|gt|amp|quot|apos|#x[0-9a-f]+|#\d+);)/iu.test(value)) throw new Error("CPD returned invalid XML attributes");
    attributes[name] = decodeXmlAttribute(value);
    return "";
  });
  if (residual.trim()) throw new Error("CPD returned malformed XML attributes");
  return attributes;
}

function cpdDocument(xml) {
  const document = xml.trim().replace(/^<\?xml\s+[^?]*\?>\s*/u, "");
  const root = /^<pmd-cpd\b([^>]*?)(?:\/>|>([\s\S]*)<\/pmd-cpd>)$/u.exec(document);
  if (!root) throw new Error("CPD returned an incomplete or unrelated XML document");
  parseXmlAttributes(root[1]);
  // Code fragments may contain arbitrary source text inside CDATA. Remove only
  // complete, valid fragment payloads before inspecting the report structure.
  const body = (root[2] ?? "").replace(/<codefragment>(?:<!\[CDATA\[[\s\S]*?\]\]>|[^<&]|&(?:lt|gt|amp|quot|apos|#x[0-9a-f]+|#\d+);)*<\/codefragment>/gu, "<codefragment/>");
  if (/<(?:error|processing[-_]?error)\b/iu.test(body)) throw new Error(`CPD reported incomplete source coverage: ${body.slice(0, 1000)}`);
  return body;
}

export function parseCpdXml(xml, { expectedPaths, normalizePath = (path) => path } = {}) {
  let body = cpdDocument(xml);
  const duplications = [];
  body = body.replace(/<duplication\b([^>]*)>([\s\S]*?)<\/duplication>/gu, (whole, attributes, contents) => {
    const header = parseXmlAttributes(attributes);
    const lineCount = Number(header.lines), tokenCount = Number(header.tokens);
    if (![lineCount, tokenCount].every((value) => Number.isSafeInteger(value) && value > 0)) throw new Error("CPD returned an invalid duplication header");
    const occurrences = [];
    const remaining = contents.replace(/<file\b([^>]*)\/>/gu, (_file, attributes) => {
      const fields = parseXmlAttributes(attributes);
      const start = Number(fields.line), end = Number(fields.endline);
      const column = fields.column === undefined ? undefined : Number(fields.column);
      const endColumn = fields.endcolumn === undefined ? undefined : Number(fields.endcolumn);
      if (!fields.path || ![start, end].every(Number.isSafeInteger) || start < 1 || end < start ||
          [column, endColumn].some((value) => value !== undefined && (!Number.isSafeInteger(value) || value < 1))) throw new Error("CPD returned an invalid source range or column");
      occurrences.push({ path: normalizePath(fields.path), start, end,
        ...(column === undefined ? {} : { column }), ...(endColumn === undefined ? {} : { endColumn }) });
      return "";
    }).replace(/<codefragment\/>/u, "");
    if (remaining.trim() || occurrences.length < 2) throw new Error("CPD returned an incomplete duplication record");
    if (new Set(occurrences.map((entry) => JSON.stringify(entry))).size !== occurrences.length) throw new Error("CPD returned duplicate occurrence coordinates");
    duplications.push({ lineCount, tokenCount, occurrences });
    return "";
  });
  const coverage = new Set();
  body = body.replace(/<file\b([^>]*)\/>/gu, (_file, attributes) => {
    const fields = parseXmlAttributes(attributes), count = Number(fields.totalNumberOfTokens);
    if (!fields.path || !Number.isSafeInteger(count) || count < 0) throw new Error("CPD returned invalid source coverage");
    const path = normalizePath(fields.path);
    if (coverage.has(path)) throw new Error(`CPD returned duplicate source coverage: ${path}`);
    coverage.add(path);
    return "";
  });
  if (body.trim()) throw new Error("CPD returned malformed or unexpected XML content");
  if (expectedPaths) {
    const expected = new Set(expectedPaths);
    const missing = [...expected].filter((path) => !coverage.has(path));
    const unexpected = [...coverage].filter((path) => !expected.has(path));
    if (missing.length || unexpected.length) throw new Error(`CPD reported incomplete inventory coverage (${coverage.size}/${expected.size}); missing: ${missing.slice(0, 12).join(", ")}; unexpected: ${unexpected.slice(0, 12).join(", ")}`);
    for (const match of duplications) for (const { path } of match.occurrences) {
      if (!expected.has(path)) throw new Error(`CPD returned an out-of-inventory path: ${path}`);
    }
  }
  return duplications;
}

function unsuppressedCpdSource(path, source) {
  if (!source.includes("CPD-OFF") && !source.includes("CPD-ON")) return source;
  const pieces = [];
  let cursor = 0;
  for (const token of sourceComments(path, source)) {
    const text = token.text.replace(/CPD-O(?:FF|N)/gu, (marker) => `CPX${marker.slice(3)}`);
    if (text === token.text) continue;
    // Only comment markers change, with identical byte/character counts. PMD
    // coordinates therefore still address the original source, including CRLF.
    pieces.push(source.slice(cursor, token.offset), text);
    cursor = token.end;
  }
  pieces.push(source.slice(cursor));
  return pieces.join("");
}

export function sourceSnapshot(paths) {
  return new Map(paths.map((path) => {
    const bytes = readFileSync(path), source = bytes.toString("utf8");
    if (!Buffer.from(source).equals(bytes)) throw new Error(`source is not valid UTF-8: ${path}`);
    return [path, { source, sha256: createHash("sha256").update(bytes).digest("hex") }];
  }));
}

export function requireUnchangedSources(snapshot) {
  for (const [path, { sha256 }] of snapshot) {
    if (!existsSync(path) || createHash("sha256").update(readFileSync(path)).digest("hex") !== sha256) throw new Error(`source changed during analysis: ${path}`);
  }
}

export async function runCpd(profile, paths, snapshot = sourceSnapshot(paths)) {
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
      excludedLexerCount: paths.length,
      status: 0,
      stderr: "",
    };
  }

  const temporaryDirectory = mkdtempSync(join(tmpdir(), "cpd-file-list-"));
  const fileList = join(temporaryDirectory, "files.txt");
  const args = detectorArguments(profile, { fileList }).cpd;
  const originalPaths = new Map();
  let result;
  try {
    const inputs = lexable.map((path, index) => {
      const source = snapshot.get(path).source;
      const unsuppressed = unsuppressedCpdSource(path, source);
      // PMD's extension admission still applies to explicit file lists. Use
      // the selected lexer's suffix so CUDA, module and template sources are
      // actually covered, then restore their original identities below.
      const copy = join(temporaryDirectory, `${index}.${profile.cpd.language === "rust" ? "rs" : "cpp"}`);
      writeFileSync(copy, unsuppressed, "utf8");
      originalPaths.set(copy, resolve(REPO_ROOT, path));
      return copy;
    });
    writeFileSync(fileList, `${inputs.join("\n")}\n`, "utf8");
    result = await runAsync(
      profile.cpd.binary,
      args,
      {},
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
  if (/\b(?:error|fatal|\w*exception)\b/iu.test(stderr)) {
    throw new Error(
      `CPD could not lex a ${profile.name} source outside the configured exclusions:\n${stderr}`,
    );
  }
  const duplications = parseCpdXml(result.stdout, {
    expectedPaths: lexable.map((path) => resolve(REPO_ROOT, path)),
    normalizePath: (path) => {
      const original = originalPaths.get(path);
      if (original === undefined) throw new Error(`CPD returned an unlisted detector input: ${path}`);
      return original;
    },
  });
  const indexedSources = new Map([...snapshot].map(([path, entry]) => [resolve(REPO_ROOT, path), { ...entry, lines: lineStarts(entry.source) }]));
  for (const match of duplications) for (const occurrence of match.occurrences) {
    const entry = indexedSources.get(occurrence.path);
    sourceOccurrence(occurrence.path, entry.source, occurrence, entry.lines);
  }
  requireUnchangedSources(snapshot);
  return {
    duplications,
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
      const hit = makeHit(
        duplication.lineCount,
        duplication.tokenCount,
        rangesByFile,
        true,
      );
      if (duplication.patterns) hit.patterns = duplication.patterns;
      return hit;
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
  for (const hit of hits) {
    for (const file of hit.files) {
      affectedFiles.add(file.path);
    }
  }
  return {
    hit_count: hits.length,
    cross_file_hits: hits.filter((hit) => hit.kind === "cross-file").length,
    within_file_hits: hits.filter((hit) => hit.kind === "within-file").length,
    affected_file_count: affectedFiles.size,
  };
}

export function parseInlineSuppressions(path, source) {
  const byPath = new Map();
  const lines = source.split("\n");
  const suppressions = [];
  let openRange = null;
  const starts = lineStarts(source);
  let line = 0;
  for (const comment of sourceComments(path, source)) {
    if (!comment.text.startsWith("//")) continue;
    while (starts[line + 1] <= comment.offset) ++line;
    const index = line;
    const ignore = comment.text.match(/^\/\/\s*CLEANUP-IGNORE:\s*(\S(?:.*\S)?)\s*$/u);
    if (ignore) {
      const prefix = source.slice(starts[index], comment.offset).trim();
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

    const off = comment.text.match(
      /^\/\/\s*(CLEANUP|CPD)-OFF:\s*(\S(?:.*\S)?)\s*$/u,
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

    const on = comment.text.match(/^\/\/\s*(CLEANUP|CPD)-ON\b/u);
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
      const retained = makeHit(
          hit.line_count,
          hit.token_count,
          rangesByFile,
          false,
        );
      if (hit.patterns) retained.patterns = hit.patterns;
      remaining.push(retained);
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
      raw_hits: duplo.hits,
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
  const concise = (hit) => ({
    ...(hit.token_count === undefined ? { line_count: hit.line_count } : { token_count: hit.token_count }),
    ...(hit.patterns === undefined ? {} : { patterns: hit.patterns }),
    files: hit.files,
  });
  return `${JSON.stringify({
    hits: report.hits.map(concise),
    structural_hits: report.structural_hits.map(concise),
  }, null, 2)}\n`;
}

export function rejectionReport(previous, report, filteredCpd) {
  const { hits, structural_hits, ...diagnostics } = report;
  return {
    ...previous,
    [report.profile]: {
      ...diagnostics,
      cpd_candidate_filter: {
        maximum_tokens: report.profile === "cpp" ? MAX_CPD_FILTER_TOKENS : null,
        indexed_files: filteredCpd.indexedFiles,
        rejected: filteredCpd.filtered.map((duplication) => ({
          reason: duplication.reason,
          token_count: duplication.tokenCount,
          occurrences: duplication.occurrences.map((occurrence) => ({
            ...occurrence, path: repoRelativePath(occurrence.path),
          })),
        })),
      },
    },
  };
}

export function writeAtomic(path, contents) {
  mkdirSync(dirname(path), { recursive: true });
  const directory = mkdtempSync(join(dirname(path), ".cleanup-"));
  const temporary = join(directory, basename(path));
  try {
    writeFileSync(temporary, contents, "utf8");
    renameSync(temporary, path);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
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
  const snapshot = sourceSnapshot(files.scannedFiles);
  const duploPaths = files.scannedFiles.filter(
    (path) =>
      !profile.detectorExcludedPrefixes.some((prefix) =>
        path.startsWith(prefix),
      ),
  );

  const externalStarted = Date.now();
  const [duplo, cpd] = await Promise.all([
    runDuplo(profile, duploPaths),
    runCpd(profile, files.scannedFiles, snapshot),
  ]);
  const externalSeconds = (Date.now() - externalStarted) / 1000;

  const codeOnlyDuplo = filterDuploCandidates(profile, duplo.hits);
  const rawDuploHits = compactDuploHits(
    profile,
    codeOnlyDuplo.hits,
    files.scannedFiles,
  ).sort(compareHits);
  const filteredCpd = profile.name === "cpp"
    ? filterCpdCandidates(cpd.duplications)
    : { duplications: cpd.duplications, filtered: [], reasons: {}, indexedFiles: 0 };
  const rawCpdHits = structuralHits(
    profile,
    filteredCpd.duplications,
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
  const rejectedPath = join(dirname(outputPath), "rejected.json");
  const previous = existsSync(rejectedPath) ? JSON.parse(readFileSync(rejectedPath, "utf8")) : {};
  const rejectedContents = `${JSON.stringify(rejectionReport(previous, report, filteredCpd), null, 2)}\n`;
  const contents = serializeReport(report);
  requireUnchangedSources(snapshot);
  writeAtomic(rejectedPath, rejectedContents);
  writeAtomic(outputPath, contents);

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
      ...(profile.name === "cpp" ? [`CPD candidate filter: ${filteredCpd.filtered.length} rejected; details in cleanup/rejected.json`] : []),
      `timing: detectors ${externalSeconds.toFixed(1)}s, total ${((Date.now() - startedAt) / 1000).toFixed(1)}s`,
    ].join("\n"),
  );
  return report;
}

export function rawCpdReport(profile, files, cpd, sourceReader = (path) => readFileSync(path, "utf8")) {
  if (cpd.status !== 0) throw new Error(`CPD raw scan exited with status ${cpd.status}\n${cpd.stderr}`);
  // One read per file, one classified occurrence per real source range. Source
  // context is not another detector and never increases CPD's occurrence count.
  const sources = new Map(files.scannedFiles.map((path) => [path, sourceReader(path)]));
  const lines = new Map([...sources].map(([path, source]) => [path, lineStarts(source)]));
  const macroConflicts = inventoryMacroConflicts(sources);
  const classified = files.scannedFiles.flatMap((path) => classifyDeclarations(path, sources.get(path), { macroConflicts }).candidates);
  return {
    format: 1,
    purpose: "Unfiltered CPD evidence and independent declaration context; not a behavior-duplication verdict",
    inventory: files.scannedFiles,
    detector: { binary: profile.cpd.binary, arguments: detectorArguments(profile).cpd,
      min_tokens: profile.cpd.minTokens, status: cpd.status, stderr: cpd.stderr,
      context_filter: false, inline_suppressions: false, sequence_skipping: false },
    raw_matches: cpd.duplications.map((match) => ({ ...match,
      occurrences: [...match.occurrences].sort((a, b) => a.path.localeCompare(b.path, "en") || a.start - b.start || (a.column ?? 1) - (b.column ?? 1) || a.end - b.end || (a.endColumn ?? 0) - (b.endColumn ?? 0)),
    })).sort((a, b) => a.occurrences[0].path.localeCompare(b.occurrences[0].path, "en") || a.occurrences[0].start - b.occurrences[0].start ||
      (a.occurrences[0].column ?? 1) - (b.occurrences[0].column ?? 1) || a.tokenCount - b.tokenCount || JSON.stringify(a).localeCompare(JSON.stringify(b), "en"))
      .map((match, index) => ({
      id: index + 1, token_count: match.tokenCount, line_count: match.lineCount,
      occurrences: match.occurrences.map((occurrence) => {
        const path = repoRelativePath(occurrence.path);
        if (!sources.has(path)) throw new Error(`CPD returned an out-of-inventory path: ${path}`);
        return sourceOccurrence(path, sources.get(path), occurrence, lines.get(path));
      }),
    })),
    classified_context: classified,
    summary: { files: sources.size, raw_matches: cpd.duplications.length, classified_candidates: classified.length },
  };
}

export async function generateRawCpd(profile, output, { review = false, inventoryInputs = collectInventoryInputs(), renderMarkdown = renderReviewMarkdown } = {}) {
  if (!output.endsWith(".json")) throw new Error("raw CPD output must be a .json report path");
  const files = buildInventory(profile, inventoryInputs);
  const authored = review ? buildInventory(AUTHORED_PROFILE, inventoryInputs).scannedFiles : [];
  const snapshot = sourceSnapshot([...new Set([...files.scannedFiles, ...authored])]);
  const cpd = await runCpd(profile, files.scannedFiles, snapshot);
  const report = rawCpdReport(profile, files, cpd, (path) => snapshot.get(path).source);
  if (review) report.review = buildReview(authored.map((path) => ({ path, language: authoredLanguage(path), ...snapshot.get(path) })), report);
  const contents = `${JSON.stringify(report, null, 2)}\n`;
  const markdown = review ? renderMarkdown(report.review) : null;
  if (review && typeof markdown !== "string") throw new Error("review renderer did not produce Markdown text");
  requireUnchangedSources(snapshot);
  writeAtomic(output, contents);
  if (review) writeAtomic(output.slice(0, -5) + ".md", markdown);
  console.log(`wrote ${output}: ${report.summary.files} files, ${report.summary.raw_matches} raw CPD groups, ${report.summary.classified_candidates} source candidates`);
  return report;
}

async function main() {
  const selection = parseArgs(process.argv.slice(2));
  if (selection.kind === "help") {
    console.log(usageText());
    return;
  }
  if (selection.kind === "raw-cpd") await generateRawCpd(selection.profile, selection.output, { review: selection.review });
  else await generateReport(selection.profile);
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
