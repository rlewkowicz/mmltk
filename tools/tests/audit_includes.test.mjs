import test from "node:test";
import assert from "node:assert/strict";
import { scanIncludes, commandWords, compilationContexts, attributeInclude, repeatedBundles, pchReplacement, buildIncludeReport, parseIncludeArgs } from "../audit_includes.mjs";

const pch = "src/pch_std.h";
const context = { target: "owner", modules: false, pch: [pch], include_dirs: [], quote_dirs: [] };
const coverage = new Map([[pch, new Set(["vector", "string", "memory"])]]);
const rewrite = (path, source, contexts = [context]) => pchReplacement(path, source, scanIncludes(source), contexts, coverage);

test("include audit ignores comments and literals, retains branches, macros and continued directives", () => {
  const source = '// #include <fake>\n/* #include <fake> */\nconst auto s = R"x(\n#include <fake>\n)x";\n' +
    '# include <vector> // comment\n#if FLAG\n#include "own.h"\n#else\n#include HEADER\n#endif\n#include \\\n <string>\n# /* legal */ include <memory>\n';
  const scan = scanIncludes(source);
  assert.deepEqual(scan.includes.map(({ header, spelling, conditional }) => [header, spelling, conditional]), [
    ["vector", "angle", false], ["own.h", "quote", true], ["HEADER", "macro", true], ["string", "angle", false], ["memory", "angle", false],
  ]);
  assert.equal(scan.includes[0].line, 6);
  assert.equal(scan.includes[3].plain, false);
});

test("include audit attributes project paths before system names and exposes ambiguity", () => {
  const paths = new Set(["src/a/own.h", "src/a/detail.h", "src/b/detail.h", "third_party/vendor.h"]);
  const attr = (header, spelling = "quote", contexts = [context]) => attributeInclude({ header, spelling }, "src/a/file.cpp", paths, contexts, "/repo");
  assert.equal(attr("own.h").resolved, "src/a/own.h");
  assert.equal(attr("vector", "angle").category, "standard");
  assert.equal(attr("linux/fs.h", "angle").category, "linux-posix");
  assert.equal(attr("cuda.h", "angle").category, "external");
  assert.equal(attr("HEADER", "macro").category, "macro");
  assert.equal(attr("vendor.h").category, "vendored");
  assert.equal(attr("detail.h", "quote", [{ ...context, include_dirs: ["/repo/src/b"] }]).resolution, "ambiguous");
  assert.equal(attr("missing.h").category, "unresolved");
});

test("include audit reads actual target policy across multiple compile environments", () => {
  const entry = (target, extra = "") => ({ directory: "/repo/build", file: "/repo/src/a.cpp",
    command: `g++ -I '/repo/include dir' -isystem /opt/lib ${extra} -o CMakeFiles/${target}.dir/a.o -c /repo/src/a.cpp` });
  const contexts = compilationContexts([entry("one"), entry("two", "-fmodules")], "/repo",
    () => "header\t/repo/src/pch_std.h\nuse\t/repo/src/a.cpp\n", () => true).get("src/a.cpp");
  assert.equal(contexts.length, 2);
  assert.deepEqual(contexts[0].include_dirs, ["/repo/include dir", "/opt/lib"]);
  assert.deepEqual(contexts[0].pch, [pch]);
  assert.equal(contexts[1].modules, true);
  assert.deepEqual(commandWords('g++ -DNAME=\\"literal\\" "a b.cpp"'), ["g++", '-DNAME="literal"', "a b.cpp"]);
  assert.throws(() => commandWords("'unfinished"));
});

test("PCH replacement preserves local includes, comments, conditional branches and line endings", () => {
  const source = '#include "own.h"\r\n#include <vector>\r\n// reason\r\n#include <string>\r\n#if FLAG\r\n#include <memory>\r\n#endif\r\nint main() {}\r\n';
  const result = rewrite("src/a.cpp", source);
  assert.equal(result.after, '#include "own.h"\r\n#include "src/pch_std.h"\r\n// reason\r\n#if FLAG\r\n#include "src/pch_std.h"\r\n#endif\r\nint main() {}\r\n');
  assert.equal(result.removed - result.added, 1);
  assert.equal(result.retained.length, 0);
  assert.equal(rewrite("src/a.cpp", result.after).after, undefined);
});

