# AGENTS.md

## Optimization

Minimize loops, CPU/GPU transfer, memory churn, blocking, and unnecessary allocation. Maximize parallelism. Prefer pinned application memory where appropriate. Allocate long-lived buffers once, size them tightly, and reuse them aggressively. Use compact, well-defined data types and buffers sized only for the task.

## Platform

This is a Linux-only data system.

Do not spend effort on macOS or Windows compatibility. Optimize for Linux.

## third_party

`third_party` is part of this codebase.

It may originate upstream, but we own it. Modify it when needed.

## Containerization

Everything is built and designed to be run via container. Do not execute local toolchains, or assume paths are relative to this host. 

## Building

Building is done via ./mmltk --build if you modify cpp files, this is required. If you ONLY modify the gui you can use --build-gui

## Logging, Debugging
Everything should write granular jsonl logs with maximum verbosity to troubleshoot any issues that may arise later. Generally those should not be default options, and should not impact runtime if they are not set, profile etc. If you see logging or print statements that are not gated, always ensure you gate them. Even going so far as to not collect certain data if it is not needed etc.

## actionplan.md

An `actionplan.md` is an executable phased plan for another agent.

Do not write user-facing notes, answers, commentary, or reminders. Do not repeat constraints already stated in `AGENTS.md`.

Plans must contain concrete phases, important files, and specific actions. Do not include vague directives such as “explore". Confirm the changes, files and line numbers in advance, things to look out for and possible post cleanup. Phases can overlap if needed.

When writing an action plan:

- It should have a Summary, Scope, and Architecture section
- Do not add a Commit section or phase; the existing session-level commit rule already applies.

When executing an action plan:

- Update `actionplan.md` atomically after each major completed change.
- Update only individual phases. Do not modify summary, constraints, or scope clarification.
- Completed phases should be removed entirely
- Do not record what was done; leave only remaining actionable next steps.
- Before updating a phase, spawn exactly one subagent to verify the work is complete. Have the sub agent use git diff to review the relevant files.
- Spawn the xhigh subagent only after doing the work, wait for it to finish, and use its report before updating the plan. It must always validate any work you do before the phase is closed. If it finds you did not stage files, you still must have it verify after you stage the files. The subagent must say it's complete before you mark it done.
- The subagent must not edit `actionplan.md`.
- Do not defer plan updates until the end.
- Do not force commit the actionplan.md

Treat interface changes as complete cutovers. Do not preserve backward compatibility unless explicitly required. Do not make incremental compatibility layers.

Do not build or test until all phases are complete.

When executing an action plan, commit changes after each phase as been marked "nothing to do"

## Code Churn

Do not avoid code churn when it improves the system.

Prioritize performance, clear ownership, proper class/template structure, healthy architecture, modularity, reuse, and limited blast radius. Reduce legacy code, fold them into new shared helpers, classes, functions, templates, factories, or other reusable abstractions where appropriate.

## Code Deduplication

Do not avoid deduplication because the work is large. Do not install system dependencies to bypass file cleanup. When cleanup is requested, remove all duplicated code in scope, including cross-file duplication.

Extract shared helpers, classes, functions, templates, factories, or other reusable abstractions where appropriate. Re-architect code when duplication indicates a structural problem. Change public and private apis freely. Create new files freely.

Don't compress lines just to avoid the cap. Repeated lines are a good place for classes, factories, templates, and shared helpers. Get granular and make sub templates, sub extractions, factories, enums, vs modifying line lengths. 

## Git
Do not make commits unless you are executing an `actionplan.md`. Post reviews and other non-actionplan tasks should leave changes uncommitted unless the user explicitly overrides this rule. Do not add or commit the actionplan.md its self.

# Tests
Do not add regression tests

## Display
We are wayland only. Do not fall back to CPU display or X11 ever.

## Web GUI
The GUI is developed using Iced a rust framework. Building is done via ./mmltk --build-gui. When modifying the gui you don't have to run tests unless they are requested. But if you modify the backend C++ you should test the elements you modified.