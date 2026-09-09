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
```
Examples:
  ./mmltk --logs --errors
  ./mmltk --logs -q 'onnx OR "CUDA error"'
  ./mmltk --logs -q '@event:shutdown AND NOT @event:started' --format timeline
  ./mmltk --logs -q 'trace_id=42' --correlate trace_id --tail --limit 80
  ./mmltk --logs -q '@event=firefox.workspace.ready' --correlate @surface
  ./mmltk --logs -q 'duration_ns>=1000000' --fields @event,duration_ns,trace_id
  ./mmltk --logs --where '@file:"latest-wayland-test"' --group-by @event
  ./mmltk --logs --family latest-wayland-test --errors --related-run --tail
  ./mmltk --logs --family latest-wayland-test --history --list-runs
  ./mmltk --logs --family latest-wayland-test --run 57-131007497132582 -q SIGSEGV
  ./mmltk --logs build/validation/viewer-copy-ownership-trace.log \\
      --family latest-wayland-test --history -q 'buffer="BufferId(21,1)"' --context 2

Grammar (quote the entire expression for the shell):
  expr      := expr OR expr | expr [AND] expr | NOT expr | '(' expr ')'
  primary   := text | '*' | has(field) | field operator value
  operator  := = != : ~ !~ > >= < <=
AND binds tighter than OR; NOT binds tightest. Adjacent terms imply AND.
Bare/quoted text and ':' are case-insensitive literal substring searches.
'='/'!=' compare exact typed values; ordering compares numbers; '~'/'!~' use
Python regex (case-sensitive; use (?i) for insensitive). Missing fields do
not satisfy comparisons, including '!='; NOT includes them. has() tests
presence including null/zero. Quote strings containing spaces or punctuation.

Fields:
  JSON paths (fields.name), event/trace_id/etc. (unqualified names also look
  inside fields), plus @file, @line, @text, @format, @clock, @time_ns,
  @event, @owner, @level, @error, @surface, @parse_error, @test, @tags,
  @run, @archive_id, @family, @artifact, @mtime_ns, @context_copy, @part,
  @terminal, @exit_code, @signal, @signal_number.
  @event resolves wrapped fields.event/name before event/name.
  @surface joins native uint64 surface_high/low and Firefox 32-hex surfaces.
  @error marks failure candidates from levels/event/message text; it is a
  search aid, not a diagnosis. Numeric error=0 metrics do not mark failures.
  child.signaled value=139 decodes to SIGSEGV (11); 143 to SIGTERM (15).
  Signals describe the observed termination, not whether shutdown was intended.
  Bare terms search test/tag/run/signal metadata as well as original text.
  Catch INFO copies retain their original timestamps and are labeled context-copy.

Inputs and ordering:
  Paths/globs are repository-relative or absolute within the repository.
  Directories select .jsonl/.log/.out/.txt; --recursive includes histories.
  Explicit files can have any suffix. Repeated paths are deduplicated.
  The default is build/validation, without recursive archived captures.
  Time ordering groups steady, UTC wall, timezone-free wall, per-file elapsed,
  and untimed records separately; ties use file/line. No clock offset is guessed.
  --order capture sorts files by captured mtime then preserves their line order;
  mtime is artifact recency, not an event timestamp or proof of causality.
  Use one capture at a time: IDs and monotonic times can repeat across runs.
  Files are read up to their captured byte size; correlation requires unchanged
  files over two passes. There is no follow mode, persistent index, or network.

Artifact families:
  --family STEM selects STEM.jsonl / STEM.log / STEM-native.log / STEM-firefox.log
  under build/validation (or supply a repository path). --history adds rotated
  siblings; --run selects an exact archive ID or 'current'. Adjacent rotations
  with the same PID within 10ms are grouped and explicitly labeled inferred.
  Shared (event, steady_ns) anchors link transcript captures to native runs and
  propagate known test/tag metadata. Missing anchors leave transcripts separate.
  --related-run includes other records from a matched run; named correlations
  are automatically scoped to the run in family/history mode. Summary output
  includes terminal events from matched runs independently of the sample limit.
```

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

- At the start of execution, record the current commit as the plan baseline in
  session state. The final whole-plan framework audit uses this immutable
  baseline; do not write it into `actionplan.md`.
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
  tidy, cleanup review, build, tests, and the whole-plan framework audit below.
  The main agent handles small evidence-driven follow-up fixes. Large missing
  implementation remains delegated work.
- Update `actionplan.md` atomically after each completed phase. Update only the
  individual phase without changing Summary, Scope, Architecture, or scope
  clarification; inspect later phases for material impact, remove completed
  phases entirely, and leave only remaining actionable steps. Record pertinent
  detours in the active phase while it remains actionable.
- After removing a completed phase, rewrite every surviving dependency, action,
  risk, and handoff that referenced it to name the concrete implemented API,
  owner, artifact, or invariant. An executable plan must not rely on the number,
  title, or prose of a phase that is no longer present.
