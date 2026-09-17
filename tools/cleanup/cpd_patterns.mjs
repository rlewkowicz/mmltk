// CPD anonymizes every identifier, including operation and type names. Restore
// source context before treating those token matches as consolidation work.
// This is candidate triage, not a C++ parser or a proof of semantic equivalence.
import { readFileSync } from "node:fs";

export const MAX_CPD_FILTER_TOKENS = 99;

const TOKEN =
  /\s+|\/\/[^\n]*|\/\*[\s\S]*?(?:\*\/|$)|(?:u8|[uUL])?R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\1"|(?:u8|[uUL])?"(?:\\[\s\S]|[^"\\])*"|'(?:\\[\s\S]|[^'\\])*'|[0-9](?:[a-zA-Z0-9_.']|[eEpP][+-])*|[a-zA-Z_$][\w$]*|::|->|==|!=|<=|>=|&&|\|\||\+\+|--|<<|>>|[^\s]/gy;
const IDENTIFIER = /^[a-zA-Z_$][\w$]*$/u;
const CONTROL = new Set([
  "if", "for", "while", "switch", "catch", "sizeof", "alignof",
  "decltype", "noexcept", "requires", "static_assert", "constexpr", "consteval",
]);
const CASTS = new Set([
  "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast",
]);
const ASSERTIONS = /^(?:CHECK|REQUIRE|ASSERT)(?:_[A-Z0-9_]+)?$/u;
const NON_TYPES = new Set([
  ...CONTROL, "return", "throw", "case", "co_return", "co_yield",
  "delete", "new", "break", "continue", "else", "goto", "operator",
]);

function lowerBound(values, value, project = (entry) => entry) {
  let low = 0;
  let high = values.length;
  while (low < high) {
    const middle = (low + high) >>> 1;
    if (project(values[middle]) < value) low = middle + 1;
    else high = middle;
  }
  return low;
}

function tokenText(tokens, start, end) {
  return tokens.slice(start, end).map((token) => token.text).join(" ");
}

function parameterDeclarations(tokens, pairs, start, end) {
  const result = [];
  let templateDepth = 0;
  for (let index = start; index <= end; ++index) {
    if (index !== end) {
      const text = tokens[index].text;
      if (text === "<") ++templateDepth;
      else if (text === ">" || text === ">>") templateDepth = Math.max(0, templateDepth - text.length);
      const close = pairs.get(index);
      if (close !== undefined && close > index && close < end) {
        index = close;
        continue;
      }
      if (text !== "," || templateDepth !== 0) continue;
    }
    const parameter = tokens.slice(start, index);
    const equal = parameter.findIndex((token) => token.text === "=");
    result.push(equal < 0 ? parameter : parameter.slice(0, equal));
    start = index + 1;
  }
  return result;
}

function algorithmReceivers(tokens) {
  const result = [];
  const identities = new Map();
  const identity = (key) => {
    if (!identities.has(key)) identities.set(key, `$${identities.size}`);
    return identities.get(key);
  };
  for (let index = 0; index < tokens.length; ++index) {
    const text = tokens[index];
    let end = index;
    if (IDENTIFIER.test(text) && ![".", "->", "::"].includes(tokens[index - 1])) {
      while ([".", "->"].includes(tokens[end + 1]) && IDENTIFIER.test(tokens[end + 2] ?? "") &&
             tokens[end + 3] !== "(") end += 2;
      if ([".", "->"].includes(tokens[end + 1]) && tokens[end + 3] === "(") {
        result.push(identity(tokens.slice(index, end + 1).join(" ")));
        index = end;
        continue;
      }
    }
    result.push(/^\$\d+$/u.test(text) ? identity(text) : text);
  }
  return result;
}

function scopeOwners(scopes, size) {
  const owners = new Int32Array(size).fill(-1);
  const active = [];
  let next = 0;
  for (let index = 0; index < size; ++index) {
    while (active.length && scopes[active.at(-1)].end <= index) active.pop();
    while (scopes[next]?.open === index) active.push(next++);
    owners[index] = active.at(-1) ?? -1;
  }
  return owners;
}

