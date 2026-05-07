#!/usr/bin/env node

import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { availableParallelism, cpus } from 'node:os';
import { spawnSync } from 'node:child_process';

const REPO_ROOT = process.cwd();
const OUTPUT_DIR = join(REPO_ROOT, 'cleanup');
const MIN_LINES = 8;
const DUPLO_BINARY = '/bin/duplo';
const THREADS = Math.max(
  1,
  typeof availableParallelism === 'function' ? availableParallelism() : cpus().length,
);
const OUTPUT_INDENT = 2;
const EXCLUDED_DUPLICATION_FILE_SUFFIXES = ['.patch', '.generated.ts'];
const NATIVE_EXTENSIONS = new Set(['.c', '.cc', '.cpp', '.cuh', '.cu', '.cxx', '.h', '.hpp']);
const WEB_SOURCE_EXTENSIONS = new Set(['.cjs', '.cts', '.d.ts', '.js', '.jsx', '.mjs', '.mts', '.ts', '.tsx']);
const WEB_STYLE_EXTENSIONS = new Set(['.css', '.less', '.sass', '.scss']);

const CMAKE_PATTERNS = ['CMakeLists.txt', '*.cmake'];
const NATIVE_PATTERNS = ['*.cpp', '*.cc', '*.cxx', '*.cu', '*.cuh', '*.h', '*.hpp'];
const PYTHON_PATTERNS = ['*.py'];
const ANGULAR_SOURCE_PATTERNS = [
  '*.ts',
  '*.tsx',
  '*.d.ts',
  '*.mts',
  '*.cts',
  '*.js',
  '*.mjs',
  '*.cjs',
  '*.jsx',
];
const ANGULAR_TEMPLATE_PATTERNS = ['*.html'];
const ANGULAR_STYLE_PATTERNS = ['*.css', '*.scss', '*.sass', '*.less'];
const ANGULAR_CONFIG_PATTERNS = [
  '**/angular.json',
  '**/workspace.json',
  '**/project.json',
  '**/tsconfig.json',
  '**/tsconfig.*.json',
];
const ANGULAR_ASSET_PATTERNS = ['*.wgsl'];
const WEB_FRONTEND_PATTERNS = [
  ...ANGULAR_SOURCE_PATTERNS,
  ...ANGULAR_TEMPLATE_PATTERNS,
  ...ANGULAR_STYLE_PATTERNS,
  ...ANGULAR_CONFIG_PATTERNS,
  ...ANGULAR_ASSET_PATTERNS,
];
const BASH_PATTERNS = ['*.sh', '*.bash', 'mmltk'];
const THIRD_PARTY_PATTERNS = ['third_party'];

const SCANS = [
  {
    id: 'cmake',
    patterns: CMAKE_PATTERNS,
  },
  {
    id: 'native',
    patterns: NATIVE_PATTERNS,
  },
  {
    id: 'python',
    patterns: PYTHON_PATTERNS,
  },
  {
    id: 'web_frontend',
    patterns: WEB_FRONTEND_PATTERNS,
  },
  {
    id: 'bash',
    patterns: BASH_PATTERNS,
  },
  {
    id: 'third_party',
    patterns: THIRD_PARTY_PATTERNS,
  },
];

const PHASE_FILES = [
  {
    number: 1,
    title: 'CMake and Build Logic',
    filename: 'phase-1-cmake-build-logic.json',
    scanId: 'cmake',
    filter: 'all',
  },
  {
    number: 2,
    title: 'C++ Within-File Reduction',
    filename: 'phase-2-cpp-within-file-reduction.json',
    scanId: 'native',
    filter: 'within-file',
  },
  {
    number: 3,
    title: 'Shared C++ Across Files',
    filename: 'phase-3-shared-cpp-across-files.json',
    scanId: 'native',
    filter: 'cross-file',
  },
  {
    number: 4,
    title: 'Angular and Web Frontend Reduction',
    filename: 'phase-4-typescript-reduction.json',
    scanId: 'web_frontend',
    filter: 'all',
  },
  {
    number: 5,
    title: 'Python Reduction',
    filename: 'phase-5-python-reduction.json',
    scanId: 'python',
    filter: 'all',
  },
  {
    number: 6,
    title: 'Bash and Entry-Point Reduction',
    filename: 'phase-6-bash-entry-point-reduction.json',
    scanId: 'bash',
    filter: 'all',
  },
  {
    number: 7,
    title: 'Third-Party Tracked Code',
    filename: 'phase-7-third-party-tracked-code.json',
    scanId: 'third_party',
    filter: 'all',
  },
];

function runCommand(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: REPO_ROOT,
    encoding: 'utf8',
    maxBuffer: 256 * 1024 * 1024,
    ...options,
  });

  if (result.error) {
    throw result.error;
  }

  return result;
}

