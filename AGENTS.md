# AGENTS.md

## Optimization

Optimization is always on.

Minimize loops, CPU/GPU transfer, memory churn, blocking, and unnecessary allocation. Maximize parallelism. Prefer pinned application memory where appropriate. Allocate long-lived buffers once, size them tightly, and reuse them aggressively. Use compact, well-defined data types and buffers sized only for the task.

## Platform

This is a Linux-only data system.

Do not spend effort on macOS or Windows compatibility. Optimize for Linux.

## Testing

Always build and test in Docker. If you modified CEF, you must build it first.

`docker build -t mmltk .` must run to completion. Do not interrupt it.

Run tests through `./mmltk`. Do not run test frameworks directly via `docker run` except for debugging.

Prefer functional tests over brittle unit tests. Do not test meaningless implementation details such as exact line counts or incidental structure.

Mathematical code should be tested for outcomes, not implementation shape. Use `atol` and `rtol` when tolerance-based numeric comparison is required.

Don't add regression tests.

## CEF

CEF version is fixed at `146.0.7680.179`; do not change it.

Preserve the CEF cache. Never wipe, reset, or regenerate it.

CEF modifications must be made through patches in `third_party/cef/patch`. Add or update patches there, then run `./buildcef.sh` to completion. `./buildcef.sh` may be long-running; do not interrupt it.

Do not ever edit the build cache directly.

Before authoring, refreshing, splitting, or validating any CEF patch, restore the authoring checkout to stock CEF/Chromium `146.0.7680.179` with cache-preserving reset tooling. Preserve the ninja/siso build graph, build outputs, downloaded sources, and dependency caches. Do not derive patch context from a previously patched working tree; after the stock reset, apply repo patches only from `third_party/cef/patch/patch.cfg` and only in configured order. Patch files must be written for that fresh stock-plus-prior-patches application point and must pass dry-run from that state.

Use `python3 third_party/cef/stock_authoring_checkout.py --prepare-patch <patch_name>` before authoring or refreshing a CEF patch.

The verifier must finish with the exact error or miss that was just fixed. Example: do not fix a CEF patch failure by editing `.docker-cache/cef-build/...`; keep the cache as-is and correct only `third_party/cef/patch`. 

When modifying files with patches, don't do gymnastics to avoid it, but if it does not impact code quality/validity try to avoid modifying files that will cause large rebuilds. 

## third_party

`third_party` is part of this codebase.

It may originate upstream, but we own it. Modify it when needed.

## actionplan.md

An `actionplan.md` is an executable phased plan for another agent.

Do not write user-facing notes, answers, commentary, or reminders. Do not repeat constraints already stated in `AGENTS.md`.

Plans must contain concrete phases, important files, and specific actions. Do not include vague directives such as “explore,” “investigate,” or “look into.” Exploration should already be complete before writing the plan, though the next agent may still infer needed details.

When writing an action plan:

- It should have a Summary, Scope, and Architecture section
- Do not add a Commit section or phase; the existing session-level commit rule already applies.
- Before finalizing `actionplan.md`, spawn exactly one subagent to verify it follows the rules of this `AGENTS.md` file.
- Wait for the subagent report and revise `actionplan.md` if needed before responding.

When executing an action plan:

- Update `actionplan.md` atomically after each major completed change.
- Update only individual phases. Do not modify summary, constraints, or scope clarification.
- Completed phases must say only `nothing to do`.
- Do not record what was done; leave only remaining actionable next steps.
- Before updating a phase, spawn exactly one subagent to verify the work is complete.
- Spawn the subagent only after doing the work, wait for it to finish, and use its report before updating the plan.
- The subagent must also verify compliance with the Optimization section.
- The subagent must not edit `actionplan.md`.
- Do not defer plan updates until the end.

Treat interface changes as complete cutovers. Do not preserve backward compatibility unless explicitly required. Do not make incremental compatibility layers.

Do not build or test until all phases are complete.

Commit changes at the end of the session.

## Code Churn

Do not avoid code churn when it improves the system.

Prioritize performance, clear ownership, proper class/template structure, healthy architecture, modularity, reuse, and limited blast radius.

## Code Deduplication

For `generate_cleanup_json.mjs` and code deduplication work, complete the cleanup.

Do not avoid deduplication because the work is large. Do not install system dependencies to bypass file cleanup. When cleanup is requested, remove all duplicated code in scope, including cross-file duplication.

Extract shared helpers, classes, functions, templates, factories, or other reusable abstractions where appropriate. Re-architect code when duplication indicates a structural problem.

When running a cleanup based `actionplan.md`, have the subagent run generate_cleanup_json.mjs before validating the phase

## Git
Don't run git at the start of the session. The current state of the repo does not matter. You can run git after major changes if you need, or for final validation

## Display
We are wayland only. Do not fall back to CPU display or X11 ever.
