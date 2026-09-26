// Human review evidence only. Shapes are lexical, not semantic equivalence,
// extraction permission, a source-registration inventory, or a rewrite rule.
import { SourceIndex, tokenizeCpp } from "./cpd_patterns.mjs";
import { sourceOccurrence } from "./declaration_patterns.mjs";

export function physicalLines(source) {
  let count = source.length && !source.endsWith("\n") ? 1 : 0;
  for (const char of source) if (char === "\n") ++count;
  return count;
}

const IDENTIFIER = /^[A-Za-z_]\w*$/u;
const FIXED = new Set(["template", "typename", "class", "struct", "const", "constexpr", "consteval", "static", "inline", "bool", "void", "true", "false", "requires", "return"]);

function shapedOccurrence(kind, occurrence) {
  const tokens = tokenizeCpp(occurrence.text);
  const values = tokens.map(({ text }) => text);
  const shape = tokens.map(({ text, kind }) => kind === "literal" ? "$literal" : /^\d/u.test(text) ? "$number" :
    IDENTIFIER.test(text) && !FIXED.has(text) ? "$identifier" : text);
  // Qualification reuse is a shared prefix, not merely the same number of
  // namespace separators across unrelated owners.
  if (kind === "qualification") for (let i = 0; i + 1 < values.length; ++i) shape[i] = values[i];
  return { kind, occurrence, values, shape, key: JSON.stringify([kind, shape]) };
}

function structuralOccurrences(path, source) {
  const index = new SourceIndex(source), { tokens, pairs } = index;
  const occurrences = [];
  const add = (kind, start, end) => {
    if (end <= start || end - start > 2048) return;
    const occurrence = sourceOccurrence(path, source, index.occurrence(path, start, end), index.lines);
    const owner = index.functions[index.owners[start]]?.name ?? index.records[index.recordOwners[start]]?.name ?? null;
    occurrences.push(shapedOccurrence(kind, { ...occurrence, owner }));
  };
  for (let i = 0; i < tokens.length; ++i) {
    // Complete small trait declarations, including primary templates and
    // specializations. A bounded window cannot swallow an enclosing system.
    if (tokens[i].text === "template" && tokens[i + 1]?.text === "<") {
      for (let end = i + 2; end < Math.min(tokens.length, i + 2048); ++end) {
        if (tokens[end].text === ";") break;
        if (tokens[end].text !== "struct") continue;
        if (!/^(?:has_|is_)|Annotation$/u.test(tokens[end + 1]?.text ?? "")) break;
        while (end < Math.min(tokens.length, i + 2048) && !["{", ";"].includes(tokens[end].text)) ++end;
        const close = pairs.get(end);
        if (close > end && tokens[close + 1]?.text === ";") add("trait_declaration", i, close + 2);
        break;
      }
    }
    // Same-name assignments and visitor calls retain the full source statement,
    // including conversions and receiver spelling. Only these demonstrated
    // bounded forms are recognized; renamed/selective mappings remain explicit.
    if (tokens[i].text === "=" && [".", "->"].includes(tokens[i - 2]?.text) && IDENTIFIER.test(tokens[i - 1]?.text ?? "")) {
      const member = tokens[i - 1].text;
      let end = i + 1;
      while (end < Math.min(tokens.length, i + 128) && ![";", "{"].includes(tokens[end].text)) ++end;
      if (tokens[end]?.text === ";" && tokens.slice(i + 1, end).some((token, n, right) => token.text === member && [".", "->"].includes(right[n - 1]?.text))) {
        let start = i - 3;
        while (start > 0 && ![";", "{", "}"].includes(tokens[start - 1].text) && i - start < 128) --start;
        add("same_name_mapping", start, end + 1);
      }
    }
    if (tokens[i].kind === "literal" && tokens[i - 1]?.text === "(" && tokens[i + 1]?.text === ",") {
      const name = /^"([A-Za-z_]\w*)"$/u.exec(tokens[i].text)?.[1];
      const close = pairs.get(i - 1);
      if (name && close > i && close - i < 128 && tokens[close - 1]?.text === name && [".", "->"].includes(tokens[close - 2]?.text) && tokens[close + 1]?.text === ";") {
        add("same_name_mapping", Math.max(0, i - 2), close + 2);
      }
    }
  }
  return occurrences;
}