function getTrackedFiles(patterns) {
  const result = runCommand('git', ['ls-files', '-z', '--', ...patterns]);
  if (result.status !== 0) {
    throw new Error(`git ls-files failed for patterns ${patterns.join(', ')}:\n${result.stderr}`);
  }

  return result.stdout.split('\0').filter(Boolean).filter((path) => !isExcludedDuplicationFile(path));
}

function runDuplo(files) {
  if (files.length === 0) {
    return { hits: [], status: 0, stderr: '' };
  }

  const result = runCommand(
    DUPLO_BINARY,
    ['-j', String(THREADS), '-ml', String(MIN_LINES), '-ip', '-json', '-', '-'],
    { input: files.join('\n') },
  );

  if (![0, 1].includes(result.status ?? 0)) {
    throw new Error(`duplo failed with status ${result.status}:\n${result.stderr}`);
  }

  const parsedHits = result.stdout.trim() ? JSON.parse(result.stdout) : [];

  return {
    hits: Array.isArray(parsedHits) ? parsedHits : [],
    status: result.status ?? 0,
    stderr: result.stderr.trim(),
  };
}

function isVendored(path) {
  return path === 'third_party' || path.startsWith('third_party/');
}

function isExcludedDuplicationFile(path) {
  const normalizedPath = path.toLowerCase();
  return EXCLUDED_DUPLICATION_FILE_SUFFIXES.some((suffix) => normalizedPath.endsWith(suffix));
}

function hitReferencesExcludedFile(hit) {
  return isExcludedDuplicationFile(hit.SourceFile1) || isExcludedDuplicationFile(hit.SourceFile2);
}

function extensionOf(path) {
  const basename = path.slice(path.lastIndexOf('/') + 1);
  const dottedExtensionMatch = basename.match(/\.d\.ts$/);
  if (dottedExtensionMatch) {
    return dottedExtensionMatch[0];
  }

  const extensionStart = basename.lastIndexOf('.');
  return extensionStart < 0 ? '' : basename.slice(extensionStart);
}

function languageFamily(path) {
  const extension = extensionOf(path);

  if (path === 'CMakeLists.txt' || extension === '.cmake') {
    return 'cmake';
  }
  if (NATIVE_EXTENSIONS.has(extension)) {
    return 'native';
  }
  if (extension === '.py') {
    return 'python';
  }
  if (WEB_SOURCE_EXTENSIONS.has(extension)) {
    return 'web-source';
  }
  if (extension === '.html') {
    return 'web-template';
  }
  if (WEB_STYLE_EXTENSIONS.has(extension)) {
    return 'web-style';
  }
  if (
    path.endsWith('/angular.json') ||
    path.endsWith('/workspace.json') ||
    path.endsWith('/project.json') ||
    path.endsWith('/tsconfig.json') ||
    /(^|\/)tsconfig\.[^/]+\.json$/.test(path)
  ) {
    return 'web-config';
  }
  if (extension === '.wgsl') {
    return 'wgsl';
  }
  if (extension === '.sh' || extension === '.bash' || path === 'mmltk') {
    return 'bash';
  }

  return `other:${extension || path}`;
}

function hitIsSameLanguage(hit) {
  return hit.SourceFile1 === hit.SourceFile2 || languageFamily(hit.SourceFile1) === languageFamily(hit.SourceFile2);
}

function isBuildSystem(path) {
  return path === 'CMakeLists.txt' || path.endsWith('.cmake') || path.startsWith('cmake/');
}

function isTestPath(path) {
  return (
    /(^|\/)(test|tests)(\/|$)/.test(path) ||
    /(^|\/)(test_|tests_)/.test(path) ||
    /(_test|_tests)\.[^/]+$/.test(path)
  );
}

function annotateHit(hit) {
  const sameFile = hit.SourceFile1 === hit.SourceFile2;
  const sourceFiles = Array.from(new Set([hit.SourceFile1, hit.SourceFile2])).sort();
  const sourceLanguageFamilies = Array.from(new Set(sourceFiles.map(languageFamily))).sort();
  const tags = [sameFile ? 'within-file duplication' : 'cross-file duplication'];

  if (isTestPath(hit.SourceFile1) && isTestPath(hit.SourceFile2)) {
    tags.push('test-only duplication');
  }

  if (isBuildSystem(hit.SourceFile1) && isBuildSystem(hit.SourceFile2)) {
    tags.push('build-system duplication');
  }

  if (isVendored(hit.SourceFile1) && isVendored(hit.SourceFile2)) {
    tags.push('vendored duplication');
  }

  let primaryTag = sameFile ? 'within-file duplication' : 'cross-file duplication';
  if (tags.includes('vendored duplication')) {
    primaryTag = 'vendored duplication';
  } else if (tags.includes('build-system duplication')) {
    primaryTag = 'build-system duplication';
  } else if (tags.includes('test-only duplication')) {
    primaryTag = 'test-only duplication';
  }

  return {
    ...hit,
    same_file: sameFile,
    source_files: sourceFiles,
    source_language_families: sourceLanguageFamilies,
    primary_tag: primaryTag,
    tags,
  };
}