test("PCH replacement stays limited to actual compiled PCH consumers", () => {
  const source = "#include <vector>\n#include <string>\n";
  for (const path of ["src/a.h", "src/a.cu", "src/a.c", "src/a.cppm", "src/a.cpp.in"]) assert.equal(rewrite(path, source).after, undefined);
  assert.equal(rewrite("src/a.cpp", source, []).after, undefined);
  assert.equal(rewrite("src/a.cpp", source, [context, { ...context, pch: [] }]).after, undefined);
  assert.equal(rewrite("src/a.cpp", source, [{ ...context, modules: true }]).after, undefined);
  assert.equal(rewrite("src/a.cpp", '#define _GNU_SOURCE\n' + source).after, `#define _GNU_SOURCE\n#include "${pch}"\n`);
  assert.equal(rewrite(pch, source).after, undefined);
  const json = 'nlohmann/json.hpp';
  const result = pchReplacement("src/a.cpp", source + `#include <${json}>\n`, scanIncludes(source + `#include <${json}>\n`), [context],
    new Map([...coverage, ["src/pch_json.h", new Set([json])]]));
  assert.equal(result.after, `#include "${pch}"\n#include <${json}>\n`);
});

test("PCH replacement covers single includes and preserves comments and continuations", () => {
  assert.equal(rewrite("src/a.cpp", "#include <vector>\n").after, `#include "${pch}"\n`);
  const source = '#include "src/pch_std.h"\n#include <vector>\n#include <string> // intent\n#include \\\n <memory>\n';
  assert.equal(rewrite("src/a.cpp", source).after, source.replace("#include <vector>\n", "").replace("<string>", `"${pch}"`).replace("<memory>", `"${pch}"`));
});

test("PCH replacement retains separate branches and include_next lookup semantics", () => {
  const source = '# /* legal */ if FIRST\n#include <vector>\n#else\n#include <string>\n#endif\n#include_next <memory>\n';
  const result = rewrite("src/a.cpp", source);
  assert.equal(result.after, source.replace("<vector>", `"${pch}"`).replace("<string>", `"${pch}"`));
  assert.equal(result.retained[0].reason, "include_next changes lookup");
});

test("include bundles count distinct consumers, close repeated pairs into triples and retain outside consumers", () => {
  const file = (path, names) => ({ path, includes: names.map((name) => ({ category: "project", resolved: `src/lib/${name}.h` })) });
  const bundles = repeatedBundles([file("src/a.cpp", ["a", "b", "c", "c"]), file("src/lib/use.cpp", ["a", "b", "c"]), file("src/b.cpp", ["a", "b"])]);
  assert.equal(bundles.length, 2);
  assert.deepEqual(bundles[0].headers, ["src/lib/a.h", "src/lib/b.h"]);
  assert.equal(bundles[0].consumers.length, 3);
  assert.equal(bundles[1].headers.length, 3);
  assert.deepEqual(bundles[1].outside_folder, ["src/a.cpp"]);
});

test("include frequencies use distinct files, exclude PCH definitions and include inactive evidence", () => {
  const sources = new Map([
    [pch, "#include <vector>\n#include <string>\n"],
    ["src/a.cpp", "#include <vector>\n#include <vector>\n#include <string>\n"],
    ["src/b.h", "#include <vector>\n"],
    ["src/c.cu", "#if FLAG\n#include <vector>\n#endif\n"],
  ]);
  const paths = [...sources.keys(), ...Array.from({ length: 11 }, (_, index) => `docs/page${index}.md`)];
  const report = buildIncludeReport(paths, sources, new Map([["src/a.cpp", [context]]]), "/repo");
  assert.deepEqual(report.pch_candidates.map((entry) => [entry.header, entry.files.length, entry.occurrences]), [["vector", 3, 4]]);
  assert.deepEqual(report.folders.map((folder) => [folder.path, folder.file_count]), [["docs", 11]]);
  assert.equal(report.summary.changed_files, 1);
  assert.equal(report.files.find((file) => file.path === "src/b.h").includes[0].category, "standard");
});

test("include audit defaults to reports under output and rejects unknown modes", () => {
  assert.equal(parseIncludeArgs([]).mode, "report");
  assert.equal(parseIncludeArgs([]).output, "output/include-audit");
  assert.equal(parseIncludeArgs(["fix", "--output", "output/custom"]).output, "output/custom");
  assert.throws(() => parseIncludeArgs(["sort"]));
  assert.throws(() => parseIncludeArgs(["fix", "--build-dir"]));
});
