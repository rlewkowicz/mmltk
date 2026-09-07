# AGENTS.md

## Sub Agents

Use subagents only when the user explicitly requests them or when executing a
specific `actionplan.md`. Do not poll subagents for status; wait for them to
return. After an action-plan executor returns,
send its completed work directly to the designated review agent. The main agent
reviews remediation plans for scope and architectural alignment without
duplicating the review agent's implementation audit.

If a review agent returns more than three times for one phase, pause and read
`CONTRACT.md`, the phase requirements, and the affected code deeply. Look for
unnecessary complexity, an incohesive system boundary, duplicated
vocabularies, or an opportunity to simplify with C++26 reflection. Consolidate
the recent direction into one coherent implementation and complete the phase.
The main agent performs this implementation audit after the repeated returns.

## Optimization

Minimize loops, CPU/GPU transfer, memory churn, blocking, and unnecessary
allocation. Maximize useful parallelism. Prefer pinned application memory where
appropriate. Allocate long-lived buffers once, size them tightly, and reuse
them aggressively. Use compact, well-defined data types and buffers sized only
for the task. Prefer O(1) algorithms where practical.

## Platform

This is a Linux-only data system. Optimize for Linux and spend no effort on
macOS or Windows compatibility.

## third_party

`third_party` is part of this codebase and may be modified when needed.
Application logic belongs in the application layer. Firefox is a stripped-down
application shell with GPU integration.

Preserve vendored build and language policies unless the task explicitly
changes them. The core toolchain policy below does not migrate `third_party`.
Firefox retains its cached Clang toolchain and bootstrap sysroot.

## Containerization

Everything is built and designed to run via container. Do not execute local
toolchains other than cleanup, invoke Docker directly, or assume paths are
relative to this host. Every build, test, sanitizer, debugger, profiler, and
containerized diagnostic command must go through `./mmltk`. If the wrapper
lacks a required capability, add it to `./mmltk` and use the wrapper capability.

## Building

First-party ordinary C++ uses GCC 16.2 and C++26 with reflection. First-party
CUDA uses NVCC 13.4, CUDA C++23, and GCC 16.2 as the host compiler with the
explicit unsupported-host override. GCC 14 is confined to bootstrapping GCC
16.2. Explicit container source builds of ONNX, simdjson, and cppcheck use GCC
16.2; their configured dependency language policies remain separate.

Development and runtime bases are independently owned Ubuntu 24.04 stages.
`docker/nvidia-payload.json` defines the selected binary payload from the
pinned NGC donor, its normalized locations, dependencies, and notices. Keep
that manifest authoritative for both image branches.

During action-plan execution, do not run full product builds or tests before
Final Validation. A narrowly targeted code-generation build may run through
`./mmltk` before Final Validation when it is required to materialize or inspect
a generated cross-language artifact. If the wrapper lacks the needed reusable
generator target or flag, add it to `./mmltk`; do not invoke the local toolchain
or Docker directly. Generated Rust remains build-directory output and is never
edited by hand. Fix generation defects in canonical reflected declarations,
generator code, or build wiring, then regenerate. Final Validation builds
backend or C++ changes with `./mmltk --build`; a GUI-only plan may use
`./mmltk --build-gui` when it changes no backend or C++ source.
Use `./mmltk --test <suite> --executable <target> -- <Catch2 arguments>` for
focused native tests. Use the wrapper's `--config`, `--gdb`, `--gdb-command`,
and `--env` options for diagnostic configurations, debugger sessions, and gated
test instrumentation.

## Crashes, Logging, and Debugging

Provide granular opt-in JSONL logging with maximum useful troubleshooting
detail. Disabled logging must avoid collecting and formatting diagnostic data.
Gate existing print and logging statements that affect normal runtime. Prove a
bug through tests or gated logging before changing behavior; do not infer fixes
from crash-dump symbols alone.

If the same distinct test still fails after four evidence-driven fix and
validation attempts during action-plan execution, assign one fresh
astra max subagent the exact failure, requirements, complete current
diff, attempted directions, and focused `./mmltk` command. Require a deep review
of ordinary API behavior, cohesive system boundaries, RAII resource safety,
concurrency, and failure propagation. Wait without polling and do not edit the
same artifacts concurrently.

## actionplan.md

An `actionplan.md` is an executable phased plan for another agent.

Do not write user-facing notes, answers, commentary, or reminders in a plan.
Do not repeat constraints already stated in `AGENTS.md`.

