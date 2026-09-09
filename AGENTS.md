# AGENTS.md

## Sub Agents

Use subagents only on explicit user request or while executing a specific
`actionplan.md`. Wait for returns; do not poll. Send completed executor work
directly to its designated reviewer. The main agent reviews remediation plans
for scope and architectural alignment without duplicating implementation audits.

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
audit and update the complete documentation set against the current codebase.
It verifies commands, paths, formats, behavior, ownership, and cross-links from
authoritative source and tooling, edits documentation only, and reports the
result. The main agent reviews the documentation diff for accuracy, scope, and
organization, then commits it.

Keep each document within its audience and purpose:

- `README.md` is the user-facing introduction: entry points, quick starts,
  repository narrative, caveats, developer commentary, and 1000-foot
  overviews. Preserve the developer's voice, humor, and interjections while
  correcting spelling and formatting. Move detailed reference and architecture
  material into `docs/`.
- `CONTRACT.md` defines high-level technical architecture: major application
  ownership, cross-repository flow, broad constraints, and critical component
  handoffs. Exclude implementation symbols, call sequences, and detailed code
  mechanics.
- `AGENTS.md` is a terse agent navigation and policy guide. Include critical
  tooling, logging, optimization, repository organization, code direction,
  workflow rules, and gotchas. Describe repository, tool, and command outcomes;
  exclude system-specific implementation outcomes and architecture.
- `docs/` is the technical wiki. Maintain `docs/README.md` as its index and
  group pages by reader task: getting started and reference; architecture and
  frameworks; data and backend systems; engineering processes, validation, and
  operations. Put detailed commands, formats, code explanations, diagrams, and
  procedures here. Give each fact one authoritative home and link to it instead
  of duplicating it.

The documentation agent chooses and evolves the smallest coherent hierarchy
for the actual material. It updates stale facts, names, links, and examples;
adds missing index entries; removes obsolete duplication; and preserves
important nuance. It does not manufacture architecture or infer behavior from
old prose when source, generated artifacts, or wrapper help can establish it.

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

Examples:

```bash
  ./mmltk --logs --help
```

Provide granular opt-in JSONL logging with maximum useful troubleshooting
detail. Disabled logging must avoid collecting and formatting diagnostic data.
Gate existing print and logging statements that affect normal runtime. Prove a
bug through tests or gated logging before changing behavior; do not infer fixes
from crash-dump symbols alone.

If the same distinct test still fails after four evidence-driven fix and
validation attempts during action-plan execution, assign one fresh astra max
subagent the exact failure, requirements, complete current diff, attempted
directions, and focused `./mmltk` command. Require a deep review of ordinary API
behavior, cohesive system boundaries, RAII resource safety, concurrency, and
failure propagation.

Use the log query tool before raw-log searches. Needing a raw search to
diagnose or prove an issue is a tooling gap: assign it to an astra max subagent,
or update the existing logging-tool agent. The query tool should efficiently
highlight matches, associated identities and frames, and end-to-end paths in
as few queries as practical.

## actionplan.md

`actionplan.md` is an executable phased plan for another agent. Include a
problem statement or goal, Summary, Scope, Architecture, concrete phases,
important files, exact actions/locations, risks, and post-cleanup concerns.
Confirm files and line numbers first. Exclude user-facing notes, answers,
commentary, reminders, repeated `AGENTS.md` constraints, and vague directives
such as “explore this or that”. Do not reference conversation context that the
next agent cannot access.

### Writing an action plan

- Do not add a Commit section or phase; the session-level commit rule applies.
- Arrange dependent work to avoid back-and-forth edits. Compilation occurs in
  Final Validation, so intermediate states need not compile.
- In plan mode, save the final plan as `actionplan.md` before execution.
- Include every file expected to change.

### Executing an action plan

- Record the starting commit as the immutable whole-plan audit baseline in
  session state, not `actionplan.md`.
- Phase ordering guides sequencing; overlap or pull work forward for a complete,
  architecturally aligned cutover. Review the implemented set against all
  applicable requirements.
