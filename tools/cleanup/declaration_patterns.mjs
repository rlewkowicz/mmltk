// Declaration evidence and strictly syntactic authoring rules. This is not a
// C++ semantic parser: uncertain lookup/preprocessing is an explicit manual case.
import { SourceIndex, tokenizeCpp } from "./cpd_patterns.mjs";

export const ANNOTATION_HEADER = "src/frameworks/reflection/declaration_annotations.h";
const POLICY_NAMESPACE = "mmltk::frameworks::reflection";
export const ANNOTATION_MACROS = Object.freeze({
  MaxBytes: "MMLTK_MAX_BYTES", MinBytes: "MMLTK_MIN_BYTES", MaxItems: "MMLTK_MAX_ITEMS",
  Minimum: "MMLTK_MINIMUM", Maximum: "MMLTK_MAXIMUM", Finite: "MMLTK_FINITE",
  RuntimeDestination: "MMLTK_RUNTIME_DESTINATION", Presentation: "MMLTK_PRESENTATION",
  CatalogProvider: "MMLTK_CATALOG",
});
export const MACRO_NAMES = new Set([...Object.values(ANNOTATION_MACROS), "MMLTK_MAX_PATH_BYTES", "MMLTK_MAX_NAME_BYTES", "MMLTK_MINIMUM_VALUE"]);
const IDENTIFIER = /^[a-zA-Z_]\w*$/u;

export function lineStarts(source) {
  const lines = [0];
  for (let offset = 0; offset < source.length; ++offset) if (source[offset] === "\n") lines.push(offset + 1);
  return lines;
}
function lineAt(lines, offset) {
  let low = 0, high = lines.length;
  while (low < high) {
    const middle = (low + high) >>> 1;
    if (lines[middle] <= offset) low = middle + 1;
    else high = middle;
  }
  return low;
}
export function sourceOccurrence(path, source, occurrence, lines = lineStarts(source)) {
  const startOffset = lines[occurrence.start - 1] + (occurrence.column ?? 1) - 1;
  const endOffset = occurrence.endColumn === undefined ? lines[occurrence.end] ?? source.length :
    lines[occurrence.end - 1] + occurrence.endColumn;
  if (!Number.isInteger(startOffset) || !Number.isInteger(endOffset) || startOffset < 0 || endOffset < startOffset || endOffset > source.length) {
    throw new Error(`invalid source span: ${path}:${occurrence.start}-${occurrence.end}`);
  }
  return { ...occurrence, path, start_offset: startOffset, end_offset: endOffset, text: source.slice(startOffset, endOffset) };
}
function span(path, source, lines, start, end) {
  const startLine = lineAt(lines, start), endLine = lineAt(lines, Math.max(start, end - 1));
  return { path, start: startLine, end: endLine, column: start - lines[startLine - 1] + 1,
    endColumn: end - lines[endLine - 1], start_offset: start, end_offset: end, text: source.slice(start, end) };
}
function spelling(tokens) { return tokens.map((token) => token.text).join(""); }

export function preprocessorRegions(source, tokens = tokenizeCpp(source)) {
  const regions = [], conditional = [];
  const stack = [];
  for (let index = 0; index < tokens.length; ++index) {
    const token = tokens[index];
    const startOfLine = source.lastIndexOf("\n", token.offset - 1) + 1;
    // Comments are preprocessing whitespace, including comments beginning on a
    // preceding line. The shared lexer has already removed them from tokens.
    if (token.text !== "#" || (tokens[index - 1]?.end ?? 0) > startOfLine || token.offset < (regions.at(-1)?.end ?? 0)) continue;
    let end = source.indexOf("\n", token.end);
    if (end < 0) end = source.length;
    while (/\\\r?$/u.test(source.slice(token.offset, end)) && end < source.length) {
      end = source.indexOf("\n", end + 1);
      if (end < 0) end = source.length;
    }
    const text = source.slice(token.offset, end);
    const directive = /^#\s*(\w+)/u.exec(text)?.[1];
    regions.push({ start: token.offset, end, text, directive });
    if (["if", "ifdef", "ifndef"].includes(directive)) stack.push(token.offset);
    if (directive === "endif" && stack.length) {
      const start = stack.pop();
      if (!stack.length) conditional.push({ start, end });
    }
  }
  if (stack.length) conditional.push({ start: stack[0], end: source.length });
  return { regions, conditional };
}
export function conflictingMacros(path, source) {
  if (path === ANNOTATION_HEADER) return [];
  return preprocessorRegions(source).regions.flatMap(({ text }) => {
    const name = /^#\s*(?:define|undef)\s+(\w+)/u.exec(text)?.[1];
    return MACRO_NAMES.has(name) ? [name] : [];
  });
}
function inside(regions, offset) { return regions.some(({ start, end }) => start <= offset && offset < end); }