function associateRawMatches(occurrences, rawMatches) {
  const byPath = new Map();
  const add = (path, start, end, type, value) => {
    if (!byPath.has(path)) byPath.set(path, []);
    byPath.get(path).push({ at: start, open: true, type, value }, { at: end, open: false, type, value });
  };
  for (const occurrence of occurrences) {
    occurrence.raw_match_ids = new Set();
    add(occurrence.path, occurrence.start_offset, occurrence.end_offset, "candidate", occurrence);
  }
  for (const match of rawMatches) for (const occurrence of match.occurrences) {
    if (byPath.has(occurrence.path)) add(occurrence.path, occurrence.start_offset, occurrence.end_offset, "raw", match.id);
  }
  for (const events of byPath.values()) {
    events.sort((a, b) => a.at - b.at || Number(a.open) - Number(b.open));
    const candidates = new Set(), raw = new Map();
    for (const event of events) {
      if (event.type === "candidate") {
        if (event.open) {
          for (const id of raw.keys()) event.value.raw_match_ids.add(id);
          candidates.add(event.value);
        } else candidates.delete(event.value);
      } else if (event.open) {
        raw.set(event.value, (raw.get(event.value) ?? 0) + 1);
        for (const occurrence of candidates) occurrence.raw_match_ids.add(event.value);
      } else {
        const count = raw.get(event.value) - 1;
        if (count) raw.set(event.value, count);
        else raw.delete(event.value);
      }
    }
  }
  for (const occurrence of occurrences) occurrence.raw_match_ids = [...occurrence.raw_match_ids].sort((a, b) => a - b);
}

function unionLines(occurrences) {
  const byPath = new Map();
  for (const occurrence of occurrences) {
    if (!byPath.has(occurrence.path)) byPath.set(occurrence.path, []);
    byPath.get(occurrence.path).push(occurrence);
  }
  let count = 0;
  for (const ranges of byPath.values()) {
    ranges.sort((a, b) => a.start - b.start || a.end - b.end);
    let end = 0;
    for (const range of ranges) { count += Math.max(0, range.end - Math.max(end, range.start - 1)); end = Math.max(end, range.end); }
  }
  return count;
}

