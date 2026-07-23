#!/usr/bin/env node

import { mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { availableParallelism, cpus } from "node:os";
import { spawnSync } from "node:child_process";

const REPO_ROOT = process.cwd();
const DEFAULT_MIN_LINES = 7;
const PYX_MIN_LINES = 10;
const OUTPUT_DIR = join(REPO_ROOT, "cleanup");
const DUPLO_BINARY = "/bin/duplo";
const THREADS = Math.max(
  1,
  typeof availableParallelism === "function"
    ? availableParallelism()
    : cpus().length,
);
const OUTPUT_INDENT = 2;
const EXCLUDED_DUPLICATION_FILE_SUFFIXES = [".patch"];
const EXCLUDED_DUPLICATION_PATH_PREFIXES = [
  ".agents/skills/browser-trace/.o11y/",
  "model/artifacts/",
  "model/experiments/",
  "vendor/llama.cpp/vendor/",
];
const EXCLUDED_NON_CUDA_DUPLICATION_PATH_PREFIXES = [
  "vendor/llama.cpp/ggml/",
];
const EXCLUDED_DUPLICATION_PATHS = [];
const JAVASCRIPT_EXTENSIONS = new Set([".cjs", ".js", ".jsx", ".mjs"]);
const TYPESCRIPT_EXTENSIONS = new Set([".cts", ".d.ts", ".mts", ".ts", ".tsx"]);
const PYTHON_EXTENSIONS = new Set([".py", ".pyi", ".pyw"]);
const CYTHON_EXTENSIONS = new Set([".pyx"]);
const CYTHON_INCLUDE_EXTENSIONS = new Set([".pxi"]);
const MODEL_PY_PXI_EXTENSIONS = [".py", ".pxi"];
const STYLESHEET_EXTENSIONS = new Set([".css", ".scss"]);
const CPP_EXTENSIONS = new Set([
  ".c",
  ".cc",
  ".cpp",
  ".cxx",
  ".h",
  ".hh",
  ".hpp",
  ".hxx",
]);
const CUDA_EXTENSIONS = new Set([".cu", ".cuh"]);
const OBJECTIVE_C_EXTENSIONS = new Set([".m", ".mm"]);
const OPENCL_EXTENSIONS = new Set([".cl"]);
const GLSL_EXTENSIONS = new Set([".comp", ".glsl"]);
const WGSL_EXTENSIONS = new Set([".wgsl"]);
const METAL_EXTENSIONS = new Set([".metal"]);
const CMAKE_EXTENSIONS = new Set([".cmake"]);
const IDL_EXTENSIONS = new Set([".idl"]);
const CODE_EXTENSIONS = new Set([
  ...JAVASCRIPT_EXTENSIONS,
  ...TYPESCRIPT_EXTENSIONS,
  ...PYTHON_EXTENSIONS,
  ...CYTHON_EXTENSIONS,
  ...CYTHON_INCLUDE_EXTENSIONS,
  ...STYLESHEET_EXTENSIONS,
  ...CPP_EXTENSIONS,
  ...CUDA_EXTENSIONS,
  ...OBJECTIVE_C_EXTENSIONS,
  ...OPENCL_EXTENSIONS,
  ...GLSL_EXTENSIONS,
  ...WGSL_EXTENSIONS,
  ...METAL_EXTENSIONS,
  ...CMAKE_EXTENSIONS,
  ...IDL_EXTENSIONS,
  ".html",
  ".sh",
]);
const JAVASCRIPT_PATH_PATTERNS = [
  /^test\/fixtures\/harness(?:\/.*)?\/scripts\/[^/.]+$/u,
];
const CMAKE_PATH_PATTERNS = [/(^|\/)CMakeLists\.txt$/u];
const CPP_TEMPLATE_SUFFIXES = [
  ".c.in",
  ".cc.in",
  ".cpp.in",
  ".cxx.in",
  ".h.in",
  ".hh.in",
  ".hpp.in",
  ".hxx.in",
];
const CMAKE_TEMPLATE_SUFFIXES = [".cmake.in"];
const WGSL_TEMPLATE_PATH_PATTERNS = [/(^|\/)wgsl-shaders\/[^/]+\.tmpl$/u];
const COMPATIBLE_FILE_TYPES = [
  "cmake",
  "cpp",
  "cuda",
  "cython",
  "glsl",
  "html",
  "idl",
  "javascript",
  "metal",
  "objective_c",
  "opencl",
  "python",
  "shell",
  "stylesheet",
  "typescript",
  "wgsl",
];
const PYTHON_ONLY_SCAN = {
  id: "python_code",
  includeCodeFiles: true,
  includeCompatibleFileTypes: ["python"],
};
const WHOLE_CODEBASE_SCAN = {
  id: "whole_codebase_code",
  includeCodeFiles: true,
};
const WHOLE_CODEBASE_ARTIFACT = {
  number: 1,
  title: "Whole Codebase Code Deduplication",
  filename: "code-deduplication.json",
  scanId: "whole_codebase_code",
  filter: "all",
};
const PYTHON_ONLY_ARTIFACT = {
  number: 1,
  title: "Python Code Deduplication",
  filename: "python-code-deduplication.json",
  scanId: "python_code",
  filter: "all",
};
const PYX_ONLY_SCAN = {
  id: "pyx_code",
  includeCodeFiles: true,
  includeCompatibleFileTypes: ["cython"],
  includeExtensions: [".pyx"],
  minLines: PYX_MIN_LINES,
};
const PYX_ONLY_ARTIFACT = {
  number: 1,
  title: "Cython Pyx Code Deduplication",
  filename: "pyx-code-deduplication.json",
  scanId: "pyx_code",
  filter: "all",
};
const MODEL_PY_PXI_SCAN = {
  id: "model_py_pxi_code",
  includeRoots: ["model/"],
  includeCodeFiles: true,
  includeExtensions: MODEL_PY_PXI_EXTENSIONS,
  minLines: PYX_MIN_LINES,
  mixCompatibleFileTypes: true,
};
const MODEL_PY_PXI_ARTIFACT = {
  number: 1,
  title: "Model Python And Pxi Deduplication",
  filename: "model-py-pxi-deduplication.json",
  scanId: "model_py_pxi_code",
  filter: "all",
};
const PYTHON_IMPORT_PROLOGUE_IGNORE_REASON = "python_import_prologue";
const PYTHON_IMPORT_PROLOGUE_TAG = "python import/prologue duplication ignored";

const SCANS = [
  {
    id: "app_runtime_code",
    includeRoots: ["src/", "entrypoints/"],
    includeCodeFiles: true,
  },
  {
    id: "test_and_harness_code",
    includeRoots: ["test/"],
    includeCodeFiles: true,
  },
  {
    id: "tooling_config_code",
    includeRoots: ["scripts/", "agent-scripts/", ".agents/skills/"],
    includeTopLevel: true,
    includeCodeFiles: true,
  },
  {
    id: "model_python_code",
    includeRoots: ["model/"],
    includeCodeFiles: true,
    includeCompatibleFileTypes: ["python"],
  },
  {
    id: "cross_repo_compatible_code",
    includeCodeFiles: true,
  },
  {
    id: "cpp_code",
    includeCodeFiles: true,
    includeCompatibleFileTypes: ["cpp", "cuda"],
  },
];

const DEFAULT_MODE = "default";
const PYTHON_MODE = "python";
const PYX_MODE = "pyx";
const MODEL_MODE = "model";

const PHASE_FILES = [
  {
    number: 1,
    title: "App Runtime Code Deduplication",
    filename: "phase-1-app-runtime-code-deduplication.json",
    scanId: "app_runtime_code",
    filter: "all",
  },
  {
    number: 2,
    title: "Test And Harness Code Deduplication",
    filename: "phase-2-test-and-harness-code-deduplication.json",
    scanId: "test_and_harness_code",
    filter: "all",
  },
  {
    number: 3,
    title: "Tooling And Config Code Deduplication",
    filename: "phase-3-tooling-and-config-code-deduplication.json",
    scanId: "tooling_config_code",
    filter: "all",
  },
  {
    number: 4,
    title: "Model Training Python Deduplication",
    filename: "phase-4-model-python-deduplication.json",
    scanId: "model_python_code",
    filter: "all",
  },
  {
    number: 5,
    title: "Cross Repo Compatible Code Deduplication",
    filename: "phase-5-cross-repo-compatible-code-deduplication.json",
    scanId: "cross_repo_compatible_code",
    filter: "all",
  },
  {
    number: 6,
    title: "C++ Code Deduplication",
    filename: "phase-6-cpp-code-deduplication.json",
    scanId: "cpp_code",
    filter: "all",
  },
];

function runCommand(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: REPO_ROOT,
    encoding: "utf8",
    maxBuffer: 256 * 1024 * 1024,
    ...options,
  });

  if (result.error) {
    throw result.error;
  }

  return result;
}