- Before closing an implementation phase, spawn exactly one sol xhigh
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

### Post-main-phase framework audit

After committing an integer-numbered main implementation phase, run one
framework audit before advancing to the next main phase. A heading such as
`Phase 2` is a main phase; `Phase 2.1` is an audit-created subphase. Do not run
this audit after a subphase, Final Validation, or a Final Validation checkpoint.

Count the files changed by the main-phase commit. Spawn exactly one fresh astra
max agent with no inherited conversation context. Give it the commit, main
phase number and title, repository path, and the workflow-agnostic prompt below.
The audit agent reads the repository sources of authority and complete commit
itself. It may edit only `actionplan.md`; it does not edit implementation,
requirements, contracts, or instructions, run builds or tests, or commit.

The audit inserts only demonstrated, cohesive framework work as subphases of
the completed main phase, numbered `<N>.1`, `<N>.2`, and so on, before the next
main phase. It does not invent work to satisfy a phase count. It updates later
phases only where the proposed framework cutovers materially change their
dependencies, anchors, files, actions, or handoffs. The main agent reviews the
revised plan for scope and architectural alignment, then executes the inserted
subphases through the ordinary executor, review, plan-update, and commit
workflow.

Do not recursively audit those subphase commits. After all subphases created
for one main phase are complete, inspect every later phase against the actual
implemented framework. Update only materially affected dependencies, anchors,
files, actions, and handoffs before starting the next main phase.

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

- Use `./mmltk --build` for both build stages below. The existing GUI-only
  exception uses `./mmltk --build-gui` for both stages when the plan changes no
  backend or C++ source.
- Validation begins with tidy and build, not cleanup. Cleanup is a stage within
  Final Validation, never a prerequisite for starting validation.
- The governing stage order is one full tidy pass, one full build, every
  applicable cleanup profile, a second full tidy pass, the cleanup review and
  any remediation, one final full build, focused tests and acceptance
  coverage, then the whole-plan framework audit. Do not start cleanup before
  the initial tidy and build succeed, do not run cleanup after the final build,
  and do not start tests before that final build succeeds.
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
  fresh sol xhigh reviewer and give it the complete diff from `pre cleanup`
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
  This is the only cleanup review engagement during Final Validation. The
  separate whole-plan framework audit occurs only after the final build and all
  focused tests and acceptance coverage pass.
- After the reviewer returns `COMPLETE` and cleanup and tidy reruns are clean,
  commit all outstanding tracked cleanup, tidy, and remediation changes with
  the exact message `post cleanup`; keep `actionplan.md` uncommitted.
- From the reviewed checkpoint, run the selected full build once. After that
  build succeeds, run the required focused tests and acceptance coverage in the
  sequence required by the plan. Do not insert another cleanup or tidy stage
  between this final build and the tests.
- After all required tests and acceptance coverage pass, run the whole-plan
  framework audit below. Do not run another cleanup review after the
  `post cleanup` checkpoint.

### Post-validation whole-plan framework audit

Run this audit exactly once after the final build and every required focused
test and acceptance case pass. It is not a phase review, does not create
numbered phases or subphases, and does not replace the earlier cleanup
correctness audit.

Spawn exactly one fresh astra max reviewer with no inherited conversation
context. Give it the repository path, the plan baseline recorded at the start
of execution, the current `HEAD`, the complete working-tree diff, and the
workflow-agnostic prompt below. The authoritative review set is every tracked
change from the plan baseline through the current working tree, including
phase commits, cleanup commits, validation fixes, build wiring, generated
source definitions, tests, and instructions changed while executing the plan.
The reviewer reads `AGENTS.md`, `CONTRACT.md`, and `actionplan.md` itself. It
must not edit implementation, requirements, contracts, instructions, or the
governing plan.

The audit looks across the whole plan for repeated concepts and overlapping
changes that only become visible atomically: cohesive ownership boundaries,
module-quality acyclic dependency surfaces without introducing C++ modules,
canonical reflected schemas, C++/generated-Rust boundary simplification,
physical RAII safety, bounded concurrency, allocation and transfer reduction,
algorithmic complexity, and meaningful reusable classes, factories, helpers,
or templates. It rejects facades, pass-through layers, speculative
abstractions, reflection over execution state, and indirection without a
demonstrated ownership, reuse, safety, performance, or blast-radius benefit.

If the result is `NOT COMPLETE`, the reviewer writes or replaces
`remediationplan.md` as its only mutation. The main agent reviews that plan for
scope and architectural alignment, assigns it to exactly one astra medium
executor, and returns the resulting changes to the same reviewer. Follow-ups
remain one review engagement; do not spawn another whole-plan reviewer or
reissue the initial prompt. After remediation, rerun every applicable cleanup
profile and the full tidy suite before follow-up review. Once the same reviewer
returns `COMPLETE`, rerun the selected full build and every focused test and
acceptance case affected by remediation. Do not recursively run another
whole-plan framework audit.

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
