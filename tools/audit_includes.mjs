#!/usr/bin/env node
// Lexical evidence, not an include-what-you-use or semantic dependency checker.
import { readFileSync, existsSync, statSync, chmodSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { dirname, basename, resolve, relative } from "node:path";
import { pathToFileURL } from "node:url";
import { AUTHORED_PROFILE, authoredLanguage, collectInventoryInputs, writeAtomic } from "./generate_cleanup_json.mjs";
import { tokenizeCpp } from "./cleanup/cpd_patterns.mjs";
import { preprocessorRegions, lineStarts } from "./cleanup/declaration_patterns.mjs";

const STANDARD = new Set(`
algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes
climits clocale cmath codecvt compare complex concepts condition_variable contracts coroutine csetjmp csignal
cstdarg cstddef cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype deque exception execution expected
filesystem flat_map flat_set format forward_list fstream functional future generator hazard_pointer
initializer_list inplace_vector iomanip ios iosfwd iostream istream iterator latch limits list locale map
mdspan memory memory_resource meta mutex new numbers numeric optional ostream print queue random ranges ratio
rcu regex scoped_allocator semaphore set shared_mutex source_location span spanstream sstream stack stacktrace
stdexcept stdfloat stop_token streambuf string string_view strstream syncstream system_error text_encoding
thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant vector
version assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h limits.h locale.h math.h setjmp.h
signal.h stdalign.h stdarg.h stdatomic.h stdbool.h stddef.h stdint.h stdio.h stdlib.h stdnoreturn.h string.h
tgmath.h threads.h time.h uchar.h wchar.h wctype.h
`.trim().split(/\s+/u));
const POSIX = new Set("aio.h arpa/inet.h dirent.h dlfcn.h endian.h fcntl.h fnmatch.h getopt.h glob.h grp.h libgen.h malloc.h netdb.h poll.h pthread.h pwd.h sched.h semaphore.h spawn.h strings.h syslog.h termios.h unistd.h utime.h".split(" "));
const isPch = (path) => /^src\/pch_\w+\.h$/u.test(path);
const ordinaryCpp = (path) => /\.(?:cpp|cc|cxx|c\+\+|C)$/u.test(path);
const sorted = (values) => [...new Set(values)].sort();
const rank = (a, b) => b.files.length - a.files.length || a.header.localeCompare(b.header);

// Compile databases store shell words, never commands for this tool to execute.
export function commandWords(command) {
  const result = [];
  let word = "", quote = "", started = false;
  for (let i = 0; i < command.length; ++i) {
    const c = command[i];
    if (c === "\\" && quote !== "'") {
      if (++i === command.length) throw new Error("unterminated compile-command escape");
      word += command[i]; started = true;
    } else if (c === quote) { quote = ""; }
    else if (!quote && (c === "'" || c === '"')) { quote = c; started = true; }
    else if (!quote && /\s/u.test(c)) {
      if (started) result.push(word);
      word = ""; started = false;
    } else { word += c; started = true; }
  }
  if (quote) throw new Error("unterminated compile-command quote");
  if (started) result.push(word);
  return result;
}

export function compilationContexts(entries, root, reader = readFileSync, available = existsSync) {
  const contexts = new Map(), policies = new Map();
  for (const entry of entries) {
    const source = resolve(entry.directory, entry.file), file = relative(root, source);
    if (file === ".." || file.startsWith("../")) continue;
    const words = entry.arguments ?? commandWords(entry.command);
    const outputIndex = words.indexOf("-o");
    const output = entry.output ?? (outputIndex >= 0 ? words[outputIndex + 1] : undefined);
    if (!output) throw new Error(`compile command has no output: ${file}`);
    const artifact = resolve(entry.directory, output);
    const outputMismatch = entry.output && outputIndex >= 0 && resolve(entry.directory, words[outputIndex + 1]) !== artifact;
    const owner = /^(.*\/CMakeFiles\/([^/]+)\.dir)\//u.exec(artifact);
    const includeDirs = [], quoteDirs = [], forced = [];
    for (let i = 0; i < words.length; ++i) {
      const word = words[i];
      if (["-I", "-isystem", "-iquote", "-include", "-include-pch", "-imacros"].includes(word)) {
        if (!words[i + 1]) throw new Error(`compile option ${word} has no argument: ${file}`);
        const path = resolve(entry.directory, words[++i]);
        if (word === "-iquote") quoteDirs.push(path);
        else if (["-I", "-isystem"].includes(word)) includeDirs.push(path);
        else forced.push({ option: word, path });
      } else if (word.startsWith("-I")) includeDirs.push(resolve(entry.directory, word.slice(2)));
      else if (word.startsWith("-isystem")) includeDirs.push(resolve(entry.directory, word.slice(8)));
      else if (word.startsWith("-iquote")) quoteDirs.push(resolve(entry.directory, word.slice(7)));
    }
    const policyPath = owner ? `${owner[1]}/mmltk-pch-policy.txt` : null;
    if (policyPath && !policies.has(policyPath)) {
      policies.set(policyPath, available(policyPath) ? reader(policyPath, "utf8").trim().split("\n").map((row) => row.split("\t")) : []);
    }
    const policy = policies.get(policyPath) ?? [];
    const registered = policy.some(([kind, path]) => kind === "use" && resolve(entry.directory, path) === source);
    const excluded = policy.some(([kind, path]) => kind === "skip" && resolve(entry.directory, path) === source);
    const groups = policy.filter(([kind]) => kind === "header").map(([, path]) => relative(root, path));
    const modules = words.some((word) => /^-fmodule/u.test(word));
    const forcedPch = forced.filter(({ path }) => basename(path).startsWith("cmake_pch."));
    const issues = [];
    let reason, pch = [];
    if (artifact.endsWith(".gch")) reason = "PCH artifact creation";
    else {
      if (outputMismatch) issues.push("compile output metadata differs from its compiler argument");
      if (registered && excluded) issues.push("source is both used and excluded by its target policy");
      if (forcedPch.length && (!owner || !registered)) issues.push("forced PCH has no matching target/source registration");
      if (registered && (forcedPch.length !== 1 || forcedPch[0]?.option !== "-include" ||
          forcedPch[0]?.path !== `${owner[1]}/cmake_pch.hxx`)) {
        issues.push("registered use lacks the exact target-local forced PCH");
      }
      if (registered && !groups.length) issues.push("registered use has no PCH header groups");
      if ((modules || !ordinaryCpp(file)) && forcedPch.length) issues.push("excluded language/module compilation forces a PCH");
      if (registered && words.some((word) => word.startsWith("@"))) issues.push("response-file arguments hide the complete PCH environment");
      if (issues.length) reason = "inconsistent PCH evidence";
      else if (modules) reason = "module compilation does not consume a PCH";
      else if (!owner) reason = "compile output has no registered CMake owner";
      else if (excluded) reason = "source is excluded by its target PCH policy";
      else if (!policy.length) reason = "target has no PCH registration";
      else if (!registered) reason = "source is absent from its target PCH consumers";
      else { reason = "target-local forced PCH"; pch = groups; }
    }
    const context = { target: owner?.[2] ?? "(unregistered)", include_dirs: includeDirs, quote_dirs: quoteDirs,
      modules, pch, pch_reason: reason, pch_issues: issues,
      forced_pch: forcedPch.map(({ option, path }) => ({ option, path: relative(root, path) })) };
    const records = contexts.get(file) ?? [];
    records.push(context); contexts.set(file, records);
  }
  return contexts;
}

export function scanIncludes(source) {
  const tokens = tokenizeCpp(source), { regions } = preprocessorRegions(source, tokens);
  const lines = lineStarts(source);
  let line = 0;
  const branches = [];
  const includes = [];
  for (const region of regions) {
    // Comments between # and include are legal preprocessing whitespace.
    const clean = region.text.replace(/\\\r?\n/gu, "").replace(/\/\*[\s\S]*?\*\//gu, " ").trimEnd();
    region.directive = /^#\s*(\w+)/u.exec(clean)?.[1];
    if (["if", "ifdef", "ifndef"].includes(region.directive)) branches.push(region.start);
    if (["else", "elif"].includes(region.directive) && branches.length) branches[branches.length - 1] = region.start;
    if (region.directive === "endif") branches.pop();
    const directive = /^#\s*include(?:_next)?\b\s*(.*)$/u.exec(clean);
    if (!directive) continue;
    const literal = /^(?:<([^>]+)>|"([^"]+)")\s*(?:\/\/.*)?$/u.exec(directive[1]);
    while (line + 1 < lines.length && lines[line + 1] <= region.start) ++line;
    includes.push({ header: literal?.[1] ?? literal?.[2] ?? directive[1], spelling: literal?.[1] ? "angle" : literal ? "quote" : "macro",
      line: line + 1, start: region.start, end: region.end,
      conditional: branches.length > 0, branch: branches.join(":"),
      plain: /^#include (?:<[^>]+>|"[^"]+")\r?$/u.test(region.text) && region.start === lines[line],
      include_next: /^#\s*include_next\b/u.test(clean) });
  }
  return { includes, regions, tokens };
}

function systemHeaderCategory(include) {
  if (include.spelling !== "angle") return undefined;
  if (STANDARD.has(include.header)) return "standard";
  if (POSIX.has(include.header) || /^(?:sys|linux|asm|net|netinet)\//u.test(include.header)) return "linux-posix";
  return undefined;
}

export function attributeInclude(include, path, paths, contexts, root) {
  if (include.spelling === "macro") return { category: "macro", resolution: "unresolved" };
  const dirs = include.spelling === "quote" ? [resolve(root, dirname(path)), root] : [];
  for (const context of contexts) dirs.push(...(include.spelling === "quote" ? context.quote_dirs : []), ...context.include_dirs);
  const matches = sorted(dirs.map((dir) => relative(root, resolve(dir, include.header))).filter((candidate) => paths.has(candidate)));
  if (matches.length === 1) return { category: isPch(matches[0]) ? "pch" : matches[0].startsWith("third_party/") ? "vendored" : "project", resolved: matches[0], resolution: "include-path" };
  if (matches.length > 1) return { category: "ambiguous", candidates: matches, resolution: "ambiguous" };
  const systemCategory = systemHeaderCategory(include);
  if (systemCategory) return { category: systemCategory, resolution: "header-name" };
  // Headers lack compile commands. A unique suffix is evidence, not proof of
  // include-path availability; expose the weaker attribution in the report.
  if (include.spelling === "quote") {
    const suffixes = [...paths].filter((candidate) => candidate.endsWith(`/${include.header}`));
    if (suffixes.length === 1) return { category: suffixes[0].startsWith("third_party/") ? "vendored" : "project", resolved: suffixes[0], resolution: "unique-suffix" };
    if (suffixes.length > 1) return { category: "ambiguous", candidates: suffixes.sort(), resolution: "ambiguous" };
  }
  return { category: include.spelling === "angle" ? "external" : "unresolved", resolution: "unresolved" };
}

// Intersect consumer sets for same-directory pairs, then close each set over
// all headers those consumers share. This finds triples and larger bundles
// without enumerating every subset of a large include block.
export function repeatedBundles(files) {
  const folders = new Map();
  for (const file of files) {
    for (const include of file.includes.filter((entry) => entry.category === "project")) {
      const folder = dirname(include.resolved);
      const headers = folders.get(folder) ?? new Map();
      const consumers = headers.get(include.resolved) ?? new Set();
      consumers.add(file.path); headers.set(include.resolved, consumers); folders.set(folder, headers);
    }
  }
  const bundles = [];
  for (const [folder, headers] of folders) {
    const entries = [...headers].filter(([, consumers]) => consumers.size >= 2), seen = new Set();
    for (let i = 0; i < entries.length; ++i) for (let j = i + 1; j < entries.length; ++j) {
      const consumers = sorted([...entries[i][1]].filter((file) => entries[j][1].has(file)));
      if (consumers.length < 2) continue;
      const key = consumers.join("\n");
      if (seen.has(key)) continue;
      seen.add(key);
      const shared = entries.filter(([, uses]) => consumers.every((file) => uses.has(file))).map(([header]) => header).sort();
      const external = consumers.filter((file) => !file.startsWith(`${folder}/`));
      bundles.push({ folder, headers: shared, consumers, outside_folder: external,
        independent_directories: sorted(external.map(dirname)), possible_include_saving: (shared.length - 1) * consumers.length - shared.length });
    }
  }
  return bundles.sort((a, b) => b.outside_folder.length - a.outside_folder.length || b.consumers.length - a.consumers.length || a.folder.localeCompare(b.folder));
}

export function pchReplacement(path, source, scan, contexts, coverage) {
  const retained = [], headerSets = [...coverage.values()];
  const tracked = scan.includes.filter((include) => systemHeaderCategory(include) ||
    (include.spelling !== "macro" && headerSets.some((headers) => headers.has(include.header))));
  const retain = (include, reason) => retained.push({ line: include.line, header: include.header,
    category: systemHeaderCategory(include) ?? "PCH-covered", reason });
  let reason;
  if (isPch(path)) reason = "PCH definition";
  else if (/\.(?:h|hh|hpp|hxx|cuh|tpp)(?:\.in)?$/u.test(path)) reason = "declaration header requires direct dependencies";
  else if (/\.cu$/u.test(path)) reason = "CUDA source does not consume an ordinary C++ PCH";
  else if (/\.(?:cppm|ixx)(?:\.in)?$/u.test(path)) reason = "module source does not consume a PCH";
  else if (!ordinaryCpp(path)) reason = "not an ordinary C++ translation unit";
  else if (!contexts.length) reason = "source has no compile command in this build graph";
  else {
    const exclusions = contexts.filter((context) => context.modules || !context.pch.length);
    if (exclusions.length) reason = sorted(exclusions.map((context) => context.pch_reason ??
      (context.modules ? "module compilation does not consume a PCH" : "compile context has no PCH"))).join("; ");
  }
  if (reason) {
    for (const include of tracked) retain(include, reason);
    return { reason, removed: 0, added: 0, retained };
  }
  const common = contexts[0].pch.filter((header) => contexts.every((context) => context.pch.includes(header)));
  const replacements = new Map();
  for (const include of tracked) {
    const owner = common.find((header) => coverage.get(header)?.has(include.header));
    if (!owner) { retain(include, "header is not provided by a PCH shared by every compile context"); continue; }
    if (include.spelling !== "angle") { retain(include, "non-angle include retains explicit lookup"); continue; }
    if (include.include_next) { retain(include, "include_next changes lookup"); continue; }
    const key = `${owner}:${include.branch}`;
    const group = replacements.get(key) ?? { owner, includes: [] };
    group.includes.push(include); replacements.set(key, group);
  }
  const edits = [], groups = [];
  let added = 0, removed = 0;
  for (const { owner: header, includes } of replacements.values()) {
    const existing = scan.includes.some((include) => include.header === header && include.branch === includes[0].branch && !include.include_next);
    groups.push(header);
    let emitted = existing;
    for (const include of includes) {
      const end = source[include.end] === "\n" ? include.end + 1 : include.end;
      // Preserve unusual spelling and attached comments in place. Branches are
      // separate groups, so an inactive branch can never supply another one.
      const replacement = !include.plain ? source.slice(include.start, end).replace(`<${include.header}>`, `"${header}"`) :
        !emitted ? `#include "${header}"${source.includes("\r\n") ? "\r\n" : "\n"}` : "";
      if (replacement) ++added;
      emitted = true;
      edits.push({ start: include.start, end, replacement }); ++removed;
    }
  }
  let after = source;
  for (const edit of edits.sort((a, b) => b.start - a.start)) after = after.slice(0, edit.start) + edit.replacement + after.slice(edit.end);
  const compiled = ordinaryCpp(path) && contexts.length && contexts.every((context) => !context.modules && groups.every((group) => context.pch.includes(group)));
  return { reason: edits.length ? compiled ? "compiled PCH" : "textual PCH header" : "no covered includes", removed, added, groups, retained, ...(edits.length ? { after } : {}) };
}

export function buildIncludeReport(paths, sources, contexts, root) {
  const allPaths = new Set(paths), files = [], scans = new Map(), frequency = new Map();
  const coverage = new Map();
  for (const [path, source] of sources) {
    const scan = scanIncludes(source); scans.set(path, scan);
    if (isPch(path)) coverage.set(path, new Set(scan.includes.filter((entry) => entry.spelling === "angle").map((entry) => entry.header)));
    const owners = contexts.get(path) ?? [];
    const includes = scan.includes.map((include) => ({ ...include, ...attributeInclude(include, path, allPaths, owners, root) }));
    files.push({ path, lines: source.split("\n").length - Number(source.endsWith("\n")), targets: sorted(owners.map((owner) => owner.target)),
      compile_contexts: owners.map(({ target, modules, pch, pch_reason, pch_issues, forced_pch }) => ({ target, modules, pch, pch_reason, pch_issues, forced_pch })),
      includes: includes.sort((a, b) => a.category.localeCompare(b.category) || a.header.localeCompare(b.header) || a.line - b.line) });
    if (isPch(path)) continue;
    for (const include of includes) {
      const header = include.resolved ?? include.header, key = `${include.category}:${header}`;
      const item = frequency.get(key) ?? { header, category: include.category, files: new Set(), occurrences: 0 };
      item.files.add(path); ++item.occurrences; frequency.set(key, item);
    }
  }
  const folders = new Map();
  for (const path of paths.filter((path) => !path.startsWith("third_party/"))) {
    const dir = dirname(path), entries = folders.get(dir) ?? [];
    entries.push(path); folders.set(dir, entries);
  }
  const directoryReport = [...folders].filter(([, entries]) => entries.length > 10).map(([path, entries]) => {
    const prefixes = new Map();
    for (const entry of entries) {
      const prefix = /^([^_.]+)_/u.exec(basename(entry))?.[1];
      if (prefix) prefixes.set(prefix, [...(prefixes.get(prefix) ?? []), entry]);
    }
    return { path, file_count: entries.length, files: entries.sort(),
      prefixes: [...prefixes].filter(([, files]) => files.length >= 3).map(([prefix, files]) => ({ prefix, files })).sort((a, b) => b.files.length - a.files.length),
      over_500_lines: files.filter((file) => dirname(file.path) === path && file.lines > 500).map((file) => ({ path: file.path, lines: file.lines })) };
  }).sort((a, b) => b.file_count - a.file_count || a.path.localeCompare(b.path));
  const frequencies = [...frequency.values()].map((item) => ({ ...item, files: sorted(item.files) })).sort(rank);
  const replacements = files.map((file) => ({ path: file.path, ...pchReplacement(file.path, sources.get(file.path), scans.get(file.path), contexts.get(file.path) ?? [], coverage) }));
  const retainedReasons = new Map();
  for (const file of replacements) for (const include of file.retained) {
    if (!["standard", "linux-posix"].includes(include.category) || isPch(file.path)) continue;
    const item = retainedReasons.get(include.reason) ?? { reason: include.reason, files: new Set(), includes: 0 };
    item.files.add(file.path); ++item.includes; retainedReasons.set(include.reason, item);
  }
  const pchCandidates = frequencies.filter((item) => ["standard", "linux-posix"].includes(item.category) && item.files.length >= 3).map((item) => ({ ...item,
    covered_by: [...coverage].filter(([, headers]) => headers.has(item.header)).map(([path]) => path) }));
  return { format: 1, scope: "Git tracked and untracked-unignored first-party files; generated/cache/output and vendored trees excluded. Includes are lexical, including inactive branches. PCH headers do not inflate frequency.",
    thresholds: { folder_files_exclusive: 10, pch_distinct_files: 3, bundle_consumers: 2 },
    summary: { files: paths.length, include_files: files.length, includes: files.reduce((n, file) => n + file.includes.length, 0),
      large_folders: directoryReport.length, changed_files: replacements.filter((file) => file.after !== undefined).length,
      removed: replacements.reduce((n, file) => n + file.removed, 0), added: replacements.reduce((n, file) => n + file.added, 0) },
    folders: directoryReport, pch_candidates: pchCandidates,
    pch_groups: [...coverage].map(([header, includes]) => ({ header, includes: sorted(includes),
      consumers: files.filter((file) => file.includes.some((entry) => entry.resolved === header)).map((file) => file.path),
      compiled_consumers: files.filter((file) => {
        const owners = contexts.get(file.path) ?? [];
        return ordinaryCpp(file.path) && owners.length && owners.every((owner) => !owner.modules && owner.pch.includes(header));
      }).map((file) => file.path) })),
    retained_system_includes: [...retainedReasons.values()].map((item) => ({ ...item, files: sorted(item.files) })).sort((a, b) => b.includes - a.includes || a.reason.localeCompare(b.reason)),
    frequency: frequencies, bundles: repeatedBundles(files), replacements, files };
}

export function renderIncludeMarkdown(report) {
  const rows = ["# Include and organization inventory", "", report.scope, "", "This is candidate evidence. Co-inclusion does not prove an umbrella header or folder move is beneficial.", "",
    `Scanned ${report.summary.include_files} C/C++/CUDA files and ${report.summary.includes} directives; ${report.summary.large_folders} folders exceed ten files.`, "",
    `PCH preview: ${report.summary.changed_files} files, ${report.summary.removed} direct includes replaced by ${report.summary.added} PCH includes.`, "",
    "## Folders over ten files", "", "| Folder | Files | Prefix families (3+) | C/C++ files over 500 lines |", "|---|---:|---|---|"];
  for (const folder of report.folders) rows.push(`| ${folder.path} | ${folder.file_count} | ${folder.prefixes.map((group) => `${group.prefix}_ (${group.files.length})`).join(", ")} | ${folder.over_500_lines.map((file) => `${basename(file.path)} (${file.lines})`).join(", ")} |`);
  rows.push("", "## Standard and Linux/POSIX includes in three or more files", "", "| Header | Category | Files | PCH |", "|---|---|---:|---|");
  for (const item of report.pch_candidates) rows.push(`| ${item.header} | ${item.category} | ${item.files.length} | ${item.covered_by.join(", ") || "candidate"} |`);
  rows.push("", "## Retained standard and Linux/POSIX includes", "", "Every retained directive has a reason in the JSON; compile contexts include forced-PCH evidence and policy contradictions.", "", "| Reason | Files | Includes |", "|---|---:|---:|");
  for (const item of report.retained_system_includes) rows.push(`| ${item.reason} | ${item.files.length} | ${item.includes} |`);
  rows.push("", "## Same-folder bundles", "", `Showing the first 25 of ${report.bundles.length} pair-derived clusters, ranked by consumers outside the folder, then total consumers. All clusters and every include remain in the JSON.`, "",
    "Pairs are expanded with the headers shared by all their consumers. This does not enumerate every possible larger subset. Full locations, attribution strength, conditional flags, compiler targets and rewrite reasons are in the JSON.");
  for (const bundle of report.bundles.slice(0, 25)) rows.push("", `### ${bundle.folder} (${bundle.consumers.length} consumers; ${bundle.outside_folder.length} outside folder)`, "",
    bundle.headers.map((header) => `- ${header}`).join("\n"), "", `Consumers: ${bundle.consumers.join(", ")}`);
  return `${rows.join("\n")}\n`;
}

export function parseIncludeArgs(args) {
  if (args.length === 1 && ["--help", "-h"].includes(args[0])) return { mode: "help" };
  const options = { mode: args[0] ?? "report", output: "output/include-audit", build: `${process.env.MMLTK_CONTAINER_CACHE_ROOT ?? ".cache"}/cmake/release` };
  if (!["report", "preview", "fix"].includes(options.mode)) throw new Error("expected report, preview or fix; use --help");
  for (let i = 1; i < args.length; i += 2) {
    if (!["--output", "--build-dir"].includes(args[i]) || !args[i + 1]) throw new Error("expected --output PREFIX or --build-dir PATH");
    options[args[i] === "--output" ? "output" : "build"] = args[i + 1];
  }
  return options;
}

function main() {
  const options = parseIncludeArgs(process.argv.slice(2));
  if (options.mode === "help") {
    console.log("Usage: ./mmltk --audit-includes [--refresh] [report|preview|fix] [--output PREFIX] [--build-dir PATH]\n" +
      "Wrapper --refresh configures the Release graph before scanning; use after changing CMake PCH registrations.\n" +
      "Writes PREFIX.json and PREFIX.md (default output/include-audit). Includes are sorted in the report; source order is preserved.\n" +
      "Fix checks build/toolchain invariants and replaces includes only with a target-local forced PCH in every compile context.\n" +
      "Reports retain every compile context and explain remaining standard/Linux includes and contradictory PCH evidence.\n" +
      "Declaration headers, CUDA, modules and targets without compiled PCHs retain direct includes.\n" +
      "Macro includes and include_next remain explicit. PCH definitions are never rewritten.\n" +
      "PCH additions/registrations and folder/umbrella changes require source review; frequency is candidate evidence.");
    return;
  }
  const root = process.cwd(), inventory = collectInventoryInputs(), deleted = new Set(inventory.deleted);
  const paths = sorted([...inventory.tracked, ...inventory.untracked]).filter((path) => !deleted.has(path) &&
    !AUTHORED_PROFILE.excludedPrefixes.some((prefix) => path.startsWith(prefix)) &&
    !AUTHORED_PROFILE.generatedOutputPatterns.some((pattern) => pattern.test(path)) && existsSync(path) && statSync(path).isFile());
  const sources = new Map(paths.filter((path) => authoredLanguage(path) === "cpp").map((path) => [path, readFileSync(path, "utf8")]));
  const database = `${options.build}/compile_commands.json`;
  if (!existsSync(database)) throw new Error(`missing ${database}; run ./mmltk --build or select an existing --build-dir`);
  // Reuse the build authority for creation/use compiler, flags, environment and
  // generated-header agreement before any automatic source mutation.
  if (options.mode === "fix") execFileSync("python3",
    ["tools/check_toolchain_invariants.py", "--build-dir", options.build, "--repo-root", root], { stdio: "inherit" });
  const contexts = compilationContexts(JSON.parse(readFileSync(database, "utf8")), root);
  const report = buildIncludeReport(paths, sources, contexts, root);
  report.build_directory = options.build; report.mode = options.mode;
  writeAtomic(`${options.output}.json`, `${JSON.stringify(report, null, 2)}\n`);
  writeAtomic(`${options.output}.md`, renderIncludeMarkdown(report));
  if (options.mode === "fix") {
    // Verify the complete snapshot before changing any source.
    for (const [path, source] of sources) if (readFileSync(path, "utf8") !== source) throw new Error(`source changed during audit: ${path}`);
    for (const file of report.replacements.filter((file) => file.after !== undefined)) {
      const mode = statSync(file.path).mode;
      writeAtomic(file.path, file.after); chmodSync(file.path, mode);
    }
  }
  console.log(`${options.mode}: ${JSON.stringify(report.summary)}; ${options.output}.{json,md}`);
}
if (process.argv[1] && pathToFileURL(process.argv[1]).href === import.meta.url) {
  try { main(); } catch (error) { console.error(error.message); process.exitCode = 2; }
}