Plans must contain a problem statement or goal, Summary, Scope, Architecture,
concrete phases, important files, exact actions and locations, risks, and
post-cleanup concerns. Confirm relevant files and line numbers in advance. Do
not include vague directives such as “explore.” Phases may overlap when their
implementation naturally forms one coherent cutover.

### Writing an action plan

- Do not add a Commit section or phase; the session-level commit rule applies.
- Arrange dependent work to avoid back-and-forth edits. Compilation occurs in
  Final Validation, so intermediate states need not compile.
- In plan mode, save the final plan as `actionplan.md` before execution.
- Include every file expected to change.

### Executing an action plan

- Phase ordering is sequencing guidance. Work may be pulled forward when it
  forms a complete, architecturally aligned cutover. Reviewers evaluate the
  implemented review set against all applicable requirements.
- Unless a phase explicitly requests a product-logic change, preserve every
  existing observable outcome, failure path, and integration behavior across
  interface cutovers. Before deleting or replacing a substantial implementation,
  trace what it does through its callers, callees, persisted forms, and tests;
  reuse cohesive behavior where possible and carry all required outcomes into
  the replacement.
- Spawn exactly one astra medium agent for the first pass of each
  implementation phase. Use a new executor and reviewer for each such phase.
  Use the existing phase executor for remediation work and the existing phase
  reviewer for follow-up review. Final Validation follows its separate
  main-agent workflow below.
- The main agent handles Final Validation in this order: tidy, build, cleanup,
  tidy, build, tests, with the single cleanup review before the final build.
  The main agent handles small evidence-driven follow-up fixes. Large missing
  implementation remains delegated work.
- Update `actionplan.md` atomically after each completed phase. Update only the
  individual phase without changing Summary, Scope, Architecture, or scope
  clarification; inspect later phases for material impact, remove completed
  phases entirely, and leave only remaining actionable steps. Record pertinent
  detours in the active phase while it remains actionable.
- Before closing an implementation phase, spawn exactly one terra xhigh
  reviewer after the work is complete. Give it the relevant complete diff.
  Wait for its report and use the report before updating the executor. Use
  the same reviewer for phase follow-ups. The reviewer must say `COMPLETE`
  before phase closure.
- Each phase reviewer compares `actionplan.md`, `CONTRACT.md`, and `AGENTS.md`
  for end-to-end continuity. The review identifies ambiguity, contradictions,
  vocabulary drift, missing handoffs, and requirements that cannot be
  implemented together.
- Reviewers treat unrequested behavior loss as a blocker. For substantial
  deletions or interface replacements, reconstruct the deleted behavior from
  the complete prior implementation, callers, persisted forms, and tests, then
  verify that every outcome is retained or is explicitly superseded by the
  requirements. Prefer reuse of cohesive existing logic over deletion followed
  by a partial reimplementation.
- Reviews of Rust/Iced work verify the repository's component ownership,
  local-message, mapped-child-message, domain-outcome, routing, and small-root
  message rules.
- The reviewer must not edit `actionplan.md`. Staging files is optional. Fix
  medium through critical blockers that impede the required application
  behavior; optional nice-to-haves do not block the phase.
- Do not defer plan updates until the end and do not force-commit
  `actionplan.md`.
- Treat interface changes as complete cutovers. Preserve backward
  compatibility only when explicitly required. Do not add incremental
  compatibility layers.
- Do not run full product builds or tests before Final Validation. Narrow
  build-time generation through `./mmltk` is permitted only for required
  generated cross-language artifacts.
- Reviews assess human-scale developer boundaries as well as runtime behavior.
  Adding a product capability should extend one cohesive system or local
  component through canonical reflected types, without mirrored vocabulary,
  monolithic owners, or passthrough coordination layers.
- After a reviewer marks a phase `COMPLETE` and the completed phase is removed,
  commit all outstanding tracked changes. Keep `actionplan.md` uncommitted.

### Final Validation workflow

- Use `./mmltk --build` for both build stages below. The existing GUI-only
  exception uses `./mmltk --build-gui` for both stages when the plan changes no
  backend or C++ source.
- Validation begins with tidy and build, not cleanup. Cleanup is a stage within
  Final Validation, never a prerequisite for starting validation.
- The governing stage order is one full tidy pass, one full build, every
  applicable cleanup profile, a second full tidy pass, the cleanup review and
  any remediation, one final full build, then focused tests and acceptance
  coverage. Do not start cleanup before the initial tidy and build succeed, do
  not run cleanup after the final build, and do not start tests before that
  final build succeeds.