function summarizeHits(hits) {
  const summary = {
    hit_count: hits.length,
    duplicated_block_lines: 0,
    within_file_hits: 0,
    cross_file_hits: 0,
    unique_file_count: 0,
    primary_tag_counts: {},
  };
  const uniqueFiles = new Set();

  for (const hit of hits) {
    summary.duplicated_block_lines += hit.LineCount ?? 0;
    if (hit.same_file) {
      summary.within_file_hits += 1;
    } else {
      summary.cross_file_hits += 1;
    }

    for (const file of hit.source_files ?? []) {
      uniqueFiles.add(file);
    }

    const key = hit.primary_tag ?? 'unclassified';
    summary.primary_tag_counts[key] = (summary.primary_tag_counts[key] ?? 0) + 1;
  }

  summary.unique_file_count = uniqueFiles.size;
  return summary;
}

function makeCommandDescription(patterns) {
  return {
    tracked_file_list: ['git', 'ls-files', '-z', '--', ...patterns],
    excluded_file_suffixes: EXCLUDED_DUPLICATION_FILE_SUFFIXES,
    duplo_scan: [
      DUPLO_BINARY,
      '-j',
      String(THREADS),
      '-ml',
      String(MIN_LINES),
      '-ip',
      '-json',
      '-',
      '-',
    ],
  };
}

function filterHits(hits, filter) {
  if (filter === 'within-file') {
    return hits.filter((hit) => hit.same_file);
  }
  if (filter === 'cross-file') {
    return hits.filter((hit) => !hit.same_file);
  }
  return hits;
}

function buildPhaseArtifact(config, scanReport, generatedAt) {
  const filteredHits = filterHits(scanReport.hits, config.filter);

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
      min_lines: MIN_LINES,
      threads: THREADS,
    },
    scope: {
      patterns: scanReport.patterns,
      tracked_file_count: scanReport.tracked_file_count,
    },
    command: scanReport.command,
    summary: summarizeHits(filteredHits),
    hits: filteredHits,
  };
}

function writeJson(relativePath, value) {
  const absolutePath = join(REPO_ROOT, relativePath);
  writeFileSync(absolutePath, `${JSON.stringify(value, null, OUTPUT_INDENT)}\n`, 'utf8');
}

function main() {
  mkdirSync(OUTPUT_DIR, { recursive: true });

  const generatedAt = new Date().toISOString();
  const scanReports = new Map();

  for (const scan of SCANS) {
    const trackedFiles = getTrackedFiles(scan.patterns);
    const duploResult = runDuplo(trackedFiles);
    const annotatedHits = duploResult.hits
      .filter((hit) => !hitReferencesExcludedFile(hit) && hitIsSameLanguage(hit))
      .map(annotateHit);

    scanReports.set(scan.id, {
      scan_id: scan.id,
      patterns: scan.patterns,
      tracked_file_count: trackedFiles.length,
      command: makeCommandDescription(scan.patterns),
      summary: summarizeHits(annotatedHits),
      hits: annotatedHits,
      duplo_status: duploResult.status,
    });
  }

  const phase0Path = 'cleanup/phase-0-duplication-ledger.json';
  const phaseArtifacts = [];

  for (const config of PHASE_FILES) {
    const scanReport = scanReports.get(config.scanId);
    const artifact = buildPhaseArtifact(config, scanReport, generatedAt);
    const relativePath = `cleanup/${config.filename}`;
    writeJson(relativePath, artifact);

    phaseArtifacts.push({
      phase: config.number,
      title: config.title,
      artifact_path: relativePath,
      scan_id: config.scanId,
      filter: config.filter,
      summary: artifact.summary,
    });
  }

  const phase0Artifact = {
    phase: {
      number: 0,
      title: 'Establish the Duplication Ledger',
    },
    generated_at: generatedAt,
    refresh_command: ['node', 'utilities/generate_cleanup_json.mjs'],
    duplo: {
      binary: DUPLO_BINARY,
      min_lines: MIN_LINES,
      threads: THREADS,
    },
    baseline_slices: SCANS.map((scan) => {
      const report = scanReports.get(scan.id);
      const matchingPhases = phaseArtifacts.filter((artifact) => artifact.scan_id === scan.id);
      return {
        scan_id: scan.id,
        patterns: scan.patterns,
        tracked_file_count: report.tracked_file_count,
        command: report.command,
        summary: report.summary,
        phase_artifacts: matchingPhases.map((artifact) => ({
          phase: artifact.phase,
          title: artifact.title,
          artifact_path: artifact.artifact_path,
          filter: artifact.filter,
        })),
      };
    }),
  };

  writeJson(phase0Path, phase0Artifact);

  for (const artifact of phaseArtifacts) {
    console.log(
      `phase ${artifact.phase}: ${artifact.artifact_path} (${artifact.summary.hit_count} hits)`,
    );
  }
  console.log(`phase 0: ${phase0Path}`);
}

main();