function parseArgs(argv) {
  let mode = DEFAULT_MODE;

  for (const arg of argv) {
    if (arg === "--python") {
      if (mode !== DEFAULT_MODE) {
        throw new Error("choose only one cleanup mode");
      }
      mode = PYTHON_MODE;
      continue;
    }

    if (arg === "--pyx") {
      if (mode !== DEFAULT_MODE) {
        throw new Error("choose only one cleanup mode");
      }
      mode = PYX_MODE;
      continue;
    }

    if (arg === "--model") {
      if (mode !== DEFAULT_MODE) {
        throw new Error("choose only one cleanup mode");
      }
      mode = MODEL_MODE;
      continue;
    }

    if (arg === "--help" || arg === "-h") {
      return { help: true, mode };
    }

    throw new Error(`unknown argument: ${arg}`);
  }

  return { help: false, mode };
}

function printUsage() {
  console.log(
    [
      "Usage:",
      "  node scripts/generate_cleanup_json.mjs",
      "  node scripts/generate_cleanup_json.mjs --python",
      "  node scripts/generate_cleanup_json.mjs --pyx",
      "  node scripts/generate_cleanup_json.mjs --model",
      "",
      "Options:",
      "  default   Generate one whole-codebase cleanup artifact grouped by compatible code type.",
      "  --python  Generate one Python-only cleanup artifact with cross-file hits first, then same-file hits.",
      "  --pyx     Generate one Cython .pyx cleanup artifact using a 10-line duplicate threshold.",
      "  --model   Generate one model .py/.pxi cleanup artifact using a 10-line duplicate threshold.",
    ].join("\n"),
  );
}

let trackedFileInventory;
let trackedFileRecordByPath;
const syntheticFileRecordCache = new Map();
const sourceFileLineCache = new Map();
const pythonFirstExecutableLineCache = new Map();

function getTrackedFileInventory() {
  if (trackedFileInventory) {
    return trackedFileInventory;
  }

  const result = runCommand("git", ["ls-files", "-z"]);
  if (result.status !== 0) {
    throw new Error(`git ls-files failed:\n${result.stderr}`);
  }

  trackedFileInventory = result.stdout
    .split("\0")
    .filter(Boolean)
    .map(buildFileRecord);
  trackedFileRecordByPath = new Map(
    trackedFileInventory.map((record) => [record.path, record]),
  );

  return trackedFileInventory;
}

function getDuplicationFileInventory() {
  return getTrackedFileInventory().filter((record) => !record.excluded);
}

function fileRecordForPath(path) {
  const trackedRecord = trackedFileRecordByPath?.get(path);
  if (trackedRecord) {
    return trackedRecord;
  }

  let syntheticRecord = syntheticFileRecordCache.get(path);
  if (!syntheticRecord) {
    syntheticRecord = buildFileRecord(path);
    syntheticFileRecordCache.set(path, syntheticRecord);
  }

  return syntheticRecord;
}

function sourceLinesForPath(path) {
  let lines = sourceFileLineCache.get(path);
  if (!lines) {
    lines = readFileSync(join(REPO_ROOT, path), "utf8").split(/\r?\n/u);
    sourceFileLineCache.set(path, lines);
  }

  return lines;
}

function stripPythonLineComment(line) {
  const commentStart = line.indexOf("#");
  return commentStart < 0 ? line : line.slice(0, commentStart);
}

function pythonParenDelta(line) {
  const source = stripPythonLineComment(line);
  const opens = source.match(/\(/gu)?.length ?? 0;
  const closes = source.match(/\)/gu)?.length ?? 0;
  return opens - closes;
}