- Begin Final Validation with the repository-configured full
  `./mmltk --tidy` suite and resolve every genuine finding. Run the selected
  full build after tidy is clean. Do not begin cleanup or use a cleanup report
  from an earlier run until both the full tidy pass and build succeed.
- Immediately after the successful pre-cleanup tidy and build, commit every
  outstanding tracked change with the exact message `pre cleanup`; keep
  `actionplan.md` uncommitted.
- Begin cleanup with one full report for every applicable cleanup profile. Do
  not infer report scope from earlier commits or changed files. Resolve every
  genuine cleanup hit, then run one final full pass of every applicable cleanup
  profile. After every cleanup profile is clean, rerun the
  repository-configured full `./mmltk --tidy` suite and resolve every genuine
  finding. If second-pass tidy remediation changes code, return to the cleanup
  stage and repeat cleanup followed by tidy before continuing.
- Group targeted cleanup and tidy follow-up paths in one invocation where
  practical while preserving the governing stage order.
  Proven detector false positives may use the narrow inline suppressions
  documented below.
- After cleanup and tidy are clean, run exactly one initial cleanup adversarial
  review for the entire Final Validation, before the final rebuild. Spawn one
  fresh terra xhigh reviewer and give it the complete diff from `pre cleanup`
  and the workflow-agnostic verifier prompt below exactly once. Require it to
  prioritize time complexity and system-boundary demarcation while auditing
  cohesive ordinary C++ systems, high-quality DRY object-oriented design,
  appropriate public/private class ownership, properly sealed interfaces,
  appropriately scoped ordinary functions, canonical reflected schemas,
  physical RAII resource safety, performance, and the absence of
  detector-driven code distortion. It must reject brute-force suppression,
  blind or overused templating, line compression, respelling, and indirection
  added solely to quiet a detector. Schema/reflection and genuinely reusable
  compile-time polymorphism may remain templated.
- If the cleanup reviewer returns `NOT COMPLETE`, it writes
  `remediationplan.md`. The main agent reviews that plan for scope and
  architectural alignment, assigns it to exactly one astra medium executor, and
  follows the remediation workflow with the same reviewer until `COMPLETE`.
  These follow-ups continue the single review engagement: do not spawn another
  reviewer or issue a second workflow-agnostic verifier prompt. After
  remediation, rerun every applicable cleanup profile and then the full tidy
  suite before asking that same reviewer to check the remediated review set.
  This cleanup audit is the only review-agent requirement during Final
  Validation.
- After the reviewer returns `COMPLETE` and cleanup and tidy reruns are clean,
  commit all outstanding tracked cleanup, tidy, and remediation changes with
  the exact message `post cleanup`; keep `actionplan.md` uncommitted.
- From the reviewed checkpoint, run the selected full build once. After that
  build succeeds, run the required focused tests and acceptance coverage in the
  sequence required by the plan. Do not insert another cleanup or tidy stage
  between this final build and the tests.
- The main agent handles Final Validation. After the cleanup reviewer returns
  `COMPLETE`, do not run another review after the `post cleanup` checkpoint,
  final build, focused tests, or acceptance coverage.

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

If a reviewer returns similar issues, stop isolated symptom patches. Re-read
the complete system API, reflected vocabulary, concurrency, resource lifetime,
failure, and integration paths, then require one cohesive correction before
the phase closes.

## remediationplan.md

A `remediationplan.md` is an executable phased plan written by a review agent
for the phase executor. The main agent reviews it for scope and architectural
alignment before follow-up. The same astra medium executor completes it and
updates it until it contains no remaining phases. Additional review begins only
after the initial remediation plan is complete.

Plans contain a problem statement or goal, Summary, Scope, Architecture,
authoritative review set, findings, concrete phases, important files, exact
actions and locations, architectural constraints, and post-cleanup concerns.
They contain no user-facing notes and do not repeat `AGENTS.md`.

### Writing a remediation plan

- Do not add validation or commit phases; those remain in `actionplan.md` and
  the main-agent workflow.
- Arrange dependent work to avoid back-and-forth edits. Compilation occurs in
  Final Validation.
- Include every file expected to change.

### Executing a remediation plan

- The executing subagent modifies `remediationplan.md`.
- Work may be pulled forward when it creates a complete, cohesive cutover.
- Update `remediationplan.md` atomically after each completed phase. Update only
  the individual phase without changing Summary, Scope, Architecture, or scope
  clarification; remove completed phases entirely, and leave only remaining
  actionable steps.