class SourceIndex {
  constructor(source) {
    this.lines = [0];
    for (let index = 0; index < source.length; ++index) {
      if (source[index] === "\n") this.lines.push(index + 1);
    }
    this.tokens = [];
    TOKEN.lastIndex = 0;
    for (const match of source.matchAll(TOKEN)) {
      const text = match[0];
      if (/^\s|^\/[/*]/u.test(text)) continue;
      this.tokens.push({ text, offset: match.index, end: match.index + text.length });
    }
    this.pairs = new Map();
    const stack = [];
    const closes = { ")": "(", "]": "[", "}": "{" };
    this.balanced = true;
    for (let index = 0; index < this.tokens.length; ++index) {
      const text = this.tokens[index].text;
      if (text === "(" || text === "[" || text === "{") stack.push(index);
      else if (closes[text]) {
        const open = stack.pop();
        if (open === undefined || this.tokens[open].text !== closes[text]) {
          this.balanced = false;
          continue;
        }
        this.pairs.set(open, index);
        this.pairs.set(index, open);
      }
    }
    this.balanced &&= stack.length === 0;
    this.functions = [];
    this.records = [];
    this.indexFunctions();
    this.owners = scopeOwners(this.functions, this.tokens.length);
    this.recordOwners = scopeOwners(this.records, this.tokens.length);
    this.indexStatements();
    this.indexLocals();
  }

  indexStatements() {
    const size = this.tokens.length;
    this.statementStarts = new Int32Array(size);
    this.statementEnds = new Int32Array(size);
    const scopes = new Set([...this.functions, ...this.records].map((scope) => scope.open));
    let start = 0;
    const finish = (end) => {
      this.statementStarts.fill(start, start, end);
      this.statementEnds.fill(end, start, end);
      start = end;
    };
    for (let index = 0; index < size; ++index) {
      const text = this.tokens[index].text;
      const prior = this.tokens[index - 1]?.text;
      const block = text === "{" && (scopes.has(index) || this.owners[index] < 0 ||
        [")", "{", "}", ";", "else", "try", "do", ":"].includes(prior));
      // Match complete operations, even when CPD's span starts after the
      // callee or ends before a differing argument. Parenthesized expressions,
      // for headers, and aggregate initializers stay in their statement.
      if (text === "(" || text === "[" || (text === "{" && !block)) {
        const close = this.pairs.get(index);
        if (close !== undefined && close > index) {
          index = close;
          continue;
        }
      }
      if (text === ";" || text === "}" || block) finish(index + 1);
    }
    finish(size);
  }

  indexFunctions() {
    const tokens = this.tokens;
    let boundary = 0;
    for (let index = 0; index < tokens.length; ++index) {
      const text = tokens[index].text;
      if (text !== "{") {
        if (text === ";" || text === "}") boundary = index + 1;
        continue;
      }
      const end = this.pairs.get(index);
      if (end === undefined) continue;
      for (let header = boundary; header + 1 < index; ++header) {
        if (!["class", "struct", "union"].includes(tokens[header].text)) continue;
        const name = tokens[header + 1].text;
        const after = tokens[header + 2]?.text;
        if (IDENTIFIER.test(name) && ["{", "final", ":"].includes(after)) {
          this.records.push({ name, start: boundary, open: index, end });
          break;
        }
      }
      const parameter = this.findParameters(boundary, index);
      if (parameter) {
          const { name, open, close } = parameter;
          const parameters = parameterDeclarations(tokens, this.pairs, open + 1, close);
          const firstParameter = parameters[0].map((token) => token.text).join(" ");
          let firstStatementEnd = index + 1;
          // An opening control block is not a simple shared setup statement.
          while (firstStatementEnd < end && firstStatementEnd - index <= 128) {
            const current = tokens[firstStatementEnd].text;
            if (current === ";") break;
            if (current === "{") {
              if (["if", "for", "while", "switch", "try", "{"].includes(tokens[index + 1]?.text)) break;
              const close = this.pairs.get(firstStatementEnd);
              if (close === undefined || close - index > 128) break;
              firstStatementEnd = close + 1;
              continue;
            }
            ++firstStatementEnd;
          }
          const firstStatement = tokens[firstStatementEnd]?.text === ";"
            ? tokenText(tokens, index + 1, firstStatementEnd + 1)
            : null;
          const guard = /^(?:const )?std :: (?:scoped_lock|lock_guard|unique_lock|shared_lock)\b/u.test(firstStatement ?? "");
          const setup = firstStatement?.includes("(") &&
            !NON_TYPES.has(tokens[index + 1]?.text) && !guard;
          this.functions.push({
            name, start: boundary, open: index, end, firstStatementEnd,
            parameters, locals: new Set(),
            guardEnd: guard ? firstStatementEnd : null,
            prefix: !setup || firstParameter === "" || firstParameter === "void" ? null : JSON.stringify([
              name,
              firstParameter,
              firstStatement,
            ]),
          });
      }
      boundary = index + 1;
    }
  }

  indexLocals() {
    for (const fn of this.functions) {
      for (const declaration of fn.parameters) {
        const name = declaration.at(-1)?.text;
        if (IDENTIFIER.test(name ?? "") && declaration.at(-2)?.text !== "::" &&
            declaration.filter((token) => IDENTIFIER.test(token.text) && token.text !== "const").length >= 2) {
          fn.locals.add(name);
        }
      }
    }
    for (let index = 1; index + 1 < this.tokens.length; ++index) {
      const owner = this.owners[index];
      if (owner < 0) continue;
      const name = this.tokens[index].text;
      const next = this.tokens[index + 1].text;
      if (!IDENTIFIER.test(name) || !["=", "{", ";", ":"].includes(next)) continue;
      let prior = index - 1;
      while (prior >= 0 && ["*", "&", "&&", "const"].includes(this.tokens[prior].text)) --prior;
      const type = this.tokens[prior]?.text;
      if ((IDENTIFIER.test(type ?? "") && !NON_TYPES.has(type)) || type === ">" || type === ">>") {
        this.functions[owner].locals.add(name);
      }
    }
  }

  findParameters(start, end) {
    for (let index = start; index < end; ++index) {
      if (this.tokens[index].text !== "(") continue;
      const close = this.pairs.get(index);
      if (close === undefined || close >= end) continue;
      let name = this.tokens[index - 1]?.text;
      if (name === ")" && this.tokens[index - 3]?.text === "operator") name = "operator()";
      if (name === "]" && this.tokens[index - 3]?.text === "operator") name = "operator[]";
      if (name === "operator" && this.tokens[close + 1]?.text === "(") {
        index = close;
        continue;
      }
      if (this.tokens[index - 2]?.text === "operator") name = `operator${name}`;
      const suffix = close + 1 === end ||
        ["const", "volatile", "&", "&&", "noexcept", "override", "final", "->", "requires", ":", "["].includes(this.tokens[close + 1]?.text);
      if (suffix && ((IDENTIFIER.test(name ?? "") && !CONTROL.has(name) && !CASTS.has(name)) ||
          name?.startsWith("operator"))) {
        return { name, open: index, close };
      }
      index = close;
    }
    return null;
  }

  range(occurrence) {
    const startOffset = (this.lines[occurrence.start - 1] ?? 0) + (occurrence.column ?? 1) - 1;
    const endOffset = occurrence.endColumn === undefined
      ? this.lines[occurrence.end] ?? Infinity
      : (this.lines[occurrence.end - 1] ?? 0) + occurrence.endColumn;
    return [
      lowerBound(this.tokens, startOffset, (token) => token.offset),
      lowerBound(this.tokens, endOffset, (token) => token.offset),
    ];
  }

  occurrence(path, start, end) {
    const first = this.tokens[start];
    const last = this.tokens[end - 1];
    const startLine = lowerBound(this.lines, first.offset + 1);
    const endLine = lowerBound(this.lines, last.offset + 1);
    return {
      path, start: startLine, end: endLine,
      column: first.offset - this.lines[startLine - 1] + 1,
      endColumn: last.end - this.lines[endLine - 1],
    };
  }

  describe(occurrence) {
    const [rawStart, rawEnd] = this.range(occurrence);
    const start = this.statementStarts[rawStart] ?? rawStart;
    const end = this.statementEnds[rawEnd - 1] ?? rawEnd;
    const prefixes = [];
    const calls = [];
    const assertions = [];
    const declarations = [];
    const records = new Set();
    const localNames = new Map();
    const executable = [];
    const namedExecutable = [];
    let declarationStart = start > 0 && ![";", "{", "}"].includes(this.tokens[start - 1].text)
      ? null : start;
    let executableTokens = 0;
    const seenFunctions = new Set();
    const statementsByFunction = new Map();
    let maxStatements = 0;
    for (let index = start; index < end; ++index) {
        const owner = this.owners[index];
        if (owner < 0) {
          const text = this.tokens[index].text;
          if (text === ";" && declarationStart !== null) {
            const fact = this.tokens.slice(declarationStart, index);
            if (fact.filter((token) => IDENTIFIER.test(token.text)).length >= 2 &&
                !fact.some((token) => ["(", ")", "public", "private", "protected", "using", "namespace", "import", "export", "typedef"].includes(token.text))) {
              declarations.push(tokenText(this.tokens, declarationStart, index + 1));
              if (this.recordOwners[index] >= 0) records.add(this.recordOwners[index]);
            }
          }
          if (text === ";" || text === "{" || text === "}") declarationStart = index + 1;
          continue;
        }
        declarationStart = null;
        const fn = this.functions[owner];
        if (index === fn.open) continue;
        if (!seenFunctions.has(owner)) {
          seenFunctions.add(owner);
          if (fn.prefix !== null && fn.firstStatementEnd >= rawStart && fn.firstStatementEnd < rawEnd &&
              fn.firstStatementEnd + 1 < fn.end) {
            prefixes.push({
              key: fn.prefix, start: fn.start, end: fn.end,
              occurrence: this.occurrence(occurrence.path, fn.start, fn.firstStatementEnd + 1),
            });
          }
        }
        const text = this.tokens[index].text;
        ++executableTokens;
        namedExecutable.push(text);
        const qualified = [".", "->", "::"].includes(this.tokens[index - 1]?.text) ||
          this.tokens[index + 1]?.text === "::";
        if (fn.locals.has(text) && !qualified) {
          const identity = `${owner}:${text}`;
          if (!localNames.has(identity)) localNames.set(identity, `$${localNames.size}`);
          executable.push(localNames.get(identity));
        } else {
          executable.push(text);
        }
        if (text === ";" && this.statementEnds[index] === index + 1 &&
            (fn.guardEnd === null || index > fn.guardEnd)) {
          const count = (statementsByFunction.get(owner) ?? 0) + 1;
          statementsByFunction.set(owner, count);
          maxStatements = Math.max(maxStatements, count);
        }
        if (!IDENTIFIER.test(text) || this.tokens[index + 1]?.text !== "(" || CONTROL.has(text)) continue;
        const close = this.pairs.get(index + 1);
        if (ASSERTIONS.test(text)) {
          if (close !== undefined && close < end) {
            assertions.push(tokenText(this.tokens, index, close + 1));
          }
        } else {
          let name = text;
          if (this.tokens[index - 1]?.text === "::") {
            let prior = index - 2;
            while (prior >= start && IDENTIFIER.test(this.tokens[prior].text)) {
              name = `${this.tokens[prior].text}::${name}`;
              if (this.tokens[prior - 1]?.text !== "::") break;
              prior -= 2;
            }
          }
          if (name !== "std::move" && name !== "std::forward") calls.push(name);
        }
    }
    // Independent shape/storage reads can share syntax while naming entirely
    // different dimensions or roles. Keep those local names; useful arithmetic
    // and subsequent operations still use normal local-variable overlays.
    const independentReads = calls.length > 0 && calls.every((name) => ["size", "empty", "data"].includes(name)) &&
      !executable.some((text) => ["if", "for", "while", "switch", "return", "throw", "+", "-", "/", "%", "*"].includes(text));
    // Arithmetic over the same method API may use either a local value or an
    // owner's member as input. Overlay complete receiver paths while keeping
    // operation names, constants, ordinary member facts, and alias relations.
    const arithmetic = assertions.length === 0 &&
      executable.some((text) => ["+", "-", "*", "/", "%"].includes(text));
    const operations = independentReads ? namedExecutable
      : arithmetic ? algorithmReceivers(executable) : executable;
    const record = declarations.length >= 2 && records.size === 1
      ? this.records[[...records][0]] : null;
    const key = maxStatements >= 2
      ? calls.length !== 0
        ? JSON.stringify(["operations", operations])
        : assertions.length >= 2
          ? JSON.stringify(["assertions", executable])
          : executable.some((text) => ["if", "for", "while", "switch", "return", "throw", "+", "-", "/", "%"].includes(text))
            ? JSON.stringify(["statements", executable])
            : null
      : record !== null
        ? this.recordKey(record)
        : null;
    const identity = JSON.stringify([occurrence.path,
      maxStatements < 2 && record !== null ? record.start : start,
      maxStatements < 2 && record !== null ? record.end : end]);
    return { prefixes, key, identity, statements: maxStatements, executableTokens, balanced: this.balanced };
  }

  recordKey(record) {
    record.key ??= JSON.stringify(["record", this.tokens.slice(record.start, record.end)
      .map((token) => token.text === record.name ? "$self" : token.text)]);
    return record.key;
  }
}

function groupOccurrences(entries, keyFor, identityFor) {
  const groups = new Map();
  for (const entry of entries) {
    const key = keyFor(entry);
    if (key === null) continue;
    const group = groups.get(key) ?? new Map();
    group.set(identityFor(entry), entry);
    groups.set(key, group);
  }
  return [...groups.values()].filter((group) => group.size >= 2).map((group) => [...group.values()]);
}

export function filterCpdCandidates(candidates, {
  sourceReader = (path) => readFileSync(path, "utf8"),
} = {}) {
  const duplications = [];
  const unfiltered = [];
  for (const candidate of candidates) {
    (candidate.tokenCount > MAX_CPD_FILTER_TOKENS ? unfiltered : duplications).push(candidate);
  }
  const byPath = new Map();
  const described = duplications.map((duplication) => duplication.occurrences.map((occurrence) => {
    const entry = { occurrence };
    const entries = byPath.get(occurrence.path) ?? [];
    entries.push(entry);
    byPath.set(occurrence.path, entries);
    return entry;
  }));
  // Retain only one file's token index at a time. Analysis cost is source size
  // plus the reported occurrence spans, not the square of the function count.
  for (const [path, entries] of byPath) {
    const index = new SourceIndex(sourceReader(path));
    for (const entry of entries) entry.description = index.describe(entry.occurrence);
  }
  const retained = [];
  const filtered = [];
  const reasons = {};
  for (let index = 0; index < duplications.length; ++index) {
    const duplication = duplications[index];
    const entries = described[index];
    const accepted = new Set();
    const keep = (group, pattern, identities, patternKey) => {
      for (const entry of group) accepted.add(entry.original ?? entry);
      retained.push({
        ...duplication, occurrences: group.map((entry) => entry.occurrence),
        patterns: [pattern], targetIdentities: identities, patternKey,
      });
    };
    for (const group of groupOccurrences(entries, (entry) => entry.description.key,
      (entry) => entry.description.identity)) {
      keep(group, group[0].description.statements >= 2
        ? "shared executable operations" : "same complete record declaration",
      group.map((entry) => entry.description.identity), group[0].description.key);
    }
    const prefixes = new Map();
    for (const entry of entries) {
      for (const prefix of entry.description.prefixes) {
        const group = prefixes.get(prefix.key) ?? new Map();
        group.set(JSON.stringify([entry.occurrence.path, prefix.start, prefix.end]),
          { ...entry, original: entry, occurrence: prefix.occurrence });
        prefixes.set(prefix.key, group);
      }
    }
    for (const [key, group] of prefixes) {
      if (group.size < 2) continue;
      keep([...group.values()], "same function, first parameter and opening statement",
        [...group.keys()], `prefix:${key}`);
    }
    // Conditional preprocessing and unsupported syntax must not silently lose
    // executable candidates merely because this lightweight index is uncertain.
    if (entries.some((entry) => !entry.description.balanced)) {
      keep(entries, "unclassified syntax requires manual review",
        entries.map((entry) => JSON.stringify(entry.occurrence)), `unclassified:${index}`);
    }
    if (accepted.size >= 2) {
      const unmatched = entries.filter((entry) => !accepted.has(entry));
      if (unmatched.length !== 0) {
        filtered.push({
          ...duplication,
          occurrences: unmatched.map((entry) => entry.occurrence),
          reason: "occurrences without the retained group's operation or declaration identity",
        });
      }
    } else {
      const reason = new Set(entries.map((entry) => entry.description.identity)).size < 2
        ? "one containing statement or declaration reported more than once"
        : entries.every((entry) => entry.description.executableTokens === 0)
          ? "declaration or signature surface"
          : entries.every((entry) => entry.description.statements < 2)
            ? "single operation or partial statement"
            : "different complete operations, types, constants or assertion facts";
      reasons[reason] = (reasons[reason] ?? 0) + 1;
      filtered.push({ ...duplication, reason });
    }
  }
  const targets = new Map();
  for (const { targetIdentities, patternKey: key, ...duplication } of retained) {
    const occurrences = new Map(targetIdentities.map((identity, index) => [identity, duplication.occurrences[index]]));
    const prior = targets.get(key);
    if (prior === undefined) {
      targets.set(key, { duplication, occurrences });
    } else {
      for (const [identity, occurrence] of occurrences) {
        if (!prior.occurrences.has(identity) || duplication.tokenCount > prior.duplication.tokenCount) {
          prior.occurrences.set(identity, occurrence);
        }
      }
      prior.duplication.tokenCount = Math.max(prior.duplication.tokenCount, duplication.tokenCount);
      prior.duplication.lineCount = Math.max(prior.duplication.lineCount, duplication.lineCount);
      filtered.push({ ...duplication, reason: "same operation pattern already represented by a consolidated match" });
    }
  }
  return {
    duplications: [
      ...unfiltered,
      ...[...targets.values()].map(({ duplication, occurrences }) =>
        ({ ...duplication, occurrences: [...occurrences.values()] })),
    ],
    filtered, reasons, indexedFiles: byPath.size,
  };
}