function policyAliases(tokens, pairs, regions, conditional) {
  const aliases = new Map(), scopes = [];
  for (let index = 0; index + 1 < tokens.length; ++index) {
    while (scopes.length && scopes.at(-1) < index) scopes.pop();
    if (tokens[index].text === "{" && pairs.get(index) > index) scopes.push(pairs.get(index));
    if (inside(regions, tokens[index].offset) || inside(conditional, tokens[index].offset) ||
        tokens[index].text !== "namespace" || tokens[index + 2]?.text !== "=") continue;
    let end = index + 3;
    while (end < tokens.length && tokens[end].text !== ";") ++end;
    if (end === tokens.length) continue;
    const name = tokens[index + 1].text;
    const target = spelling(tokens.slice(index + 3, end));
    if (!aliases.has(name)) aliases.set(name, []);
    aliases.get(name).push({ target, global: scopes.length === 0, start: tokens[end].end, end: tokens[scopes.at(-1)]?.offset ?? Infinity });
  }
  // Competing type/namespace declarations are conservatively manual, even when
  // a complete C++ lookup could prove them unrelated. Never infer imported aliases.
  for (let index = 0; index + 1 < tokens.length; ++index) {
    if (["class", "struct", "using", "typename"].includes(tokens[index].text) ||
        (tokens[index].text === "namespace" && tokens[index + 2]?.text !== "=")) {
      if (aliases.has(tokens[index + 1].text)) aliases.set(tokens[index + 1].text, []);
    }
  }
  return aliases;
}
function canonicalName(tokens, aliases) {
  const qualified = spelling(tokens);
  // Includes may introduce names in any enclosing scope. A lexical scan cannot
  // prove relative qualified lookup, even when this file declares no shadow.
  if (!qualified.startsWith("::")) return "";
  const name = qualified.slice(2);
  if (name.startsWith(`${POLICY_NAMESPACE}::`)) return name;
  const split = name.indexOf("::");
  if (split <= 0) return "";
  const visible = aliases.get(name.slice(0, split))?.filter((alias) => alias.global && alias.start <= tokens[0].offset && tokens[0].offset < alias.end);
  if (!visible?.length) return "";
  const alias = visible.at(-1);
  return alias.target === `::${POLICY_NAMESPACE}` ? POLICY_NAMESPACE + name.slice(split) : "";
}
function annotationRewrite(tokens, source, aliases) {
  // Input is the expression between [[= and ]]. Only a single canonical
  // policy construction is accepted; no comma-list/unknown composition rewrite.
  const brace = tokens.findIndex((token) => token.text === "{");
  if (brace < 0 || tokens.at(-1)?.text !== "}") return { reason: "annotation expression is not a known policy construction" };
  let depth = 0, close = brace;
  for (; close < tokens.length; ++close) {
    if (tokens[close].text === "{") ++depth;
    if (tokens[close].text === "}" && --depth === 0) break;
  }
  if (close !== tokens.length - 1) return { reason: "annotation contains unfamiliar expressions or composition" };
  const head = tokens.slice(0, brace), valueTokens = tokens.slice(brace + 1, -1);
  const angle = head.findIndex((token) => token.text === "<");
  const nameTokens = angle < 0 ? head : head.slice(0, angle);
  const name = canonicalName(nameTokens, aliases);
  const policy = name.startsWith(`${POLICY_NAMESPACE}::`) ? name.slice(POLICY_NAMESPACE.length + 2) : null;
  const macro = ANNOTATION_MACROS[policy];
  if (!macro) return { reason: "policy lookup is not proven global or is outside the annotation spelling surface" };
  const value = valueTokens.length ? source.slice(valueTokens[0].offset, valueTokens.at(-1).end) : "";
  const typeTokens = angle < 0 ? [] : head.slice(angle + 1, -1);
  const type = angle >= 0 && head.at(-1)?.text.endsWith(">") ? source.slice(head[angle].end, head.at(-1).end - 1).trim() : "";
  if (angle >= 0 && ![">", ">>"].includes(head.at(-1)?.text)) return { reason: "complex template closure requires manual review" };
  if (["Minimum", "Maximum"].includes(policy)) {
    if (!type && value && policy === "Minimum") return { replacement: `MMLTK_MINIMUM_VALUE(${value})`, policy };
    if (!type || !value || typeTokens.some((token) => token.text === ",")) return { reason: "typed bound requires one visible type argument; use a scoped type alias for template commas" };
    return { replacement: `${macro}(${type}, ${value})`, policy };
  }
  if (["Presentation", "CatalogProvider"].includes(policy)) {
    if (!type || value) return { reason: "unfamiliar templated policy construction" };
    return { replacement: `${macro}(${type})`, policy };
  }
  if (type) return { reason: "unfamiliar policy template arguments" };
  if (["Finite", "RuntimeDestination"].includes(policy)) {
    if (value) return { reason: "marker policy has unfamiliar initialization" };
    return { replacement: macro, policy };
  }
  if (!value) return { reason: "bound policy has no explicit value" };
  if (policy === "MaxBytes") {
    const bound = canonicalName(valueTokens, aliases);
    if (bound === `${POLICY_NAMESPACE}::kMaximumPathBytes`) return { replacement: "MMLTK_MAX_PATH_BYTES", policy };
    if (bound === `${POLICY_NAMESPACE}::kMaximumNameBytes`) return { replacement: "MMLTK_MAX_NAME_BYTES", policy };
  }
  return { replacement: `${macro}(${value})`, policy };
}

