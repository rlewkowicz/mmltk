# AGENTS.md

## Navigation

Start with [CONTRACT.md](CONTRACT.md) for architecture and the
[technical wiki](docs/README.md) for source ownership and reference material.
Use [commands](docs/commands.md), [validation](docs/validation.md), and
[logging](docs/logging.md) for wrapper operations and evidence. GUI work also
uses [interaction and presentation](docs/gui-interaction.md).
[Build reference](docs/build.md) owns generated-artifact paths and cache rules;
[GPU execution](docs/gpu-execution.md) owns capability-inspection commands.
Use [reflected declaration authoring](docs/reflection.md) for canonical schema
syntax and [declaration tooling](docs/validation.md#raw-cpd-and-declaration-formatting)
for raw CPD evidence and safe formatter operations.

## Sub Agents

Use subagents only on explicit user request or while executing a specific
`actionplan.md`. Wait for returns; do not poll. Send completed executor work
directly to its designated reviewer. The main agent reviews remediation plans
for scope and architectural alignment without duplicating implementation audits.
These implementation-phase delegation rules do not apply to validation fixes;
[Final Validation](#final-validation-workflow) defines their main-agent ownership.

After more than three reviewer returns for one phase, the main agent pauses
for a deep implementation audit of `CONTRACT.md`, phase requirements, and
affected code. Seek unnecessary complexity, incohesive boundaries, duplicated
vocabulary, and C++26 reflection opportunities; consolidate the recent
direction into one coherent implementation and complete the phase.

## Optimization

Minimize loops, CPU/GPU transfer, memory churn, blocking, and unnecessary
allocation; maximize useful parallelism. Prefer pinned application memory
where appropriate and O(1) algorithms where practical. Use compact, well-defined
types and tightly task-sized buffers; allocate long-lived buffers once and
reuse aggressively.

## Platform

This is a Linux-only data system. Optimize for Linux and spend no effort on
macOS or Windows compatibility.

## third_party

`third_party` may be modified as part of this codebase. Keep application logic
in the application layer.

Preserve vendored build/language policies unless explicitly changed by the task;
the core toolchain policy does not migrate `third_party`. Firefox retains its
cached Clang toolchain and bootstrap sysroot.

## Containerization

Build and run everything via containers. Except cleanup, do not execute local
toolchains. Do not invoke Docker directly or assume host-relative paths. Route
every build, test, sanitizer, debugger, profiler, and containerized diagnostic
through `./mmltk`; add and use missing wrapper capabilities.

## Documentation

During `actionplan.md` execution, after Final Validation completes and all
preceding changes are committed, spawn exactly one fresh astra max subagent to
update the complete documentation set against current code. It verifies commands,
paths, formats, behavior, ownership, and cross-links against authoritative source
and tooling, edits documentation only, and reports results. The main agent
reviews the diff for accuracy, scope, and organization, then commits it. This
pass does not reopen implementation review, cleanup, builds, tests, or acceptance.

Keep each document within its audience and purpose:

- `README.md` is the user-facing introduction: entry points, quick starts,
  repository narrative, caveats, developer commentary, and 1000-foot
  overviews. Do not modify anything above its first `## Build` heading.
  Preserve that region verbatim, including spelling, formatting, links, and
  technical claims; this also applies to documentation passes. From `## Build`
  onward, preserve the developer's voice, humor, and interjections while
  correcting spelling and formatting. Move detailed reference and architecture
  material from that editable region into `docs/`.
- `CONTRACT.md` owns high-level technical architecture: major application
  ownership, cross-repository flow, broad constraints, and critical component
  handoffs. Exclude implementation symbols, call sequences, and detailed code
  mechanics.
- `AGENTS.md` is a terse agent navigation and policy guide covering critical
  tooling, logging, optimization, repository organization, code direction,
  workflow rules, and gotchas. Describe repository, tool, and command outcomes;
  exclude system-specific implementation outcomes and architecture.
- `docs/` is the technical wiki, indexed by `docs/README.md`. Maintain that index
  and group pages by reader task: getting started and reference; architecture
  and frameworks; data and backend systems; engineering processes, validation,
  and operations. Put detailed commands, formats, code explanations, diagrams,
  and procedures here. Give each fact one authoritative home; link instead of
  duplicating it.

The documentation agent chooses and evolves the smallest coherent hierarchy
for the material, updates stale facts, names, links, and examples, adds missing
index entries, and removes obsolete duplication while preserving nuance.
Do not manufacture architecture or infer behavior from old prose when source,
generated artifacts, or wrapper help can establish it.

## Building

First-party C++ uses GCC 16.2 and C++26 reflection. CUDA uses NVCC 13.4,
CUDA C++23, and GCC 16.2 as its host compiler with the unsupported-host
override. GCC 14 only bootstraps GCC 16.2. Explicit ONNX, simdjson, and cppcheck
source builds use GCC 16.2 while retaining their dependency language policies.

Development and runtime own independent Ubuntu 24.04 base stages.
`docker/nvidia-payload.json` is authoritative for both branches' selected
pinned-NGC binary payload, normalized locations, dependencies, and notices.

During action-plan execution, defer full product builds and tests to Final
Validation. Only narrowly targeted generation needed to materialize or inspect
cross-language artifacts may build earlier through `./mmltk`. Run product
builds with `./mmltk --build`. Generated Rust stays in the build directory;
never hand-edit it. Fix canonical reflected declarations, generation, or build
wiring and regenerate through `./mmltk`.

## Crashes, Logging, and Debugging

Logging help: `./mmltk --logs --help`.
Explicit build, test, and tidy invocations retain
[wrapper transcripts](docs/logging.md#wrapper-build-test-and-tidy-transcripts).

Provide granular opt-in JSONL logging with maximum useful troubleshooting
detail. Disabled logging must not collect or format diagnostics. Keep normal
execution quiet; always report crashes and fatal operation or process failures
on stderr with a concise useful error, even with diagnostics disabled. Include
the failing component and available error or exit status without enabling
tracing, probes, or routine logging. Fatal signal reporting must be
async-signal-safe; avoid duplicate reports. Gate nonfatal diagnostic prints and
logging that affect normal runtime. Prove bugs through tests or gated logging
before changing behavior; crash-dump symbols alone do not justify fixes.

During Final Validation, if the same distinct test still fails after four
evidence-driven fix and validation attempts, the main agent deeply diagnoses the
exact failure, requirements, complete current diff, attempted directions, and
focused `./mmltk` evidence. Inspect ordinary API behavior, cohesive system
boundaries, RAII resource safety, concurrency, and failure propagation before
further edits. Repeated failures authorize no validation executor, diagnostic
subagent, or additional reviewer.

Use the log query tool before raw-log searches. A raw search needed to diagnose
or prove an issue is a tooling gap: outside Final Validation, assign it to an
astra max subagent or update the existing logging-tool agent; during Final
Validation, the main agent fixes it directly. The query tool should efficiently
highlight matches, associated identities and frames, and end-to-end paths in as
few queries as practical.

## actionplan.md

`actionplan.md` is an executable phased plan for another agent. Confirm files and
line numbers first. Include a problem statement or goal, Summary, Scope,
Architecture, concrete phases, important files, every expected changed file,
exact actions/locations, risks, and post-cleanup concerns. Exclude user-facing
notes, answers, commentary, reminders, repeated `AGENTS.md` constraints, vague
directives (“explore this or that”), and conversation context inaccessible to
the next agent.

### Writing an action plan

- Do not add a Commit section or phase; the session-level commit rule applies.
- Arrange dependent work to avoid back-and-forth edits. Compilation occurs in
  Final Validation, so intermediate states need not compile.
- In plan mode, save the final plan as `actionplan.md` before execution.

### Executing an action plan

- Phase order guides sequencing; overlap or pull work forward for a complete,
  architecturally aligned cutover. Review the implementation against all
  applicable requirements.
- Unless a phase explicitly requests a product-logic change, preserve every
  observable outcome, failure path, and integration behavior across
  interface cutovers. Before deleting or replacing a substantial implementation,
  trace its behavior through callers, callees, persisted forms, and tests;
  reuse cohesive behavior where possible and retain all required outcomes.
- For each implementation phase's first pass, spawn exactly one new astra xhigh
  executor. Use a low subagent for followup remediation within implementation
  phases; the main agent handles small evidence-driven follow-up fixes and
  delegates large missing implementation. Final Validation uses its separate
  main-agent workflow regardless of fix size.
- Update `actionplan.md` atomically after each phase: remove completed phases
  entirely, leaving actionable steps; never defer updates. Preserve
  Summary, Scope, Architecture, and scope clarification. Otherwise edit only
  the active phase and materially affected later steps. Record pertinent detours
  while actionable. Replace every surviving reference to removed phases
  (dependencies, actions, risks, handoffs) with concrete implemented APIs, owners,
  artifacts, or invariants, never removed phase numbers, titles, or prose.
- After each implementation phase's first pass, spawn exactly one new sol xhigh
  reviewer with the relevant complete diff. Wait for and use its report before
  updating the executor; reuse that reviewer for follow-ups. Closure requires
  `COMPLETE`.
- Phase reviewers compare `actionplan.md`, `CONTRACT.md`, and `AGENTS.md` for
  end-to-end continuity, ambiguity, contradictions, vocabulary drift, missing
  handoffs, and incompatible requirements.
- Reviewers block unrequested behavior loss. For substantial deletions or
  interface replacements, reconstruct behavior from the complete prior
  implementation, callers, persisted forms, and tests. Verify every outcome is
  retained or explicitly superseded by requirements; prefer cohesive logic reuse
  over deletion and partial reimplementation.
- Reviews of Rust/Iced work verify component ownership and interaction rules
  against `CONTRACT.md`, relevant documentation, and source.
- The reviewer must not edit `actionplan.md`. Staging files is optional. Fix
  medium through critical blockers that impede the required application
  behavior; optional nice-to-haves do not block the phase.
- Make complete interface cutovers; preserve backward compatibility only when
  explicitly required, without incremental compatibility layers.
- Review human-scale developer boundaries and runtime behavior. A product
  capability should extend one cohesive system or local component through
  canonical reflected types, without mirrored vocabulary, monolithic owners,
  or passthrough coordination layers.
- After `COMPLETE` and removal of the phase, commit all outstanding tracked
  changes except `actionplan.md`, which must remain uncommitted, never force-added.

For recurring similar findings, stop symptom patches. Re-read the complete
system API, reflected vocabulary, concurrency, lifetime, failure, and integration
paths; require one cohesive correction before phase closure.

### Post-main-phase framework audit

Run one framework audit after each integer-numbered main implementation commit,
before the next main phase (`Phase 2` is main; `Phase 2.1` an audit subphase).
Do not audit subphase commits, Final Validation, or its checkpoints.

Count changed files. Spawn exactly one fresh astra max agent without inherited
context; supply the commit, main phase number/title, repository path, and prompt
below. It independently reads the authorities and complete commit, may edit only
`actionplan.md`, and must not build, test, or commit.

Follow the prompt for subphase insertion and later-phase changes. The main agent
checks scope and architectural alignment, then executes inserted subphases
through the ordinary executor/review/plan-update/commit workflow. After all
subphases finish, inspect every later phase against the implemented framework
and update materially affected dependencies, anchors, files, actions, and
handoffs before advancing.

Use this prompt verbatim, replacing `<MAIN PHASE>`, `<PHASE TITLE>`,
`<PHASE COMMIT>`, and `<REPOSITORY>` with concrete values:

> Work as a fresh standalone architecture and action-plan reviewer in
> `<REPOSITORY>`. Do not rely on or request prior conversation context. Read
> `AGENTS.md`, `CONTRACT.md`, and `actionplan.md` completely before acting.
> Audit the complete diff and changed-file set of commit `<PHASE COMMIT>`, which
> completed main Phase `<MAIN PHASE>` — `<PHASE TITLE>`. Inspect every changed
> artifact and relevant unchanged caller, callee, dependency, configuration,
> build rule, generated boundary, test, and persisted form. Confirm and report
> the exact changed-file count.
>
> Find repeated concepts, cross-file change patterns, misplaced ownership,
> hidden coupling, duplicated vocabulary, and boundaries whose blast radius is
> larger than their product responsibility. Treat each affected C++ boundary as
> if it needed C++20-module-quality dependency hygiene, but do not introduce
> modules. Inspect the include, link, schema, generation, and concept
> dependencies as a directed acyclic graph. Identify cycles, reverse
> dependencies, transitive-include reliance, include-order requirements,
> incomplete ordinary headers, implementation leakage, and declarations that
> would not survive isolated compilation.
>
> Propose focused ordinary classes, sealed owners, factories, helpers, reusable
> templates, or canonical C++26-reflected declarations only where they
> consolidate a real shared concept, repeated behavior, or authoritative fact.
> Prefer compile-time structural projection with no runtime lookup or storage
> cost. Keep resource ownership, control flow, and product policy in ordinary
> systems with direct APIs. Reduce future cross-file edits and make ownership
> boundaries enforceable. Do not add facades, passthrough coordinators,
> one-method wrappers, speculative abstractions, parallel registries,
> reflection over resource or execution state, preprocessor-heavy mirrored
> schemas, hidden control flow, or indirection without a measurable
> deduplication, ownership, safety, or blast-radius benefit.
>
> Audit the C++ to generated-Rust boundary for one canonical native vocabulary,
> exhaustive reflected projection and dispatch, stable field identity, schema
> validation, and removal of handwritten member-wise or string-keyed mirrors.
> Keep genuinely visual copy, component state, layout, styling, theme,
> navigation, and interaction behavior owned by Rust and Iced. Treat
> observability, tracing, logging, and correlation as effect-only diagnostics
> unless the audited phase demonstrates a product requirement. Diagnostic
> identities never become ordering, cache-validity, acknowledgement, or
> resource-lifetime state.
>
> Preserve every observable behavior, failure path, integration outcome,
> persisted format, resource-lifetime guarantee, concurrency property, test
> case, widget identity, styling fact, and user-facing function in the audited
> commit. Reconstruct substantial moved, deleted, or replaced behavior through
> its callers, callees, tests, and persisted forms. Reject any simplification
> that silently loses capability.
>
> Edit only `actionplan.md`. Preserve its Goal and Summary, Scope, and
> Architecture verbatim. If the audit demonstrates valuable framework work,
> insert the minimum cohesive executable subphases numbered `<MAIN PHASE>.1`,
> `<MAIN PHASE>.2`, and so on before the next main phase. For each subphase give
> concrete goals, confirmed paths and line anchors, exact actions and
> locations, dependencies, required cases and evidence, risks and handoffs,
> and every expected file. Arrange complete cutovers without compatibility
> layers. Update later phases only where the proposed work materially changes
> their dependencies, anchors, files, actions, or handoffs. If no meaningful
> subphase survives this audit, do not manufacture one; leave the plan
> unchanged and explain why.
>
> Re-read the complete modified plan for internal consistency. Return the
> changed-file count, a concise dependency and ownership assessment, every
> inserted or updated phase, rejected abstractions with reasons, and any
> demonstrated concern that remains represented in the plan.

### Final Validation workflow

The main agent owns all validation commands, diagnosis, and edits, including
application code, tests, build wiring, diagnostics, tooling, cleanup, and tidy.
Regardless of fix size or failed attempts, do not delegate validation
implementation or troubleshooting, or turn fixes into implementation phases or
detours invoking executor/reviewer cycles. Validation fixes receive no per-fix,
phase, cleanup, or framework review. The main agent edits and validates directly
throughout Final Validation without starting or reopening reviews.

Final Validation ends when the required final build, tests, and acceptance pass.
Commit all remaining tracked changes, then complete and commit the documentation
pass. Do not add a full-plan or whole-plan review, post-validation audit, or
another validation stage; documentation does not restart validation.

Run these stages in order. Cleanup must follow successful initial tidy/build
and never follow the final build; tests require a successful final build.
Both builds use `./mmltk --build`; both tidy passes use the complete configured
`./mmltk --tidy` suite regardless of changed files or commits.

1. Run full tidy and resolve every genuine finding, then the initial full build.
   Immediately after both succeed, commit every outstanding tracked change with
   the exact message `pre cleanup`; keep `actionplan.md` uncommitted.
2. Obtain a fresh full report from every applicable cleanup profile regardless
   of earlier commits or changed files; never reuse earlier reports. Resolve
   every genuine hit, then run a final full pass of every profile. Once all are
   clean, run the second full tidy and resolve every genuine finding. If tidy
   remediation changes code, repeat cleanup then tidy before continuing.
3. After cleanup and tidy are clean, commit all outstanding tracked cleanup and
   tidy changes with the exact message `post cleanup`; keep `actionplan.md`
   uncommitted.
4. From `post cleanup`, run the final full build once, then the complete `all`
   suite and Wayland acceptance, exactly `./mmltk --test all` followed by
   `./mmltk --test workspace-wayland --headless-compositor`, with no intervening
   cleanup/tidy. Both must pass. Do not replace or supplement this gate with
   focused tests, individual executables, additional suites, or plan-specific
   acceptance commands. Put required feature coverage in these existing suites;
   diagnostic investigation adds no acceptance gate.
5. If validation fails after `post cleanup`, the main agent diagnoses and edits
   the cause, rebuilds affected product code with `./mmltk --build`, and reruns
   both required test commands in order. Do not add review or delegation or
   repeat cleanup/tidy for these fixes.

Group targeted cleanup and tidy follow-up paths in one invocation where
practical, preserving stage order. Proven detector false positives may use the
narrow inline suppressions [below](#deduplication-rules).

## remediationplan.md

`remediationplan.md` is the reviewer's executable phased plan for the executor.
After main-agent scope/architecture review, the same astra xhigh executor
completes and updates it until no phases remain; only then resume review.

Include a problem statement or goal, Summary, Scope, Architecture, authoritative
review set, findings, concrete phases, important files, every expected changed
file, exact actions/locations, architectural constraints, and post-cleanup
concerns. Exclude user-facing notes and repeated `AGENTS.md` constraints.

### Writing a remediation plan

- Do not add validation or commit phases; those remain in `actionplan.md` and
  the main-agent workflow.
- Arrange dependent work to avoid back-and-forth edits. Compilation occurs in
  Final Validation.

### Executing a remediation plan

- Work may be pulled forward when it creates a complete, cohesive cutover.
- The executing owner atomically removes each completed phase from
  `remediationplan.md`, leaving only actionable steps. Change only that phase;
  preserve Summary, Scope, Architecture, and scope clarification. Never defer
  updates or force-commit `remediationplan.md`.
- Apply the action-plan behavior-preservation rule before deleting or replacing
  substantial code, including code outside the immediate finding location.
- Apply the action-plan complete-cutover and compatibility rules.

## Code Churn

Accept churn for performance, cohesive design, class/template structure,
modularity, reuse, and reduced blast radius. Remove legacy code; consolidate
repetition in appropriate shared helpers, classes, functions, templates,
factories, or reflected declarations. Use C++26 reflection for canonical schema
derivation and exhaustive dispatch.

## Code Deduplication

Remove all genuine duplicated code in scope, including cross-file duplication,
by consolidating repeated behavior or data into one reusable implementation or
canonical reflected declaration.
Do not bypass cleanup by installing system dependencies or evade either
detector through line compression, respelling, one-method wrappers, or
pass-through indirection.

For facilities applying broadly across types, derive repeated member-wise
machinery through C++26 static reflection from one canonical declaration.
Member additions/removals should propagate without a second handwritten inventory.

Prefer compile-time reflection/generation without runtime lookup or storage.
Dynamic reflection, macro registries, and runtime type tables require product
runtime-discovery needs. Reflection owns only reusable structural projection;
ordinary classes retain clear public contracts, private state, sealed ownership,
and direct functions.

Prefer deleting unused variants over abstracting them. Before helper extraction,
search callers for an existing registry, reflected dispatch surface, factory,
ordinary system, or utility expressing the concept. Before deleting/replacing
artifacts, search the entire repository and migrate every caller in one cutover.

Cleanup affecting handwritten Rust runs both the C++ and frontend profiles.
Resolve Rust duplication in the component owning the behavior or data. See
[validation](docs/validation.md#deduplication-reports) for cleanup commands and
report formats.

### Deduplication rules

- Replace string-keyed dispatch with the canonical typed enum or reflected
  intent. Do not consolidate comparisons into a parallel string table.
- Derive wire, event, and client payload projections from canonical reflected
  native types. Perform one conversion for a genuinely distinct external
  format.
- Reflection-bearing declarations and ordinary systems belong in
  system-colocated self-contained ordinary headers with ordinary sources for
  runtime definitions. Ordinary headers include their complete dependencies
  and contain no module declaration or `import`.
- A target may register one declaration-free, import-free, acyclic ordinary
  surface header for a cohesive family used by at least three independent
  consumers. Narrow consumers include direct declaration headers.
- Materialize reflection through the ordinary header containing the canonical
  type or schema. Publish ordinary types, functions, constants, or genuinely
  reusable template specializations so consumers do not re-reflect types or
  depend on include order.
- Retain a named module for a stable, isolated, cohesive non-reflection boundary
  whose declarations have no ordinary-header duplicate. Delete pure aggregators
  and replaced module routes.
- CMake source registration defines target membership, source and header sets,
  retained modules, includes, compile requirements, and links. Avoid parallel
  declaration inventories and scanners.
- When cleanup changes a module interface, partition, import, exported template,
  or shared test fixture, validate the affected module and directly changed
  importers together.
- Inline suppressions apply only to inspected false positives. Use one
  `// CLEANUP-IGNORE: <reason>` for a single occurrence or the narrowest
  `// CLEANUP-OFF: <reason>` through `// CLEANUP-ON` range. Suppression
  registries, baselines, separate files, file-wide ranges, and macro-wide
  markers are invalid. Never suppress genuine duplication in either profile.

## Git

Unless the user explicitly overrides, commit only during `actionplan.md`
execution: after each reviewed implementation phase, at the required cleanup
checkpoints, after Final Validation, and after the documentation pass. Include
all remaining tracked changes, including unrelated tracked changes, at those
commits. Leave non-plan work uncommitted. Never add or commit `actionplan.md`
or `remediationplan.md`.

## Tests

Do not add regression tests. You may add standard tests that exercise required
behavior, boundaries, failure handling, resource safety, and integration.
Final test selection is fixed by [Final Validation](#final-validation-workflow).
A new `--test all`, including filtered invocations, replaces earlier test runs
owned by this checkout; [focused suite names preserve them](docs/validation.md#replacing-an-active-test-run).

Do not add tests to Firefox's test suites or execute those suites. Test Firefox
only indirectly through this repository's first-party suites and packaged
application acceptance.

For hardware tests, correct an evidence-identified cause narrowly.
Roll back only if the targeted failure persists or the test stays stuck at the
same point. If it advances to a distinct blocker, retain the correction and log
every boundary needed to diagnose that blocker. Base subsequent changes on
proven facts/captured logs, never guesses or unrelated later timeouts.

## Display

The application is Wayland-only. Do not use CPU display or X11 fallbacks.

## Web GUI

The GUI uses Iced Rust. Building's action-plan timing applies to GUI work.
Outside plans, use `./mmltk --build`, then test modified behavior.

Read `CONTRACT.md`, relevant GUI documentation, and owning components before
editing. Native domain facts belong in canonical C++26 declarations projected
into generated typed Rust; handwritten Rust/Iced owns presentation and
interaction. Do not duplicate native facts. Validate rendered behavior against
requirements; constructing UI code alone is insufficient acceptance evidence.

## Polling and Events

Prefer event-driven execution, callbacks, stop tokens, and blocking waits local
to system-owned workers. Avoid heavy mutexing, polling, arbitrary delays, and
unnecessary blocking.

## Tidy

All validation follows [Final Validation](#final-validation-workflow)'s stage
order, commands, and completion rules.

The full suite formats first-party C/C++/CUDA and the Iced application Rust
package, runs clang-tidy on supported native translation units, and validates
reflection units through GCC compiler objects. Targeted native `--file` runs
remain scoped. Cppcheck is gated off because its parser lacks C++26 reflection
support.

For follow-ups, audit the entire prior tidy log, group failures by root cause,
fix the whole known batch including blocked downstream importers, and rerun
flagged files together where practical. Do not run `--third_party`.

## Governing Contract and System Architecture

- Keep this file within its [documentation scope](#documentation). Put product
  architecture in `CONTRACT.md`, documentation, and source. Omit code-specific
  names/examples except exact operational prompts and log references.
- `CONTRACT.md` states only positive desired architecture/product outcomes;
  historical negatives and migration details belong in plans.
- Keep the contract concise, using diagrams and a current ownership matrix at
  system boundaries. Cover broad architectural, browser, UI, GPU, performance,
  failure, and shutdown outcomes; exclude per-system algorithms, method and
  protocol-record inventories, library choices, fixed private budgets,
  temporary migration state, test fixtures, callback plumbing, diagnostic
  layouts, and class-private state.
- `CONTRACT.md` is authoritative; implementation/plans converge toward it.
  Change it only for user-intended architecture/product outcome changes.
- Consult the contract, relevant documentation, and owning source for product
  structure and integration boundaries.

@CONTRACT.md