- Unless a phase explicitly requests a product-logic change, preserve every
  existing observable outcome, failure path, and integration behavior across
  interface cutovers. Before deleting or replacing a substantial implementation,
  trace what it does through its callers, callees, persisted forms, and tests;
  reuse cohesive behavior where possible and carry all required outcomes into
  the replacement.
- Spawn exactly one new astra medium executor for each implementation phase's
  first pass; reuse it for remediation. Final Validation follows its separate
  main-agent workflow. The main agent handles small evidence-driven follow-up
  fixes; delegate large missing implementation.
- Update `actionplan.md` atomically after each phase, never defer to the end.
  Remove completed phases entirely, leaving actionable steps. Preserve Summary,
  Scope, Architecture, and scope clarification; otherwise change only the active
  phase and materially affected later steps. Record pertinent detours while
  actionable. Replace all surviving references to removed phases (dependencies,
  actions, risks, handoffs) with concrete implemented APIs, owners, artifacts,
  or invariants, never removed phase numbers, titles, or prose.
- After implementation, spawn exactly one new sol xhigh reviewer with the
  relevant complete diff. Wait for and use its report before updating the
  executor; reuse that reviewer for follow-ups. Closure requires `COMPLETE`.
- Phase reviewers compare `actionplan.md`, `CONTRACT.md`, and `AGENTS.md` for
  end-to-end continuity: ambiguity, contradictions, vocabulary drift, missing
  handoffs, and incompatible requirements.
- Reviewers treat unrequested behavior loss as a blocker. For substantial
  deletions or interface replacements, reconstruct the deleted behavior from
  the complete prior implementation, callers, persisted forms, and tests, then
  verify that every outcome is retained or is explicitly superseded by the
  requirements. Prefer reuse of cohesive existing logic over deletion followed
  by a partial reimplementation.
- Reviews of Rust/Iced work verify component ownership and interaction rules
  against `CONTRACT.md`, relevant documentation, and source.
- The reviewer must not edit `actionplan.md`. Staging files is optional. Fix
  medium through critical blockers that impede the required application
  behavior; optional nice-to-haves do not block the phase.
- Treat interface changes as complete cutovers. Preserve backward
  compatibility only when explicitly required. Do not add incremental
  compatibility layers.
- Reviews assess human-scale developer boundaries as well as runtime behavior.
  Adding a product capability should extend one cohesive system or local
  component through canonical reflected types, without mirrored vocabulary,
  monolithic owners, or passthrough coordination layers.
- After `COMPLETE` and removal of the phase, commit all outstanding tracked
  changes except `actionplan.md`, which must remain uncommitted, never force-added.

### Post-main-phase framework audit

Run one framework audit after each integer-numbered main implementation commit,
before the next main phase (`Phase 2` is main; `Phase 2.1` is an audit subphase).
Do not audit subphase commits, Final Validation, or its checkpoints.

Count changed files. Spawn exactly one fresh astra max agent without inherited
context; supply the commit, main phase number/title, repository path, and prompt
below. The agent independently reads the authorities and complete commit, may
edit only `actionplan.md`, and must not build, test, or commit.

The prompt governs subphase insertion and later-phase changes. The main agent
reviews scope and architectural alignment, then executes inserted subphases
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
> resource-lifetime state. Do not introduce or inspect an external tracing
> framework merely by analogy; evaluate one only when the commit presents a
> concrete unmet requirement that the existing diagnostics cannot satisfy.
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

- The main agent runs these ordered stages: full tidy, full build, every
  applicable cleanup profile, full tidy, cleanup review/remediation, final full
  build, focused tests/acceptance, whole-plan framework audit. Cleanup never
  precedes successful initial tidy/build or follows the final build; tests
  require that final build to pass. The whole-plan audit's explicit remediation
  cycle below is the exception.
- Both builds use `./mmltk --build`. Both tidy passes use the complete
  configured `./mmltk --tidy` suite regardless of changed files or commits.
  Resolve every genuine finding before the initial build. Do not reuse earlier
  cleanup reports or start cleanup before initial tidy/build succeed.