export function classifyDeclarations(path, source, { macroConflicts = new Set() } = {}) {
  const index = new SourceIndex(source), { tokens, pairs, lines } = index;
  const { regions, conditional } = preprocessorRegions(source, tokens);
  const aliases = index.balanced ? policyAliases(tokens, pairs, regions, conditional) : new Map();
  const localConflicts = new Set([...macroConflicts, ...conflictingMacros(path, source)]);
  const candidates = [], annotations = [];
  const tokenEnd = (position) => pairs.get(position) > position ? pairs.get(position) : position;
  const context = (start, end) => {
    const record = index.records[index.recordOwners[start]];
    const fn = index.functions[index.owners[start]];
    let finish = end;
    while (finish < tokens.length && finish < end + 150 && ![";", "}"].includes(tokens[finish].text)) ++finish;
    const memberTokens = tokens.slice(end, finish);
    const equal = memberTokens.findIndex((token) => ["=", "{"].includes(token.text));
    const declaration = equal < 0 ? memberTokens : memberTokens.slice(0, equal);
    const member = declaration.at(-1)?.text;
    const startLine = Math.max(1, lineAt(lines, tokens[start].offset) - 1);
    const endLine = Math.min(lines.length, lineAt(lines, tokens[Math.max(start, end - 1)].end) + 1);
    return { owner: record?.name ?? fn?.name ?? null,
      member: IDENTIFIER.test(member ?? "") ? member : null,
      start: startLine, end: endLine, text: source.slice(lines[startLine - 1], lines[endLine] ?? source.length) };
  };
  const add = (start, end, categories, disposition, reason, extra = {}) => {
    if (end <= start) return;
    const candidate = { ...span(path, source, lines, tokens[start].offset, tokens[end - 1].end),
      categories, context: context(start, end), disposition, reason, ...extra };
    candidates.push(candidate);
    return candidate;
  };
  for (let i = 0; i < tokens.length; ++i) {
    const text = tokens[i].text;
    if (tokens[i].kind !== "code" || inside(regions, tokens[i].offset)) continue;
    if (text === "[" && tokens[i + 1]?.text === "[" && tokens[i + 2]?.text === "=") {
      const close = pairs.get(i);
      if (close === undefined || close <= i) {
        add(i, Math.min(i + 4, tokens.length), ["reflection_annotation"], "manual", "unbalanced annotation syntax");
        continue;
      }
      const result = annotationRewrite(tokens.slice(i + 3, close - 1), source, aliases);
      const annotationText = source.slice(tokens[i].offset, tokens[close].end);
      // Trivia inside a replaced span has no unambiguous attachment after the
      // replacement. Retain it verbatim and request a manual decision.
      const hasComment = tokenizeCpp(annotationText, { trivia: true }).some((token) => token.kind === "comment");
      const conflict = result.replacement && localConflicts.has(result.replacement.match(/^\w+/u)[0]);
      const reason = path.endsWith(".cppm") ? "module authoring requires manual dependency placement" : inside(conditional, tokens[i].offset) ? "conditional preprocessing requires manual lookup" :
        hasComment ? "comment inside annotation must retain its source attachment" :
        conflict ? "conflicting annotation macro definition or undefinition" : result.reason;
      const categories = ["reflection_annotation"];
      if (["Minimum", "Maximum", "MaxBytes", "MinBytes", "MaxItems"].includes(result.policy)) categories.push("typed_limit");
      const candidate = add(i, close + 1, categories, reason ? "manual" : "rewrite", reason ?? "canonical typed construction; macro expands to the same annotation expression",
        { ...(result.policy ? { policy: result.policy } : {}), ...(!reason ? { replacement: result.replacement } : {}) });
      annotations.push({ start: i, end: close + 1, candidate });
      i = close;
      continue;
    }
    if (text === "template" && tokens[i + 1]?.text === "<") {
      let end = i + 2, depth = 1;
      for (; end < tokens.length && depth; ++end) {
        if (tokens[end].text === "<") ++depth;
        else if ([">", ">>"].includes(tokens[end].text)) depth -= tokens[end].text.length;
      }
      add(i, end, ["template_declaration"], "manual", "template constraints and specialization ownership need semantic review");
    }
    if (MACRO_NAMES.has(text)) {
      const end = tokens[i + 1]?.text === "(" ? tokenEnd(i + 1) + 1 : i + 1;
      const policy = Object.keys(ANNOTATION_MACROS).find((name) => ANNOTATION_MACROS[name] === text) ??
        (text === "MMLTK_MINIMUM_VALUE" ? "Minimum" : "MaxBytes");
      const categories = ["reflection_annotation", "shortened_annotation"];
      if (["Minimum", "Maximum", "MaxBytes", "MinBytes", "MaxItems"].includes(policy)) categories.push("typed_limit");
      const reason = localConflicts.has(text) ? "conflicting annotation macro definition or undefinition" :
        path.endsWith(".cppm") ? "module authoring requires manual dependency placement" :
        inside(conditional, tokens[i].offset) ? "conditional preprocessing requires manual lookup" : null;
      const candidate = add(i, end, categories, reason ? "manual" : "retained", reason ?? "already uses the canonical syntax-only spelling", { policy });
      annotations.push({ start: i, end, candidate });
      i = end - 1;
      continue;
    }
    if (/^MMLTK_REFLECT_(?:FIELDS|ENUM)$/u.test(text)) {
      const end = tokens[i + 1]?.text === "(" ? tokenEnd(i + 1) + 1 : i + 1;
      add(i, end, ["registration_macro"], "retained", "canonical declaration-local ADL materialization; shortening would hide the boundary");
      i = end - 1;
      continue;
    }
    if (text === "operator" && tokens[i + 1]?.text === "==") {
      let end = i + 2;
      while (end < tokens.length && ![";", "{"].includes(tokens[end].text)) ++end;
      if (tokens.slice(i, end).some((token) => token.text === "default")) add(i, Math.min(end + 1, tokens.length), ["defaulted_equality"], "retained", "ordinary equality keeps qualifiers, visibility and diagnostics explicit");
      continue;
    }
    if (text === "[" && tokens[i + 1]?.text === "[") {
      let end = tokenEnd(i) + 1;
      if (tokens[end]?.text === "__device__" && tokens[end + 1]?.text === "__forceinline__") end += 2;
      add(i, end, ["attribute_qualifier"], "manual", "standard attributes remain explicit; repeated CUDA qualifier groups can use a local valid macro");
      i = end - 1;
      continue;
    }
    if (["static_cast", "reinterpret_cast"].includes(text)) {
      let end = i + 1;
      while (end < tokens.length && end < i + 40 && tokens[end].text !== "(") ++end;
      if (tokens[end]?.text === "(") {
        add(i, tokenEnd(end) + 1, ["cast"], "manual", "conversion semantics, evaluation and alignment need an owner-level decision");
      }
      continue;
    }
    if (text === "std" && tokens[i + 1]?.text === "::" && ["optional", "vector", "array", "span", "inplace_vector"].includes(tokens[i + 2]?.text)) {
      let end = i + 3;
      if (tokens[end]?.text === "<") {
        let depth = 1;
        for (++end; end < tokens.length && depth > 0; ++end) {
          if (tokens[end].text === "<") ++depth;
          else if ([">", ">>"].includes(tokens[end].text)) depth -= tokens[end].text.length;
        }
      }
      const categories = [tokens[i + 2].text === "optional" ? "optional_declaration" : "collection_declaration"];
      if (end - i > 12) categories.push("long_type");
      add(i, end, categories, "retained", "standard vocabulary preserves ownership and optionality without a parallel alias system");
      continue;
    }
    if (IDENTIFIER.test(text) && tokens[i + 1]?.text === "::" && tokens[i - 1]?.text !== "::") {
      let end = i + 1, depth = 0;
      while (tokens[end]?.text === "::" && IDENTIFIER.test(tokens[end + 1]?.text ?? "")) { end += 2; ++depth; }
      if (depth >= 2) add(i, end, ["qualified_name"], "manual", "use a meaningful scoped using declaration only after checking owner and lookup");
    }
    if (["option", "custom_option", "negative_flag", "option_with_item_policy"].includes(text) && tokens[i + 1]?.text === "<") {
      let end = i + 2;
      while (end < tokens.length && end < i + 100 && tokens[end].text !== "(") ++end;
      add(i, end, ["nested_descriptor"], "manual", "reuse typed member-path/descriptor APIs; preserve every option identity and policy");
    }
  }
  for (let i = 0; i < annotations.length;) {
    let end = i + 1;
    while (end < annotations.length && annotations[end - 1].end === annotations[end].start) ++end;
    if (end - i > 1) {
      const group = annotations.slice(i, end);
      const unknown = group.some(({ candidate }) => candidate.disposition === "manual");
      for (const { candidate } of group) {
        candidate.categories.push("adjacent_annotation_composition");
        if (unknown && candidate.disposition === "rewrite") {
          candidate.disposition = "manual";
          candidate.reason = "adjacent composition contains an unfamiliar or ambiguous annotation";
          delete candidate.replacement;
        }
      }
    }
    i = end;
  }
  for (const fn of index.functions) {
    if (fn.open > fn.start && !inside(regions, tokens[fn.start].offset)) {
      add(fn.start, fn.open, ["function_signature"], "retained", "ordinary function signature retains access, qualifiers and exception behavior");
    }
  }
  candidates.sort((a, b) => a.start_offset - b.start_offset || a.end_offset - b.end_offset);
  return { candidates };
}