- Apply the action-plan behavior-preservation rule before deleting or replacing
  substantial code, including code outside the immediate finding location.
- Do not defer updates until the end or force-commit `remediationplan.md`.
- Treat interface changes as complete cutovers. Preserve backward compatibility
  only when explicitly required. Do not add incremental compatibility layers.

## Code Churn

Accept code churn when it improves performance, cohesive system design,
class/template structure, modularity, reuse, and blast radius. Remove legacy
code and fold repeated behavior into shared helpers, classes, functions,
templates, factories, or reflected declarations where appropriate. Use C++26
reflection for canonical schema derivation and exhaustive dispatch.

## Code Deduplication

Remove all genuine duplicated code in scope, including cross-file duplication.
Do not install system dependencies to bypass cleanup. Do not compress lines,
respell code, or add indirection merely to evade detector limits.

Use C++26 static reflection to derive repeated member-wise machinery from one
canonical reflected declaration when a facility applies broadly across types.
Typical candidates include serialization and wire projection, generated GUI or
inspector fields, object-storage mappings, hashing, equality, exhaustive
dispatch, and other operations that would otherwise require one manually
maintained field map per class. Adding or removing a reflected member should
flow through these generic facilities without a second hand-written inventory.

Prefer compile-time reflection and generation with no runtime lookup or storage
cost. Use dynamic reflection, macro registries, or runtime type tables only when
the product genuinely requires runtime discovery. Reflection complements rather
than erases system boundaries: ordinary classes retain clear public contracts,
private implementation state, sealed ownership, and direct functions, while
reflection owns only reusable structural projection.

Prefer deleting an unused variant over abstracting it. Before extracting a new
helper, search callers for an existing registry, reflected dispatch surface,
factory, ordinary system, or utility that already expresses the concept.
Before deleting or replacing an artifact, search the complete repository and
move every caller in the same cutover.

`tools/generate_cleanup_json.mjs` writes
`cleanup/cpp-code-deduplication.json` with:

- `hits`: Duplo textual duplicate blocks, ordered cross-file before within-file.
- `structural_hits`: CPD structural duplicate blocks, ordered cross-file before
  within-file.

Cleanup that affects handwritten Rust runs both the default C++ profile and
`tools/generate_cleanup_json.mjs --frontend`. The frontend profile writes
`cleanup/frontend-code-deduplication.json` with the same ordered textual and
structural hit model. Resolve genuine Rust duplication through the component
that owns the behavior or the presentation-model data owner that owns the
fact. Do not compress lines, respell code, introduce one-method wrappers, or
add pass-through indirection to evade either detector.

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

Do not make commits unless executing an `actionplan.md`. Post-review and other
non-plan tasks leave changes uncommitted unless the user explicitly overrides
this rule. During action-plan execution, commit after each reviewed phase
closes. After Final Validation, commit all remaining tracked cleanup and
validation changes, including unrelated tracked changes. Do not add or commit
`actionplan.md`.

## Tests

Do not add regression tests. You may add standard tests that exercise required
behavior, boundaries, failure handling, resource safety, and integration.

For the Wayland hardware test, if evidence identifies the cause of the current
failure, make the focused correction. Roll back that correction only when it
does not resolve the specifically targeted failure or the test remains stuck at
the same point. If it advances the test to a distinct later blocker, retain the
correction and add logging at every boundary needed to diagnose that new
blocker. Base each subsequent change on proven facts and captured logs, not a
guess or an unrelated later timeout.

## Display

The application is Wayland-only. Do not use CPU display or X11 fallbacks.

## Web GUI

The GUI uses Iced Rust. During action-plan execution, defer GUI builds and tests
to Final Validation. Outside an action plan, use `./mmltk --build-gui` for
GUI-only changes and `./mmltk --build` for backend or C++ changes, then test the
modified backend behavior.

Native catalogs, enums, defaults, constraints, stable field identities,
snapshots, events, operations, and other domain facts are projected through
C++26 reflection into generated typed Rust. Do not restate them as parallel
Rust lists or string switches. Visual copy, widgets, layout, styling, theme,
navigation composition, and responsive behavior remain owned by Rust and Iced.
Regenerate generated bindings through `./mmltk`; never edit generated Rust by
hand. Handwritten Iced components own visual grouping and user-facing copy.