function isPythonTripleQuotedStringStart(trimmedLine) {
  const match = trimmedLine.match(/^[rRuUbBfF]*("""|''')/u);
  if (!match) {
    return undefined;
  }

  return match[1];
}

function isPythonImportStart(trimmedLine) {
  return (
    /^import\s+[A-Za-z_][\w.]*/u.test(trimmedLine) ||
    /^from\s+[.\w]+\s+import\b/u.test(trimmedLine)
  );
}

function pythonFirstExecutableLineNumber(path) {
  let cached = pythonFirstExecutableLineCache.get(path);
  if (cached !== undefined) {
    return cached;
  }

  const lines = sourceLinesForPath(path);
  let moduleDocstringAllowed = true;
  let docstringDelimiter;
  let importParenDepth = 0;
  let importContinuesWithBackslash = false;

  for (let index = 0; index < lines.length; index += 1) {
    const trimmed = lines[index].trim();
    const lineNumber = index + 1;

    if (docstringDelimiter) {
      if (trimmed.includes(docstringDelimiter)) {
        docstringDelimiter = undefined;
      }
      continue;
    }

    if (importParenDepth > 0 || importContinuesWithBackslash) {
      importParenDepth = Math.max(
        0,
        importParenDepth + pythonParenDelta(trimmed),
      );
      importContinuesWithBackslash = stripPythonLineComment(trimmed)
        .trimEnd()
        .endsWith("\\");
      continue;
    }

    if (!trimmed || trimmed.startsWith("#")) {
      continue;
    }

    if (moduleDocstringAllowed) {
      const delimiter = isPythonTripleQuotedStringStart(trimmed);
      if (delimiter) {
        moduleDocstringAllowed = false;
        if (trimmed.indexOf(delimiter, delimiter.length) < 0) {
          docstringDelimiter = delimiter;
        }
        continue;
      }
    }

    if (isPythonImportStart(trimmed)) {
      moduleDocstringAllowed = false;
      importParenDepth = Math.max(0, pythonParenDelta(trimmed));
      importContinuesWithBackslash = stripPythonLineComment(trimmed)
        .trimEnd()
        .endsWith("\\");
      continue;
    }

    cached = lineNumber;
    pythonFirstExecutableLineCache.set(path, cached);
    return cached;
  }

  cached = lines.length + 1;
  pythonFirstExecutableLineCache.set(path, cached);
  return cached;
}

function buildFileRecord(path) {
  const extension = extensionOf(path);
  const firstSlashIndex = path.indexOf("/");
  const isTopLevel = firstSlashIndex < 0;

  return {
    path,
    extension,
    compatibleFileType: compatibleFileTypeForPath(path, extension),
    repoPlane: repoPlane(path),
    excluded: isExcludedDuplicationFile(path),
    isTopLevel,
    rootName: isTopLevel ? "" : path.slice(0, firstSlashIndex),
    rootPrefix: isTopLevel ? "" : path.slice(0, firstSlashIndex + 1),
    testPath: isTestPath(path),
    toolingOrConfigPath: isToolingOrConfigPath(path),
  };
}

function buildScanInputs(scans = SCANS) {
  const inventory = getDuplicationFileInventory();
  const scanInputs = new Map();

  for (const scan of scans) {
    const records = inventory.filter((record) =>
      recordMatchesScan(record, scan),
    );
    const compatibleFileTypeFileLists =
      groupRecordsByCompatibleFileType(records);

    scanInputs.set(scan.id, {
      scan_id: scan.id,
      scan,
      min_lines: scan.minLines ?? DEFAULT_MIN_LINES,
      selection: scanInputDescription(scan),
      records,
      files: records.map((record) => record.path),
      tracked_file_count: records.length,
      compatible_file_type_file_lists: compatibleFileTypeFileLists,
      compatible_file_type_file_counts: Object.fromEntries(
        compatibleFileTypeFileLists.map(([fileType, files]) => [
          fileType,
          files.length,
        ]),
      ),
    });
  }

  return { inventory, scanInputs };
}

function recordMatchesScan(record, scan) {
  const fileType = record.compatibleFileType;

  if (scan.includeCodeFiles && !fileType) {
    return false;
  }

  if (
    scan.includeCompatibleFileTypes &&
    !scan.includeCompatibleFileTypes.includes(fileType)
  ) {
    return false;
  }

  if (scan.includePaths?.includes(record.path)) {
    return true;
  }

  if (scan.includeTopLevel && record.isTopLevel) {
    return true;
  }

  if (scan.includePaths && !scan.includeRoots && !scan.includeExtensions) {
    return false;
  }

  const extension = record.extension;
  if (scan.includeExtensions && !scan.includeExtensions.includes(extension)) {
    return false;
  }

  if (scan.excludeExtensions && scan.excludeExtensions.includes(extension)) {
    return false;
  }

  if (scan.includeRoots) {
    return scan.includeRoots.some((root) => record.path.startsWith(root));
  }

  return true;
}

function buildCodeFileCoverage(scanInputs, inventory) {
  const trackedCodeRecords = inventory.filter(
    (record) => record.compatibleFileType && !record.excluded,
  );
  const excludedTrackedCodeRecords = inventory.filter(
    (record) => record.compatibleFileType && record.excluded,
  );
  const trackedCodeFiles = trackedCodeRecords.map((record) => record.path);
  const scannedCodeFiles = new Set();

  for (const scanInput of scanInputs.values()) {
    for (const file of scanInput.files) {
      scannedCodeFiles.add(file);
    }
  }

  const missingTrackedCodeFiles = trackedCodeFiles.filter(
    (file) => !scannedCodeFiles.has(file),
  );

  if (missingTrackedCodeFiles.length > 0) {
    throw new Error(
      `cleanup scans do not cover all tracked code files:\n${missingTrackedCodeFiles.join(
        "\n",
      )}`,
    );
  }

  return {
    tracked_code_file_count: trackedCodeFiles.length,
    scanned_code_file_count: scannedCodeFiles.size,
    excluded_tracked_code_file_count: excludedTrackedCodeRecords.length,
    missing_tracked_code_files: [],
    compatible_file_type_counts: countBy(
      trackedCodeRecords,
      (record) => record.compatibleFileType ?? "unknown",
    ),
    code_extensions: Array.from(CODE_EXTENSIONS).sort(),
    compatible_file_types: COMPATIBLE_FILE_TYPES,
    cpp_template_suffixes: CPP_TEMPLATE_SUFFIXES,
    cmake_template_suffixes: CMAKE_TEMPLATE_SUFFIXES,
    javascript_path_patterns: JAVASCRIPT_PATH_PATTERNS.map(
      (pattern) => pattern.source,
    ),
    cmake_path_patterns: CMAKE_PATH_PATTERNS.map((pattern) => pattern.source),
    wgsl_template_path_patterns: WGSL_TEMPLATE_PATH_PATTERNS.map(
      (pattern) => pattern.source,
    ),
  };
}

function runDuplo(files, minLines = DEFAULT_MIN_LINES) {
  if (files.length === 0) {
    return { hits: [], status: 0, stderr: "" };
  }

  const result = runCommand(
    DUPLO_BINARY,
    ["-j", String(THREADS), "-ml", String(minLines), "-ip", "-json", "-", "-"],
    { input: files.join("\n") },
  );

  if (![0, 1].includes(result.status ?? 0)) {
    throw new Error(
      `duplo failed with status ${result.status}:\n${result.stderr}`,
    );
  }

  const parsedHits = result.stdout.trim() ? JSON.parse(result.stdout) : [];

  return {
    hits: Array.isArray(parsedHits) ? parsedHits : [],
    status: result.status ?? 0,
    stderr: result.stderr.trim(),
  };
}

const duploResultCache = new Map();

function duploCacheKey(fileType, minLines, files) {
  return `${fileType}\0${minLines}\0${files.join("\0")}`;
}

function runCachedDuplo(fileType, minLines, files) {
  const cacheKey = duploCacheKey(fileType, minLines, files);
  let result = duploResultCache.get(cacheKey);
  if (!result) {
    result = runDuplo(files, minLines);
    duploResultCache.set(cacheKey, result);
  }

  return result;
}

function groupRecordsByCompatibleFileType(records) {
  const groups = new Map();

  for (const record of records) {
    const fileType = record.compatibleFileType;
    if (!fileType) {
      throw new Error(
        `tracked scan file has no compatible file type: ${record.path}`,
      );
    }

    const group = groups.get(fileType) ?? [];
    group.push(record.path);
    groups.set(fileType, group);
  }

  return Array.from(groups.entries()).sort(([left], [right]) =>
    left.localeCompare(right),
  );
}

function runDuploByCompatibleFileType(scanInput) {
  if (scanInput.scan.mixCompatibleFileTypes) {
    const result = runCachedDuplo(
      scanInput.scan.id,
      scanInput.min_lines,
      scanInput.files,
    );

    return {
      hits: result.hits,
      status: result.status,
      statuses: { mixed: result.status },
      stderr: result.stderr ? { mixed: result.stderr } : {},
      compatible_file_type_file_counts:
        scanInput.compatible_file_type_file_counts,
    };
  }

  const hits = [];
  const statuses = {};
  const stderr = {};

  for (const [
    fileType,
    compatibleFiles,
  ] of scanInput.compatible_file_type_file_lists) {
    const result = runCachedDuplo(
      fileType,
      scanInput.min_lines,
      compatibleFiles,
    );
    hits.push(...result.hits);
    statuses[fileType] = result.status;

    if (result.stderr) {
      stderr[fileType] = result.stderr;
    }
  }

  return {
    hits,
    status: Object.values(statuses).includes(1) ? 1 : 0,
    statuses: sortedObject(statuses),
    stderr: sortedObject(stderr),
    compatible_file_type_file_counts:
      scanInput.compatible_file_type_file_counts,
  };
}

function isExcludedDuplicationFile(path) {
  const normalizedPath = path.toLowerCase();
  return (
    EXCLUDED_DUPLICATION_PATHS.includes(normalizedPath) ||
    EXCLUDED_DUPLICATION_FILE_SUFFIXES.some((suffix) =>
      normalizedPath.endsWith(suffix),
    ) ||
    EXCLUDED_DUPLICATION_PATH_PREFIXES.some((prefix) =>
      normalizedPath.startsWith(prefix),
    ) ||
    EXCLUDED_NON_CUDA_DUPLICATION_PATH_PREFIXES.some(
      (prefix) =>
        normalizedPath.startsWith(prefix) &&
        !CUDA_EXTENSIONS.has(extensionOf(normalizedPath)),
    )
  );
}

function hitReferencesExcludedFile(hit) {
  return (
    fileRecordForPath(hit.SourceFile1).excluded ||
    fileRecordForPath(hit.SourceFile2).excluded
  );
}

function extensionOf(path) {
  const basename = path.slice(path.lastIndexOf("/") + 1);
  const dottedExtensionMatch = basename.match(/\.d\.ts$/);
  if (dottedExtensionMatch) {
    return dottedExtensionMatch[0];
  }

  const extensionStart = basename.lastIndexOf(".");
  return extensionStart < 0 ? "" : basename.slice(extensionStart);
}

function compatibleFileType(path) {
  return compatibleFileTypeForPath(path, extensionOf(path));
}

function compatibleFileTypeForPath(path, extension) {
  if (JAVASCRIPT_PATH_PATTERNS.some((pattern) => pattern.test(path))) {
    return "javascript";
  }
  if (PYTHON_EXTENSIONS.has(extension)) {
    return "python";
  }
  if (
    CYTHON_EXTENSIONS.has(extension) ||
    CYTHON_INCLUDE_EXTENSIONS.has(extension)
  ) {
    return "cython";
  }
  if (JAVASCRIPT_EXTENSIONS.has(extension)) {
    return "javascript";
  }
  if (TYPESCRIPT_EXTENSIONS.has(extension)) {
    return "typescript";
  }
  if (STYLESHEET_EXTENSIONS.has(extension)) {
    return "stylesheet";
  }
  if (extension === ".html") {
    return "html";
  }
  if (extension === ".sh") {
    return "shell";
  }
  if (
    CPP_EXTENSIONS.has(extension) ||
    CPP_TEMPLATE_SUFFIXES.some((suffix) => path.endsWith(suffix))
  ) {
    return "cpp";
  }
  if (CUDA_EXTENSIONS.has(extension)) {
    return "cuda";
  }
  if (OBJECTIVE_C_EXTENSIONS.has(extension)) {
    return "objective_c";
  }
  if (OPENCL_EXTENSIONS.has(extension)) {
    return "opencl";
  }
  if (GLSL_EXTENSIONS.has(extension)) {
    return "glsl";
  }
  if (
    WGSL_EXTENSIONS.has(extension) ||
    WGSL_TEMPLATE_PATH_PATTERNS.some((pattern) => pattern.test(path))
  ) {
    return "wgsl";
  }
  if (METAL_EXTENSIONS.has(extension)) {
    return "metal";
  }
  if (
    CMAKE_EXTENSIONS.has(extension) ||
    CMAKE_PATH_PATTERNS.some((pattern) => pattern.test(path)) ||
    CMAKE_TEMPLATE_SUFFIXES.some((suffix) => path.endsWith(suffix))
  ) {
    return "cmake";
  }
  if (IDL_EXTENSIONS.has(extension)) {
    return "idl";
  }

  return undefined;
}

function hitHasCompatibleFileType(hit, scan) {
  const firstFileType = fileRecordForPath(hit.SourceFile1).compatibleFileType;
  const secondFileType = fileRecordForPath(hit.SourceFile2).compatibleFileType;

  if (!firstFileType || !secondFileType) {
    return false;
  }

  return Boolean(
    scan.mixCompatibleFileTypes || firstFileType === secondFileType,
  );
}

function repoPlane(path) {
  if (path.startsWith("vendor/llama.cpp/")) {
    return "vendor/llama.cpp";
  }
  if (path.startsWith(".agents/skills/")) {
    return ".agents/skills";
  }
  if (!path.includes("/")) {
    return "repo_root";
  }

  return path.slice(0, path.indexOf("/"));
}

function crossFilePlane(sourceFileRecords) {
  if (sourceFileRecords.length <= 1) {
    return "within_file";
  }

  const planes = Array.from(
    new Set(sourceFileRecords.map((record) => record.repoPlane)),
  );
  return planes.length === 1 && planes[0] !== "repo_root"
    ? planes[0]
    : "repo_root";
}

function incrementCount(counts, key, amount = 1) {
  counts[key] = (counts[key] ?? 0) + amount;
}

function sortedObject(value) {
  return Object.fromEntries(
    Object.entries(value).sort(([left], [right]) => left.localeCompare(right)),
  );
}

function countBy(values, keyForValue) {
  const counts = {};

  for (const value of values) {
    incrementCount(counts, keyForValue(value));
  }

  return sortedObject(counts);
}

function isTestPath(path) {
  return (
    /(^|\/)(test|tests)(\/|$)/.test(path) ||
    /(^|\/)(test_|tests_)/.test(path) ||
    /(_test|_tests)\.[^/]+$/.test(path)
  );
}

function isToolingOrConfigPath(path) {
  return (
    path.startsWith("scripts/") ||
    path.endsWith(".config.ts") ||
    path === "env.d.ts"
  );
}

function hitRangeIsPythonImportPrologue(path, startLine, endLine) {
  const fileType = fileRecordForPath(path).compatibleFileType;
  if (fileType !== "python") {
    return false;
  }

  const start = Number(startLine);
  const end = Number(endLine);
  if (!Number.isInteger(start) || !Number.isInteger(end) || start < 1) {
    return false;
  }

  return end < pythonFirstExecutableLineNumber(path);
}

function isPythonImportPrologueHit(hit) {
  return (
    hit.compatible_file_type === "python" &&
    hitRangeIsPythonImportPrologue(
      hit.SourceFile1,
      hit.StartLineNumber1,
      hit.EndLineNumber1,
    ) &&
    hitRangeIsPythonImportPrologue(
      hit.SourceFile2,
      hit.StartLineNumber2,
      hit.EndLineNumber2,
    )
  );
}

function annotateHit(hit) {
  const sameFile = hit.SourceFile1 === hit.SourceFile2;
  const sourceFiles = Array.from(
    new Set([hit.SourceFile1, hit.SourceFile2]),
  ).sort();
  const sourceFileRecords = sourceFiles.map(fileRecordForPath);
  const sourceCompatibleFileTypes = Array.from(
    new Set(
      sourceFileRecords
        .map((record) => record.compatibleFileType)
        .filter(Boolean),
    ),
  ).sort();
  const compatibleFileTypeName =
    sourceCompatibleFileTypes.length === 1
      ? sourceCompatibleFileTypes[0]
      : "mixed";
  const plane = crossFilePlane(sourceFileRecords);
  const tags = [
    sameFile ? "within-file duplication" : "cross-file duplication",
  ];
  const firstFileRecord = fileRecordForPath(hit.SourceFile1);
  const secondFileRecord = fileRecordForPath(hit.SourceFile2);

  if (firstFileRecord.testPath && secondFileRecord.testPath) {
    tags.push("test-only duplication");
  }

  if (
    firstFileRecord.toolingOrConfigPath &&
    secondFileRecord.toolingOrConfigPath
  ) {
    tags.push("tooling/config duplication");
  }

  let primaryTag = sameFile
    ? "within-file duplication"
    : "cross-file duplication";
  if (tags.includes("test-only duplication")) {
    primaryTag = "test-only duplication";
  } else if (tags.includes("tooling/config duplication")) {
    primaryTag = "tooling/config duplication";
  }

  const annotatedHit = {
    ...hit,
    same_file: sameFile,
    source_files: sourceFiles,
    compatible_file_type: compatibleFileTypeName,
    source_compatible_file_types: sourceCompatibleFileTypes,
    source_language_families: sourceCompatibleFileTypes,
    cross_file_plane: plane,
    primary_tag: primaryTag,
    tags,
  };

  if (isPythonImportPrologueHit(annotatedHit)) {
    annotatedHit.cleanup_ignore_reason = PYTHON_IMPORT_PROLOGUE_IGNORE_REASON;
    annotatedHit.tags = [...annotatedHit.tags, PYTHON_IMPORT_PROLOGUE_TAG];
  }

  return annotatedHit;
}

function summarizeHits(hits) {
  const summary = {
    hit_count: hits.length,
    duplicated_block_lines: 0,
    within_file_hits: 0,
    cross_file_hits: 0,
    unique_file_count: 0,
    primary_tag_counts: {},
    compatible_file_type_counts: {},
    cross_file_plane_counts: {},
  };
  const uniqueFiles = new Set();

  for (const hit of hits) {
    const lineCount = hit.line_count ?? hit.LineCount ?? 0;
    const sameFile = hit.kind
      ? hit.kind === "within-file"
      : Boolean(hit.same_file);
    const sourceFiles = hit.files
      ? hit.files.map((file) => file.path)
      : (hit.source_files ?? []);
    summary.duplicated_block_lines += lineCount;
    if (sameFile) {
      summary.within_file_hits += 1;
    } else {
      summary.cross_file_hits += 1;
    }

    for (const file of sourceFiles) {
      uniqueFiles.add(file);
    }

    incrementCount(
      summary.primary_tag_counts,
      hit.primary_tag ??
        (sameFile ? "within-file duplication" : "cross-file duplication"),
    );
    incrementCount(
      summary.compatible_file_type_counts,
      hit.compatible_file_type ?? "unknown",
    );

    if (!sameFile) {
      incrementCount(
        summary.cross_file_plane_counts,
        hit.cross_file_plane ?? "repo_root",
      );
    }
  }

  summary.unique_file_count = uniqueFiles.size;
  summary.primary_tag_counts = sortedObject(summary.primary_tag_counts);
  summary.compatible_file_type_counts = sortedObject(
    summary.compatible_file_type_counts,
  );
  summary.cross_file_plane_counts = sortedObject(
    summary.cross_file_plane_counts,
  );
  return summary;
}

function scanInputDescription(scan) {
  return {
    include_roots: scan.includeRoots ?? [],
    include_paths: scan.includePaths ?? [],
    include_extensions: scan.includeExtensions ?? [],
    exclude_extensions: scan.excludeExtensions ?? [],
    include_compatible_file_types: scan.includeCompatibleFileTypes ?? [],
    include_top_level: Boolean(scan.includeTopLevel),
    include_tracked_code_files_only: Boolean(scan.includeCodeFiles),
  };
}

function makeCommandDescription(scanInput) {
  const { scan } = scanInput;

  return {
    tracked_file_list: ["git", "ls-files", "-z"],
    selection: scanInput.selection,
    excluded_file_suffixes: EXCLUDED_DUPLICATION_FILE_SUFFIXES,
    tracked_code_files_only: Boolean(scan.includeCodeFiles),
    grouped_by_compatible_file_type: Boolean(
      scan.includeCodeFiles && !scan.mixCompatibleFileTypes,
    ),
    mixed_compatible_file_types: Boolean(scan.mixCompatibleFileTypes),
    duplo_scan: [
      DUPLO_BINARY,
      "-j",
      String(THREADS),
      "-ml",
      String(scanInput.min_lines),
      "-ip",
      "-json",
      "-",
      "-",
    ],
  };
}

function filterHits(hits, filter) {
  if (filter === "within-file") {
    return hits.filter((hit) => hit.same_file);
  }
  if (filter === "cross-file") {
    return hits.filter((hit) => !hit.same_file);
  }
  return hits;
}

function splitActionableHits(hits) {
  const actionableHits = [];
  const ignoredHits = [];

  for (const hit of hits) {
    if (hit.cleanup_ignore_reason) {
      ignoredHits.push(hit);
    } else {
      actionableHits.push(hit);
    }
  }

  return { actionableHits, ignoredHits };
}

function hitLineCount(hit) {
  const explicitLineCount = Number(hit.LineCount);
  if (Number.isInteger(explicitLineCount) && explicitLineCount > 0) {
    return explicitLineCount;
  }

  const start = Number(hit.StartLineNumber1);
  const end = Number(hit.EndLineNumber1);
  return Number.isInteger(start) && Number.isInteger(end) && end >= start
    ? end - start + 1
    : 0;
}

function hitCompatibleFileTypes(hit) {
  if (
    Array.isArray(hit.source_compatible_file_types) &&
    hit.source_compatible_file_types.length > 0
  ) {
    return [...hit.source_compatible_file_types].sort();
  }

  if (hit.compatible_file_type) {
    return [hit.compatible_file_type];
  }

  return ["unknown"];
}

function hitCompatibleFileTypeKey(hit) {
  return hitCompatibleFileTypes(hit).join(",");
}

function hitDuplicateKey(hit) {
  const typeKey = hitCompatibleFileTypeKey(hit);

  if (Array.isArray(hit.Lines) && hit.Lines.length > 0) {
    return `${typeKey}\0${hitLineCount(hit)}\0${hit.Lines.join("\n")}`;
  }

  return [
    typeKey,
    "location",
    hitLineCount(hit),
    hit.SourceFile1,
    hit.StartLineNumber1,
    hit.EndLineNumber1,
    hit.SourceFile2,
    hit.StartLineNumber2,
    hit.EndLineNumber2,
  ].join("\0");
}

function hitOccurrence(hit, side) {
  const path = String(hit[`SourceFile${side}`] ?? "");
  const start = Number(hit[`StartLineNumber${side}`]);
  const end = Number(hit[`EndLineNumber${side}`]);

  if (!path || !Number.isInteger(start) || !Number.isInteger(end)) {
    return undefined;
  }

  return { path, start, end };
}

function occurrenceKey(occurrence) {
  return `${occurrence.path}\0${occurrence.start}\0${occurrence.end}`;
}

function sortedRanges(ranges) {
  return [...ranges].sort(
    (left, right) => left[0] - right[0] || left[1] - right[1],
  );
}

function groupedHitsForCleanup(hits) {
  const groups = new Map();

  for (const hit of hits) {
    const key = hitDuplicateKey(hit);
    let group = groups.get(key);
    if (!group) {
      group = {
        line_count: hitLineCount(hit),
        compatibleFileTypes: new Set(),
        occurrences: new Map(),
      };
      groups.set(key, group);
    }

    group.line_count = Math.max(group.line_count, hitLineCount(hit));
    for (const fileType of hitCompatibleFileTypes(hit)) {
      group.compatibleFileTypes.add(fileType);
    }

    for (const side of [1, 2]) {
      const occurrence = hitOccurrence(hit, side);
      if (occurrence) {
        group.occurrences.set(occurrenceKey(occurrence), occurrence);
      }
    }
  }

  return Array.from(groups.values()).map(compactGroupedHit);
}

function compactGroupedHit(group) {
  const rangesByPath = new Map();

  for (const occurrence of group.occurrences.values()) {
    const ranges = rangesByPath.get(occurrence.path) ?? [];
    ranges.push([occurrence.start, occurrence.end]);
    rangesByPath.set(occurrence.path, ranges);
  }

  const files = Array.from(rangesByPath.entries())
    .map(([path, ranges]) => ({
      path,
      lines: sortedRanges(ranges),
    }))
    .sort((left, right) => left.path.localeCompare(right.path));
  const occurrenceCount = files.reduce(
    (total, file) => total + file.lines.length,
    0,
  );
  const compatibleFileTypes = Array.from(group.compatibleFileTypes).sort();

  return {
    kind: files.length > 1 ? "cross-file" : "within-file",
    line_count: group.line_count,
    file_count: files.length,
    occurrence_count: occurrenceCount,
    compatible_file_type:
      compatibleFileTypes.length === 1 ? compatibleFileTypes[0] : "mixed",
    source_compatible_file_types: compatibleFileTypes,
    files,
  };
}

function firstHitPath(hit) {
  return hit.files[0]?.path ?? "";
}

function firstHitLine(hit) {
  return hit.files[0]?.lines[0]?.[0] ?? 0;
}

function compareGroupedHitsForCleanup(left, right) {
  if (left.kind !== right.kind) {
    return left.kind === "cross-file" ? -1 : 1;
  }

  return (
    right.file_count - left.file_count ||
    right.line_count - left.line_count ||
    right.occurrence_count - left.occurrence_count ||
    firstHitPath(left).localeCompare(firstHitPath(right)) ||
    firstHitLine(left) - firstHitLine(right)
  );
}

function compareGroupedHitsForCleanupByLineCount(left, right) {
  if (left.kind !== right.kind) {
    return left.kind === "cross-file" ? -1 : 1;
  }

  return (
    right.line_count - left.line_count ||
    right.file_count - left.file_count ||
    right.occurrence_count - left.occurrence_count ||
    firstHitPath(left).localeCompare(firstHitPath(right)) ||
    firstHitLine(left) - firstHitLine(right)
  );
}

function orderedHitsForCleanup(
  hits,
  compareHits = compareGroupedHitsForCleanup,
) {
  return groupedHitsForCleanup(hits).sort(compareHits);
}

function buildPhaseArtifact(config, scanReport, generatedAt, options = {}) {
  const filteredHits = filterHits(scanReport.hits, config.filter);
  const { actionableHits, ignoredHits } = splitActionableHits(filteredHits);
  const hits = orderedHitsForCleanup(actionableHits, options.compareHits);
  const ignored_hits = orderedHitsForCleanup(ignoredHits, options.compareHits);

  return {
    phase: {
      number: config.number,
      title: config.title,
    },
    generated_at: generatedAt,
    scan_id: scanReport.scan_id,
    filter: config.filter,
    duplo: {
      binary: DUPLO_BINARY,
      min_lines: scanReport.min_lines,
      threads: THREADS,
    },
    scope: {
      ...scanReport.selection,
      tracked_file_count: scanReport.tracked_file_count,
      compatible_file_type_file_counts:
        scanReport.compatible_file_type_file_counts,
    },
    command: scanReport.command,
    duplo_statuses: scanReport.duplo_statuses,
    duplo_stderr: scanReport.duplo_stderr,
    hit_order: options.orderDescription ?? [
      "cross-file duplication",
      "higher file_count",
      "higher line_count",
      "higher occurrence_count",
      "within-file duplication",
    ],
    summary: summarizeHits(hits),
    ignored_summary: summarizeHits(ignored_hits),
    hits,
    ignored_hits,
  };
}

function writeJson(relativePath, value) {
  const absolutePath = join(REPO_ROOT, relativePath);
  writeFileSync(
    absolutePath,
    `${JSON.stringify(value, null, OUTPUT_INDENT)}\n`,
    "utf8",
  );
}

function buildScanReports(scans, scanInputs) {
  const scanReports = new Map();

  for (const scan of scans) {
    const scanInput = scanInputs.get(scan.id);
    const duploResult = runDuploByCompatibleFileType(scanInput);
    const annotatedHits = duploResult.hits
      .filter(
        (hit) =>
          !hitReferencesExcludedFile(hit) &&
          hitHasCompatibleFileType(hit, scan),
      )
      .map(annotateHit);
    const { actionableHits } = splitActionableHits(annotatedHits);

    scanReports.set(scan.id, {
      scan_id: scan.id,
      selection: scanInput.selection,
      tracked_file_count: scanInput.tracked_file_count,
      compatible_file_type_file_counts:
        duploResult.compatible_file_type_file_counts,
      command: makeCommandDescription(scanInput),
      summary: summarizeHits(orderedHitsForCleanup(actionableHits)),
      hits: annotatedHits,
      min_lines: scanInput.min_lines,
      duplo_status: duploResult.status,
      duplo_statuses: duploResult.statuses,
      duplo_stderr: duploResult.stderr,
    });
  }

  return scanReports;
}

function resetOutputDir() {
  rmSync(OUTPUT_DIR, { recursive: true, force: true });
  mkdirSync(OUTPUT_DIR, { recursive: true });
}

function runDefaultMode() {
  resetOutputDir();

  const generatedAt = new Date().toISOString();
  const scans = [WHOLE_CODEBASE_SCAN];
  const { inventory, scanInputs } = buildScanInputs(scans);
  const codeFileCoverage = buildCodeFileCoverage(scanInputs, inventory);
  const scanReports = buildScanReports(scans, scanInputs);
  const scanReport = scanReports.get(WHOLE_CODEBASE_SCAN.id);
  const artifactPath = `cleanup/${WHOLE_CODEBASE_ARTIFACT.filename}`;
  const artifact = {
    ...buildPhaseArtifact(WHOLE_CODEBASE_ARTIFACT, scanReport, generatedAt, {
      compareHits: compareGroupedHitsForCleanupByLineCount,
      orderDescription: [
        "cross-file duplication within the same compatible code type",
        "higher line_count",
        "higher file_count",
        "higher occurrence_count",
        "within-file duplication",
        "higher line_count",
      ],
    }),
    refresh_command: ["node", "scripts/generate_cleanup_json.mjs"],
    code_file_coverage: codeFileCoverage,
  };

  writeJson(artifactPath, artifact);
  console.log(
    `code: ${artifactPath} (${artifact.summary.cross_file_hits} cross-file hits, ${artifact.summary.within_file_hits} same-file hits, ${artifact.ignored_summary.hit_count} ignored import/prologue hits)`,
  );
}

function buildPythonCoverage(inventory, scanInput) {
  const trackedPythonRecords = inventory.filter(
    (record) => record.compatibleFileType === "python",
  );
  const scannedPythonFiles = new Set(scanInput.files);
  const missingTrackedPythonFiles = trackedPythonRecords
    .map((record) => record.path)
    .filter((file) => !scannedPythonFiles.has(file));

  if (missingTrackedPythonFiles.length > 0) {
    throw new Error(
      `python cleanup scan does not cover all tracked Python files:\n${missingTrackedPythonFiles.join(
        "\n",
      )}`,
    );
  }

  return {
    tracked_python_file_count: trackedPythonRecords.length,
    scanned_python_file_count: scannedPythonFiles.size,
    missing_tracked_python_files: [],
    python_extensions: Array.from(PYTHON_EXTENSIONS).sort(),
  };
}

function buildPyxCoverage(inventory, scanInput) {
  const trackedPyxRecords = inventory.filter((record) =>
    CYTHON_EXTENSIONS.has(record.extension),
  );
  const scannedPyxFiles = new Set(scanInput.files);
  const missingTrackedPyxFiles = trackedPyxRecords
    .map((record) => record.path)
    .filter((file) => !scannedPyxFiles.has(file));

  if (missingTrackedPyxFiles.length > 0) {
    throw new Error(
      `pyx cleanup scan does not cover all tracked Cython .pyx files:\n${missingTrackedPyxFiles.join(
        "\n",
      )}`,
    );
  }

  return {
    tracked_pyx_file_count: trackedPyxRecords.length,
    scanned_pyx_file_count: scannedPyxFiles.size,
    missing_tracked_pyx_files: [],
    pyx_extensions: Array.from(CYTHON_EXTENSIONS).sort(),
  };
}

function buildModelPyPxiCoverage(inventory, scanInput) {
  const modelExtensions = new Set(MODEL_PY_PXI_EXTENSIONS);
  const trackedModelRecords = inventory.filter(
    (record) =>
      record.path.startsWith("model/") && modelExtensions.has(record.extension),
  );
  const scannedModelFiles = new Set(scanInput.files);
  const missingTrackedModelFiles = trackedModelRecords
    .map((record) => record.path)
    .filter((file) => !scannedModelFiles.has(file));

  if (missingTrackedModelFiles.length > 0) {
    throw new Error(
      `model .py/.pxi cleanup scan does not cover all tracked model .py/.pxi files:\n${missingTrackedModelFiles.join(
        "\n",
      )}`,
    );
  }

  return {
    tracked_model_py_pxi_file_count: trackedModelRecords.length,
    scanned_model_py_pxi_file_count: scannedModelFiles.size,
    missing_tracked_model_py_pxi_files: [],
    model_extensions: MODEL_PY_PXI_EXTENSIONS,
  };
}

function runPythonMode() {
  resetOutputDir();

  const generatedAt = new Date().toISOString();
  const scans = [PYTHON_ONLY_SCAN];
  const { inventory, scanInputs } = buildScanInputs(scans);
  const scanReports = buildScanReports(scans, scanInputs);
  const scanInput = scanInputs.get(PYTHON_ONLY_SCAN.id);
  const scanReport = scanReports.get(PYTHON_ONLY_SCAN.id);
  const artifactPath = `cleanup/${PYTHON_ONLY_ARTIFACT.filename}`;
  const artifact = {
    ...buildPhaseArtifact(PYTHON_ONLY_ARTIFACT, scanReport, generatedAt),
    refresh_command: ["node", "scripts/generate_cleanup_json.mjs", "--python"],
    code_file_coverage: buildPythonCoverage(inventory, scanInput),
  };

  writeJson(artifactPath, artifact);
  console.log(
    `python: ${artifactPath} (${artifact.summary.cross_file_hits} cross-file hits, ${artifact.summary.within_file_hits} same-file hits, ${artifact.ignored_summary.hit_count} ignored import/prologue hits)`,
  );
}

function runPyxMode() {
  resetOutputDir();

  const generatedAt = new Date().toISOString();
  const scans = [PYX_ONLY_SCAN];
  const { inventory, scanInputs } = buildScanInputs(scans);
  const scanReports = buildScanReports(scans, scanInputs);
  const scanInput = scanInputs.get(PYX_ONLY_SCAN.id);
  const scanReport = scanReports.get(PYX_ONLY_SCAN.id);
  const artifactPath = `cleanup/${PYX_ONLY_ARTIFACT.filename}`;
  const artifact = {
    ...buildPhaseArtifact(PYX_ONLY_ARTIFACT, scanReport, generatedAt),
    refresh_command: ["node", "scripts/generate_cleanup_json.mjs", "--pyx"],
    code_file_coverage: buildPyxCoverage(inventory, scanInput),
  };

  writeJson(artifactPath, artifact);
  console.log(
    `pyx: ${artifactPath} (${artifact.summary.cross_file_hits} cross-file hits, ${artifact.summary.within_file_hits} same-file hits)`,
  );
}

function runModelMode() {
  resetOutputDir();

  const generatedAt = new Date().toISOString();
  const scans = [MODEL_PY_PXI_SCAN];
  const { inventory, scanInputs } = buildScanInputs(scans);
  const scanReports = buildScanReports(scans, scanInputs);
  const scanInput = scanInputs.get(MODEL_PY_PXI_SCAN.id);
  const scanReport = scanReports.get(MODEL_PY_PXI_SCAN.id);
  const artifactPath = `cleanup/${MODEL_PY_PXI_ARTIFACT.filename}`;
  const artifact = {
    ...buildPhaseArtifact(MODEL_PY_PXI_ARTIFACT, scanReport, generatedAt, {
      orderDescription: [
        "cross-file duplication",
        "higher file_count",
        "higher line_count",
        "higher occurrence_count",
        "within-file duplication",
      ],
    }),
    refresh_command: ["node", "scripts/generate_cleanup_json.mjs", "--model"],
    code_file_coverage: buildModelPyPxiCoverage(inventory, scanInput),
  };

  writeJson(artifactPath, artifact);
  console.log(
    `model: ${artifactPath} (${artifact.summary.cross_file_hits} cross-file hits, ${artifact.summary.within_file_hits} same-file hits)`,
  );
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help) {
    printUsage();
    return;
  }

  if (args.mode === PYTHON_MODE) {
    runPythonMode();
    return;
  }

  if (args.mode === PYX_MODE) {
    runPyxMode();
    return;
  }

  if (args.mode === MODEL_MODE) {
    runModelMode();
    return;
  }

  runDefaultMode();
}

main();