export function buildReview(files, rawReport) {
  const groups = new Map(), classified = new Map();
  for (const occurrence of rawReport.classified_context) {
    if (!classified.has(occurrence.path)) classified.set(occurrence.path, []);
    classified.get(occurrence.path).push(occurrence);
  }
  for (const { path, source, language } of files) {
    if (language !== "cpp") continue;
    const entries = structuralOccurrences(path, source);
    for (const candidate of classified.get(path) ?? []) {
      const kind = candidate.categories.includes("descriptor_exclusion") ? "cli_exclusion" :
        candidate.categories.includes("nested_descriptor") ? "cli_binding" : candidate.categories.includes("qualified_name") ? "qualification" : null;
      if (kind && (kind === "qualification" || candidate.complete)) {
        const { context, ...occurrence } = candidate;
        entries.push(shapedOccurrence(kind, { ...occurrence, owner: context.owner }));
      }
    }
    for (const entry of entries) {
      if (!groups.has(entry.key)) groups.set(entry.key, new Map());
      groups.get(entry.key).set(`${path}:${entry.occurrence.start_offset}:${entry.occurrence.end_offset}`, entry);
    }
  }
  const families = [];
  for (const [, grouped] of [...groups].sort(([a], [b]) => a.localeCompare(b, "en"))) {
    if (grouped.size < 3) continue;
    const entries = [...grouped.values()].sort((a, b) => a.occurrence.path.localeCompare(b.occurrence.path, "en") || a.occurrence.start_offset - b.occurrence.start_offset);
    const first = entries[0];
    const varying = first.values.map((value, index) => entries.some((entry) => entry.values[index] !== value) ? index : -1).filter((index) => index >= 0);
    families.push({ id: families.length + 1, status: "unreviewed", kind: first.kind,
      invariant_tokens: first.values.map((value, index) => varying.includes(index) ? null : value),
      varying_slots: varying.map((index) => ({ token: index, kind: first.shape[index] })),
      occurrence_count: entries.length, file_count: new Set(entries.map(({ occurrence }) => occurrence.path)).size,
      owner_count: new Set(entries.filter(({ occurrence }) => occurrence.owner !== null).map(({ occurrence }) => `${occurrence.path}:${occurrence.owner}`)).size,
      unresolved_owner_occurrences: entries.filter(({ occurrence }) => occurrence.owner === null).length,
      covered_lines: unionLines(entries.map(({ occurrence }) => occurrence)),
      occurrences: entries.map(({ occurrence, values }) => ({ ...occurrence, varying_values: varying.map((index) => values[index]) })) });
  }
  const occurrences = families.flatMap((family) => family.occurrences);
  associateRawMatches(occurrences, rawReport.raw_matches);
  const familyIds = new Map();
  for (const family of families) for (const occurrence of family.occurrences) {
    if (!familyIds.has(occurrence.path)) familyIds.set(occurrence.path, new Set());
    familyIds.get(occurrence.path).add(family.id);
  }
  const inventory = files.map(({ path, source, language, sha256 }) => ({ path, language, sha256,
    physical_lines: physicalLines(source), bytes: Buffer.byteLength(source),
    test_evidence: { dedicated: /(?:^|\/)(?:tests?|fixtures)(?:\/|\.)|(?:\.test\.|(?:^|\/)test_[^/]+|(?:^|\/)tests\.rs$)/u.test(path),
      embedded: language === "rust" ? /#\s*\[\s*(?:test|cfg\s*\(\s*test\s*\))\s*\]/u.test(source) : language === "cpp" && /\b(?:TEST_CASE|TEMPLATE_TEST_CASE|SCENARIO)\s*\(/u.test(source) },
    family_ids: [...(familyIds.get(path) ?? [])], status: "unreviewed",
    analysis: language === "cpp" ? "bounded lexical patterns" : "size triage only" })).sort((a, b) => a.path.localeCompare(b.path, "en"));
  return { purpose: "Manual lexical review candidates; no semantic-equivalence or removable-line claim", threshold: 500,
    inventory, queue: inventory.filter((file) => file.physical_lines > 500 || file.family_ids.length).map(({ path }) => path), families,
    summary: { authored_files: inventory.length, large_files: inventory.filter((file) => file.physical_lines > 500).length,
      families: families.length, distinct_occurrences: new Set(occurrences.map((entry) => `${entry.path}:${entry.start_offset}:${entry.end_offset}`)).size,
      covered_lines: unionLines(occurrences) } };
}

export function renderReviewMarkdown(review) {
  const escape = (text) => String(text).replace(/[|`<>\r\n]/gu, (char) => `&#${char.charCodeAt(0)};`);
  const queued = new Set(review.queue);
  const lines = ["# Declaration and large-file review", "", review.purpose + ".", "",
    "All entries are **unreviewed**. Covered lines are the union of evidence spans, not removable lines or production-line counts.", "",
    "| File | Language | Physical lines | Test evidence | Families |", "| --- | --- | ---: | --- | ---: |"];
  for (const file of review.inventory) if (queued.has(file.path)) {
    const tests = Object.entries(file.test_evidence).filter(([, present]) => present).map(([kind]) => kind).join(", ") || "none recognized";
    lines.push(`| ${escape(file.path)} | ${file.language} | ${file.physical_lines} | ${tests} | ${file.family_ids.length} |`);
  }
  for (const family of review.families) {
    lines.push("", `## Family ${family.id}: ${family.kind} (unreviewed)`, "",
      `${family.occurrence_count} distinct occurrences in ${family.file_count} files; ${family.covered_lines} evidence lines after overlap merging.`, "",
      `Varying token slots: ${family.varying_slots.map(({ token, kind }) => `${token} (${kind})`).join(", ") || "none"}. Full values and anchors are in JSON.`);
    for (const occurrence of family.occurrences.slice(0, 3)) {
      const excerpt = occurrence.text.split("\n").slice(0, 8).join("\n").slice(0, 800);
      lines.push("", `- ${escape(occurrence.path)}:${occurrence.start}:${occurrence.column}–${occurrence.end}:${occurrence.endColumn}; raw match IDs: ${occurrence.raw_match_ids.slice(0, 12).join(", ") || "none"}${occurrence.raw_match_ids.length > 12 ? " (more in JSON)" : ""}`, "",
        `<pre>${excerpt.replace(/&/gu, "&amp;").replace(/</gu, "&lt;").replace(/>/gu, "&gt;")}${excerpt.length < occurrence.text.length ? "\n… (bounded excerpt)" : ""}</pre>`);
    }
    if (family.occurrences.length > 3) lines.push("", "Remaining occurrence anchors and varying values are in JSON.");
  }
  return lines.join("\n") + "\n";
}