- Immediately after the successful pre-cleanup tidy and build, commit every
  outstanding tracked change with the exact message `pre cleanup`; keep
  `actionplan.md` uncommitted.
- Obtain one full report per applicable cleanup profile, regardless of earlier
  commits or changed files. Resolve every genuine hit, then run a final full
  pass of every profile. Once all are clean, run the second full tidy and resolve
  every genuine finding. If tidy remediation changes code, repeat cleanup then
  tidy before continuing.
- Group targeted cleanup and tidy follow-up paths in one invocation where
  practical while preserving the governing stage order.
  Proven detector false positives may use the narrow inline suppressions
  documented below.
- After clean cleanup/tidy, spawn exactly one fresh sol xhigh cleanup adversarial
  reviewer for the entire Final Validation, before the final rebuild. Supply
  the complete diff from `pre cleanup` and the workflow-agnostic verifier prompt
  below exactly once. Require it to
  prioritize time complexity and system-boundary demarcation while auditing
  cohesive ordinary C++ systems, high-quality DRY object-oriented design,
  appropriate public/private class ownership, properly sealed interfaces,
  appropriately scoped ordinary functions, canonical reflected schemas,
  physical RAII resource safety, performance, and the absence of
  detector-driven code distortion. It must reject brute-force suppression,
  blind or overused templating, line compression, respelling, and indirection
  added solely to quiet a detector. Schema/reflection and genuinely reusable
  compile-time polymorphism may remain templated.
- On `NOT COMPLETE`, the reviewer writes `remediationplan.md`; the main agent
  checks scope/architecture and assigns exactly one astra medium executor.
  Follow the remediation workflow with that reviewer until `COMPLETE`, never
  spawning another cleanup reviewer or reissuing the prompt. After remediation,
  rerun every applicable cleanup profile, then full tidy, before follow-up review.
- After the reviewer returns `COMPLETE` and cleanup and tidy reruns are clean,
  commit all outstanding tracked cleanup, tidy, and remediation changes with
  the exact message `post cleanup`; keep `actionplan.md` uncommitted.
- From `post cleanup`, run the final full build once, then the plan's focused
  tests and acceptance sequence, with no intervening cleanup/tidy. After all
  pass, run the whole-plan audit. No further cleanup review is permitted.

### Post-validation whole-plan framework audit

Run exactly once after the final build and all required focused tests/acceptance
pass. This separate audit neither replaces phase/cleanup reviews nor creates
numbered phases or subphases.

Spawn exactly one fresh astra max reviewer without inherited context. Supply
repository path, immutable plan baseline, current `HEAD`, complete working-tree
diff, and prompt below. The authoritative review set includes every tracked
baseline-to-working-tree change: phase/cleanup commits, validation fixes, build
wiring, generated source definitions, tests, and instructions. The prompt
governs the independent audit, permitted mutation, and architecture criteria.

On `NOT COMPLETE`, the main agent checks the reviewer's `remediationplan.md`
for scope/architecture and assigns exactly one astra medium executor. Rerun
every applicable cleanup profile then full tidy before returning remediation to
the same reviewer. After `COMPLETE`, rerun the selected full build and every
affected focused test/acceptance case. Follow-ups are one engagement: do not
spawn another whole-plan reviewer, reissue the prompt, or recursively audit.

Use this prompt verbatim, replacing `<REPOSITORY>`, `<PLAN BASELINE>`,
`<CURRENT HEAD>`, and `<REVIEW SET>` with concrete values:

> Work as a fresh standalone whole-plan architecture reviewer in
> `<REPOSITORY>`. Do not rely on or request prior conversation context. Read
> `AGENTS.md`, `CONTRACT.md`, and `actionplan.md` completely before acting.
> Audit `<REVIEW SET>`, containing every tracked change from plan baseline
> `<PLAN BASELINE>` through current commit `<CURRENT HEAD>` and the complete
> working tree. Inspect every changed artifact and relevant unchanged caller,
> callee, dependency, configuration, build rule, generated boundary, test, and
> persisted form. Confirm and report the exact changed-file count.
>
> Find patterns that are only visible across the complete plan: repeated
> concepts, cross-file change clusters, misplaced ownership, hidden coupling,
> duplicated vocabulary, and boundaries whose edit blast radius exceeds their
> product responsibility. Treat each affected C++ boundary as if it required
> C++20-module-quality dependency hygiene, but do not introduce modules. Model
> include, link, schema, generation, concept, and ownership dependencies as a
> directed acyclic graph. Identify cycles, reverse dependencies,
> transitive-include reliance, include-order requirements, incomplete ordinary
> headers, implementation leakage, and declarations that would not survive
> isolated compilation.
>
> Require a focused ordinary class, sealed owner, factory, helper, reusable
> template, or canonical C++26-reflected declaration only when it consolidates
> a demonstrated shared concept, repeated behavior, or authoritative fact.
> Prefer compile-time structural projection with no runtime lookup or storage
> cost. Keep product policy, resource ownership, and control flow in cohesive
> ordinary systems with direct APIs. Seek measurable reductions in future
> cross-file edits, allocation, CPU/GPU transfer, blocking, memory churn, and
> algorithmic complexity. Reject facades, passthrough coordinators, one-method
> wrappers, speculative abstractions, parallel registries, reflection over
> resource or execution state, preprocessor-heavy mirrored schemas, hidden
> control flow, and indirection without a demonstrated reuse, ownership,
> safety, performance, or blast-radius benefit.
>
> Audit the C++ to generated-Rust boundary for one canonical native vocabulary,
> exhaustive reflected projection and dispatch, stable field identity, schema
> validation, and removal of handwritten member-wise or string-keyed mirrors.
> Keep visual copy, component state, layout, styling, theme, navigation, and
> interaction behavior owned by Rust and Iced. Treat observability, tracing,
> logging, and correlation as effect-only diagnostics unless the plan states a
> product requirement. Diagnostic identities never become ordering,
> cache-validity, acknowledgement, or resource-lifetime state.
>
> Preserve every observable behavior, failure path, integration outcome,
> persisted format, resource-lifetime guarantee, concurrency property, test
> case, widget identity, styling fact, and user-facing function. Reconstruct
> substantial moved, deleted, or replaced behavior through its callers,
> callees, tests, and persisted forms. Reject any simplification that silently
> loses capability. Account for the successful cleanup, build, focused-test,
> and acceptance evidence, but independently challenge architecture,
> performance, modularity, and cross-boundary maintainability.
>
> Return one consolidated report with a dependency-DAG and ownership
> assessment, exact locations, demonstrated impact, and the minimum cohesive
> correction for every finding. Say `COMPLETE` only when no material
> whole-plan framework correction remains. Optional ideas do not block
> completion.
>
> If the result is `NOT COMPLETE`, write or replace `remediationplan.md` with
> an executable remediation plan before returning. This is your only permitted
> mutation. Include the authoritative review set, consolidated findings,
> concrete phases, important files, exact actions and locations, architectural
> constraints, and post-cleanup concerns. Do not add a validation or commit
> phase, do not edit `actionplan.md`, and do not modify implementation,
> requirements, contracts, or instructions.

### Workflow-agnostic verifier prompt

Use this prompt verbatim, replacing `<WORK SCOPE>`, `<REQUIREMENTS SOURCE>`, and
`<REVIEW SET>` with concrete values:

> Audit the complete `<REVIEW SET>` for `<WORK SCOPE>` against every requirement
> in `<REQUIREMENTS SOURCE>` and all applicable repository instructions. Review
> from scratch, inspect the whole review set, and return all findings together.
>
> First, translate the requirements into an explicit acceptance checklist.
> Identify intended behavior, affected components, inputs and outputs,
> invariants, integration points, performance constraints, and evidence needed
> for each item. Inspect every changed artifact and relevant unchanged caller,
> callee, dependency, configuration, build rule, generated artifact,
> documentation file, and test.
>
> Perform a first pass for functional correctness and system cohesion. Verify
> ordinary APIs are direct and complete, each system has a focused product
> purpose, reflected intents/events/snapshots come from canonical typed
> declarations, complete cutovers remove obsolete routes, and repeated code is
> consolidated into a clear reusable abstraction. Check normal, empty, limit,
> invalid, duplicate, partial-progress, cancellation, retry, restart, shutdown,
> and cleanup paths where applicable.
>
> Unless the requirements explicitly change product logic, treat every
> observable outcome and failure path in the prior implementation as required.
> For each substantial deletion or interface replacement, reconstruct what the
> removed code did from its callers, callees, persisted forms, and tests. Verify
> the replacement retains those outcomes and reuses cohesive existing behavior
> where possible; a smaller interface does not justify silent capability loss.
>
> Check physical resource safety explicitly. Trace allocation, context binding,
> buffer and view lifetime, synchronization, worker stop and join, partial
> construction, exception cleanup, device or dependency loss, and cross-thread
> access. Verify RAII releases each resource safely, receiver-owned image copies
> complete before borrowed views release, and concurrency preserves valid data
> without polling, unnecessary blocking, races, leaks, or unbounded growth.
>
> Check failure propagation, integration behavior, observability, security,
> deterministic validation evidence, allocation and memory churn, CPU/GPU
> transfer count, blocking, and algorithmic complexity. Challenge assumptions at
> component boundaries and interactions between individually correct changes.
> Prioritize time-complexity regressions, boundary demarcation, public/private
> class ownership, sealed interfaces, and whether repeated member-wise mappings
> should derive from canonical C++26 reflection instead of handwritten parallel
> code.
> Mark a review dimension `NOT APPLICABLE` only with a concrete reason.
>
> Perform a second adversarial pass beginning with invalid input, inconsistent
> local conditions, boundary values, unavailable dependencies, exceptions,
> partial execution, capacity
> pressure, concurrency interleavings, cancellation, shutdown, and cleanup.
> Re-read the complete review set from those paths and verify the implementation
> remains cohesive, resource-safe, bounded, performant, and consistent with
> `CONTRACT.md`.
>
> Return one consolidated report. For every finding include severity, exact
> artifact and location, failing execution path, violated requirement, and the
> minimum required correction. Include a coverage ledger mapping every
> requirement to `VERIFIED` or to a finding. Say `COMPLETE` only when both passes
> find no unresolved issue.
>
> Treat `<REVIEW SET>` as authoritative and verify it is complete and internally
> consistent. Do not edit reviewed artifacts or their governing plan,
> specification, ticket, or requirements source. Obey workflow-specific
> validation timing and plan-file restrictions.
>
> If the result is `NOT COMPLETE`, write or replace `remediationplan.md` with an
> executable remediation plan before returning. This is the review agent's only
> permitted mutation. Include the authoritative review set, consolidated
> findings, concrete phases, important files, exact actions and locations,
> architectural constraints, and post-cleanup concerns. Do not add a validation
> or commit phase, do not edit `actionplan.md`, and do not modify implementation
> or requirements artifacts.

For recurring similar findings, stop symptom patches. Re-read the complete
system API, reflected vocabulary, concurrency, lifetime, failure, and integration
paths; require one cohesive correction before phase closure.

## remediationplan.md

`remediationplan.md` is the reviewer's executable phased plan for the executor.
After main-agent scope/architecture review, the same astra medium executor
completes and updates it until no phases remain. Only then begin additional review.

Include a problem statement or goal, Summary, Scope, Architecture, authoritative
review set, findings, concrete phases, important files, exact actions/locations,
architectural constraints, and post-cleanup concerns; exclude user-facing notes
and repeated `AGENTS.md` constraints.

### Writing a remediation plan

- Do not add validation or commit phases; those remain in `actionplan.md` and
  the main-agent workflow.