export function formatDeclarationSource(path, source, options) {
  const result = classifyDeclarations(path, source, options);
  const edits = result.candidates.filter((candidate) => candidate.disposition === "rewrite")
    .map(({ start_offset: start, end_offset: end, replacement, reason }) => ({ start, end, replacement: replacement + (/\S/u.test(source[end] ?? "") ? " " : ""), reason }));
  const { regions, conditional } = preprocessorRegions(source);
  const hasInclude = regions.some((region) => region.directive === "include" && !inside(conditional, region.start) &&
    region.text.match(/^#\s*include\s*[<"]([^>"]+)[>"]/u)?.[1] === ANNOTATION_HEADER);
  const usesAnnotationMacros = result.candidates.some((candidate) => candidate.disposition === "retained" && candidate.categories.includes("shortened_annotation"));
  if ((edits.length || usesAnnotationMacros) && !hasInclude) {
    const firstInclude = regions.find((region) => region.directive === "include" && !inside(conditional, region.start));
    const pragma = regions.find((region) => /^#\s*pragma\s+once\b/u.test(region.text));
    const offset = firstInclude?.start ?? (pragma ? Math.min(source.length, pragma.end + 1) : 0);
    const newline = source.includes("\r\n") ? "\r\n" : "\n";
    edits.push({ start: offset, end: offset, replacement: `#include "${ANNOTATION_HEADER}"${newline}`, reason: "direct self-contained annotation dependency" });
  }
  edits.sort((a, b) => a.start - b.start || a.end - b.end);
  let output = "", cursor = 0;
  for (const edit of edits) {
    if (edit.start < cursor) throw new Error(`overlapping declaration edits in ${path}`);
    output += source.slice(cursor, edit.start) + edit.replacement;
    cursor = edit.end;
  }
  output += source.slice(cursor);
  return { ...result, edits, output, changed: output !== source };
}
