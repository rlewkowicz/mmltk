#!/usr/bin/env node
import { readFileSync, statSync, chmodSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { buildInventory, CLEANUP_PROFILES, collectInventoryInputs, writeAtomic } from "./generate_cleanup_json.mjs";
import { conflictingMacros, formatDeclarationSource } from "./cleanup/declaration_patterns.mjs";

export function parseFormatArgs(args) {
  if (args.length === 1 && ["--help", "-h"].includes(args[0])) return { mode: "help" };
  if (!["check", "preview", "fix"].includes(args[0])) throw new Error("expected check, preview, or fix; use --help");
  const options = { mode: args[0], files: [], report: "cleanup/declarations-format.json" };
  let reportSeen = false;
  for (let i = 1; i < args.length; i += 2) {
    if (!["--file", "--report"].includes(args[i]) || !args[i + 1]) throw new Error("expected --file PATH or --report PATH");
    if (args[i] === "--file") options.files.push(args[i + 1]);
    else {
      if (reportSeen) throw new Error("--report may appear only once");
      options.report = args[i + 1];
      reportSeen = true;
    }
  }
  return options;
}
export function declarationFormatReport(inventory, sources, options = {}) {
  const conflicts = new Set(inventory.flatMap((path) => conflictingMacros(path, sources.get(path))));
  const selected = options.files?.length ? [...new Set(options.files)].sort() : inventory;
  for (const path of selected) if (!inventory.includes(path)) throw new Error(`source is not in the first-party CPD inventory: ${path}`);
  const results = selected.map((path) => ({ path, ...formatDeclarationSource(path, sources.get(path), { macroConflicts: conflicts }) }));
  return {
    format: 1,
    purpose: "Syntactic annotation formatting; manual candidates are not duplication verdicts or automatic permission",
    inventory: selected,
    macro_conflicts: [...conflicts].sort(),
    changed_files: results.filter((result) => result.changed).map((result) => result.path),
    files: results.filter((result) => result.candidates.length).map(({ path, edits, candidates, changed, output }) => ({
      path, edits, candidates,
      ...(changed ? { before: sources.get(path), after: output } : {}),
    })),
    summary: { files: selected.length, changed_files: results.filter((result) => result.changed).length,
      ...Object.fromEntries(["rewrite", "manual", "retained"].map((kind) => [kind, results.reduce((count, result) => count + result.candidates.filter((candidate) => candidate.disposition === kind).length, 0)])) },
  };
}
export function runFormatter(options) {
  if (!options.report.endsWith(".json")) throw new Error("formatter report must be a .json path");
  const inventory = buildInventory(CLEANUP_PROFILES.cpp, collectInventoryInputs()).scannedFiles;
  const sources = new Map(inventory.map((path) => [path, readFileSync(path, "utf8")]));
  const report = declarationFormatReport(inventory, sources, options);
  // The report always records the original ranges and replacement text, including
  // on fix. Raw detector evidence is a separate artifact and is never overwritten.
  writeAtomic(options.report, `${JSON.stringify(report, null, 2)}\n`);
  if (options.mode === "fix") {
    for (const file of report.files) {
      if (file.after === undefined) continue;
      if (readFileSync(file.path, "utf8") !== sources.get(file.path)) throw new Error(`source changed while formatting: ${file.path}`);
      const mode = statSync(file.path).mode;
      writeAtomic(file.path, file.after);
      chmodSync(file.path, mode);
    }
  }
  console.log(`${options.mode}: ${report.summary.changed_files} files, ${report.summary.rewrite} safe rewrites, ${report.summary.manual} manual candidates; ${options.report}`);
  if (options.mode === "preview") for (const path of report.changed_files) console.log(`  ${path}`);
  return options.mode === "check" && report.changed_files.length ? 1 : 0;
}
function main() {
  const options = parseFormatArgs(process.argv.slice(2));
  if (options.mode === "help") {
    console.log("Usage: ./mmltk --format-declarations check|preview|fix [--file PATH ...] [--report PATH]\n" +
      "Default: complete first-party CPD inventory. Check exits 1 for safe pending edits; preview leaves sources unchanged.\n" +
      "Every mode writes original spans, reasons, proposed edits and explicit manual candidates.\n" +
      "Fix changes only proven canonical policy constructions and adds their direct include. Repeated fix is idempotent.\n" +
      "Unknown lookup, conflicting macros, preprocessing, comments inside annotations and unfamiliar compositions remain manual.");
    return;
  }
  process.exitCode = runFormatter(options);
}
if (process.argv[1] && pathToFileURL(process.argv[1]).href === import.meta.url) {
  try { main(); } catch (error) { console.error(error.message); process.exitCode = 2; }
}