- Arrange dependent work to avoid back-and-forth edits. Compilation occurs in
  Final Validation.
- Include every file expected to change.

### Executing a remediation plan

- The executing subagent modifies `remediationplan.md`.
- Work may be pulled forward when it creates a complete, cohesive cutover.
- Atomically remove each completed phase from `remediationplan.md`, leaving only
  actionable steps. Change only that phase; preserve Summary, Scope,
  Architecture, and scope clarification.
- Apply the action-plan behavior-preservation rule before deleting or replacing
  substantial code, including code outside the immediate finding location.
- Do not defer updates until the end or force-commit `remediationplan.md`.
- Apply the action-plan complete-cutover and compatibility rules.

## Code Churn

Accept churn for performance, cohesive design, class/template structure,
modularity, reuse, and reduced blast radius. Remove legacy code; consolidate
repetition in appropriate shared helpers, classes, functions, templates,
factories, or reflected declarations. Use C++26 reflection for canonical schema
derivation and exhaustive dispatch.

## Code Deduplication

Remove all genuine duplicated code in scope, including cross-file duplication.
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

Prefer deleting unused variants over abstracting them. Before extracting helpers,
search callers for an existing registry, reflected dispatch surface, factory,
ordinary system, or utility expressing the concept. Before deleting/replacing
artifacts, search the entire repository and migrate every caller in one cutover.

`tools/generate_cleanup_json.mjs` writes
`cleanup/cpp-code-deduplication.json` with:

- `hits`: Duplo textual duplicate blocks, ordered cross-file before within-file.
- `structural_hits`: CPD structural duplicate blocks, ordered cross-file before
  within-file.

Cleanup affecting handwritten Rust runs both the default C++ profile and
`tools/generate_cleanup_json.mjs --frontend`, which writes
`cleanup/frontend-code-deduplication.json` with the same ordered hit model.
Resolve Rust duplication in the component owning the behavior or data.

### Deduplication rules

- Resolve a hit by consolidating repeated behavior or data into one reusable
  implementation or canonical reflected declaration.
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
execution: after each reviewed phase closes and after Final Validation for all
remaining tracked cleanup/validation changes, including unrelated tracked
changes. Leave post-review and other non-plan work uncommitted. Never add or
commit `actionplan.md`.

## Tests

Do not add regression tests. You may add standard tests that exercise required
behavior, boundaries, failure handling, resource safety, and integration.

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

All validation follows Final Validation's full
tidy/build/cleanup/tidy/final-build/tests order. Run both builds with
`./mmltk --build`. During action-plan execution, insert the single cleanup
review before the final build and the whole-plan audit after tests, with the
specified remediation cycles.

The suite formats first-party C/C++/CUDA, runs clang-tidy on supported
translation units, and validates reflection units through GCC compiler objects.
Cppcheck is gated off because its parser lacks C++26 reflection support.

For follow-ups, audit the entire prior tidy log, group failures by root cause,
fix the whole known batch including blocked downstream importers, and rerun
flagged files together where practical. Do not run `--third_party`.

## Governing Contract and System Architecture

- Keep this file focused on workflow and repository navigation. Put product
  architecture in `CONTRACT.md`, documentation, and source, not code-specific
  names or examples here. Exact operational prompts and log references are
  exceptions.
- `CONTRACT.md` states only positive desired architecture/product outcomes;
  put historical negatives and migration details in plans.
- `CONTRACT.md` describes broad architectural, browser, UI, GPU, performance,
  failure, and shutdown outcomes. Keep it concise, using diagrams and a
  current ownership matrix at system boundaries. Keep per-system algorithms,
  method and protocol-record inventories, library choices, fixed private
  budgets, temporary migration state, test fixtures, callback plumbing,
  diagnostic layouts, and class-private state out of it.
- `CONTRACT.md` is authoritative; implementation/plans converge toward it.
  Change it only for user-intended architecture/product outcome changes.
- Consult the contract, relevant documentation, and owning source for product
  structure and integration boundaries.

@CONTRACT.md