Reviews of Iced work verify the rendered product composition and geometry,
stable widget identities, light and dark styling facts, typed progress
reduction and placement, and immediate versus debounced edit cadence. Merely
constructing an Iced `Element` is not sufficient acceptance evidence.

Complex pages and widgets use dedicated component structs and modules with
local data, view, and update logic. Component-specific UI facts remain local
instead of expanding the root `App`. Each component or page defines a local
`Message` enum. The root `Message` wraps child messages, for example
`Message::Settings(settings::Message)`, and child `Element`s use `.map()` to
route messages to the parent. Raw widget messages remain local; components
bubble domain outcomes such as `SaveRequested` or `TabClosed(id)`.

A dedicated routing module switches pages. Layout definitions stay out of the
root update handler, and the root `Message` remains small.

## Polling and Events

Prefer event-driven execution, callbacks, stop tokens, and blocking waits local
to system-owned workers. Avoid heavy mutexing, polling, arbitrary delays, and
unnecessary blocking.

## Tidy

Validation always starts with the repository-configured full `./mmltk --tidy`
suite, then a full build. Only after both succeed, run cleanup, then the full
tidy suite again, the final build, and tests. Both tidy passes cover the full
configured suite regardless of changed files or commits. During action-plan
execution, the single cleanup review belongs after clean cleanup and tidy,
before the final build. Use the GUI-only build exception where applicable.

The configured suite formats first-party C/C++/CUDA, runs clang-tidy on
supported translation units, and validates reflection translation units
through their GCC compiler objects. Cppcheck is currently gated off because
its parser does not support the repository's C++26 reflection syntax.

For follow-up runs, audit the complete prior tidy log, group all failures by
root cause, fix the complete known batch including blocked downstream
importers, and rerun all flagged files together where practical. Do not run
`--third_party`.

## Governing Contract and System Architecture

- When modifying `CONTRACT.md`, state only the positive desired architecture
  and product outcomes. Historical negatives and migration details belong in
  plans.
- `CONTRACT.md` describes broad architectural, browser, UI, GPU, performance,
  failure, and shutdown outcomes. Keep it concise, using diagrams and a
  current ownership matrix at system boundaries. Keep per-system algorithms,
  method and protocol-record inventories, library choices, fixed private
  budgets, temporary migration state, test fixtures, callback plumbing,
  diagnostic layouts, and class-private state out of it.
- `CONTRACT.md` is authoritative. Implementation and action plans converge
  toward it. Change the contract only when the user intentionally changes the
  desired architecture or product outcome.
- The canonical application shape is one application shell, one BrowserServer,
  independent ordinary product-domain systems, one PresentationSystem, and
  Firefox process integration.
- Ordinary C++ control flow is the default: direct calls, compact result types,
  normal exception propagation, and RAII cleanup. Map failures once at the
  nearest intent, worker, or external-service boundary.
- Use an explicit finite-state model only when the product domain genuinely
  requires one. Keep its vocabulary, transitions, and invariant private and
  colocated within one cohesive system. When behavior depends on mirrored
  phases or transition rules spread across components and files, simplify the
  API and control flow instead of adding coordinators, receipts, convergence
  objects, or proof machinery.
- Reviews evaluate observable behavior, cohesive APIs, bounded concurrency,
  direct failure propagation, and destructor-safe resources. They do not
  require formal ownership maps, transition maps, or quiescence proofs.
- Ordinary systems expose direct typed methods, minimal reflected snapshots,
  and typed UI events. One canonical reflected native vocabulary materializes
  intents, events, browser descriptors, validation, exhaustive dispatch, and
  cross-language projections.
- Each long-running system owns its workers, stop mechanism, CUDA
  configuration/context, streams, events, models, high-water buffers, and
  reusable staging appropriate to its workload.
- Cross-system images use borrowed typed read views for one explicit
  receiver-owned copy. The receiver waits for GPU completion before releasing
  the view and publishes receiver-owned storage.
- `PresentationSystem` is the single compositor and native writer for the
  shared exported backbuffer. Firefox retains native import, WebGPU/Vulkan,
  swapchain, compositor cadence, and Wayland presentation. Iced retains view
  transforms and rendering.
- Direct calls use ordinary exception propagation. Worker boundaries publish
  typed failures and isolate lazy reconstruction to the failed system.
- Shutdown stops ingress, requests system stops, returns from the browser loop,
  and relies on reverse-order RAII destruction to join workers and release
  physical resources.

@CONTRACT.md
