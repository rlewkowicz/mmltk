# Validation and focused tests

[Quick start](../README.md#build) · [Logging](logging.md) · [Headless Wayland](headless-wayland.md)

The governing validation sequence and review/checkpoint rules are in
[AGENTS.md](../AGENTS.md). Follow those rules when executing a plan.
The commands below describe individual capabilities; they are not a substitute
for the required stage ordering.

## Formatting and static analysis

```bash
./mmltk --tidy
```

The full configured suite formats tracked first-party C/C++/CUDA files,
refreshes `.cache/cmake/analysis`, runs clang-tidy where supported, and
validates reflection translation units through GCC compiler objects.
CUDA clang-tidy covers host/device code at `sm_86`. Cppcheck is currently
disabled because its parser does not support the repository's reflection
syntax; `--cppcheck-only` is unavailable.

For follow-up work, select one or several tracked sources with one `--file`,
or resume at a translation unit with `--start-at`:

```bash
./mmltk --tidy --file src/common/system/cpu_affinity.cpp src/common/system/execution_policy.cpp
./mmltk --tidy --file src/backend/imaging/explore/explore_render_core.cu
./mmltk --tidy --start-at src/common/system/cpu_affinity.cpp
```

`--file` and `--start-at` cannot be combined. Focused runs do not replace the
full passes required by the workflow. Tidy writes a log below `build/logs`;
`MMLTK_TIDY_LOG_FILE` selects another report path. The implementation and
available analysis overrides are in
[run_static_analysis.sh](../tools/run_static_analysis.sh).

## Deduplication reports

```bash
./mmltk --cleanup-report cpp
./mmltk --cleanup-report frontend
./mmltk --cleanup-report all
```

These generate the C++ and/or frontend reports under `cleanup/`.
`cpp-code-deduplication.json` and `frontend-code-deduplication.json` contain
textual `hits` and structural `structural_hits`, ordered with cross-file
findings before within-file findings. Use the applicable full profiles and
resolution rules in `AGENTS.md`; reports do not themselves change product code.

## Native and browser suites

```bash
./mmltk --test list
./mmltk --test core
./mmltk --test application-systems --executable mmltk_controller_visual_systems_tests
./mmltk --test browser-app
```

Native selections configure the cached Release graph by default, explicitly
build selected test targets, and run their executables. Current targets can be
Ninja no-ops. `--config dev` selects the development graph where supported.
The browser-app suite owns the GUI graph and its Cargo test target.

| Suite | Selection |
| --- | --- |
| `application-systems` | Browser, service, shell, data/compute, visual, GPU, and Live suites |
| `application-contracts` | Browser, service, visual, and serialization suites |
| `transport` | Physical browser transport and attachment |
| `browser-runtime` | Desktop entrypoint and Firefox process-owner fixtures |
| `core` | Core acceptance, dataset, model catalog, system/concurrency, CLI, and tool tests |
| `rfdetr` | Native RF-DETR model, training, inference, export, CUDA, and layer suites |
| `rfdetr-profile` | Instrumented training profile runner; selects `dev` |
| `browser-app` | Iced protocol, state, and transport tests |
| `workspace-wayland` | Packaged Firefox/NVIDIA hardware acceptance |
| `headless-compositor` | Real NVIDIA Weston availability and protocol checks |
| `headless-compositor-tool` | Supervisor ownership/failure fixtures without GPU |
| `cleanup-tool` | Cleanup-report tooling fixtures |
| `log-query-tool` | Log parser/query/correlation/triage fixtures |
| `build` | Build the configured native test targets without running them |
| `all` | Build the native test targets; run the ordinary native executables |

`all` builds `mmltk_workspace_wayland_integration` but excludes it from its run
list. It also does not execute `browser-app`, the tooling suites, or the
profile runner. Run those explicitly. `gui` and `tsan` suite names are
currently unavailable even though other GUI/development build facilities exist.

Some RF-DETR tests download model checkpoints and derive normalized weights,
ONNX, and TensorRT engines in `.cache/tests/rfdetr` on first use. Hardware-gated
tests may skip when their requirements are unavailable; a skip is not hardware
acceptance evidence.

## Selection, environment, deadlines, and debugging

Put test-runner arguments after `--`:

```bash
./mmltk --test core -- --list-tests
./mmltk --test rfdetr -- '~[optin]'
./mmltk --test browser-app -- presentation
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests \
  --env MMLTK_GDR_TEST_DEVICE=0 -- '[gdr][hardware]'
```

| Option | Behavior |
| --- | --- |
| `--executable TARGET` | Select one target owned by the suite |
| `--config release` or `--config dev` | Select the supported native graph |
| `--env NAME` or `--env NAME=VALUE` | Forward a native-test environment variable; repeatable |
| `--gdb` | Run one selected native executable in containerized GDB |
| `--gdb-command COMMAND` | Add an ordered debugger command; also enables GDB |
| `--headless-compositor` | Use private NVIDIA Weston with `workspace-wayland` |

For example:

```bash
./mmltk --test core --executable mmltk_common_system_tests --config dev \
  --gdb-command run --gdb-command bt
```

Noninteractive GDB uses batch mode; without supplied commands it runs the
executable and prints a backtrace. Ordinary native tests default to a
300-second executable deadline with a ten-second TERM-to-KILL allowance;
`MMLTK_TEST_TIMEOUT_SECONDS` must be a positive integer. Debugger runs bypass
that timeout.

`workspace-wayland` requires the packaged Release graph and rejects GDB.
`browser-app` accepts Cargo test arguments after `--` but does not support
native executable, environment, or debugger options. The three compositor/log
tool suites reject extra arguments and native test options; `cleanup-tool`
also owns its fixture invocation.

## Packaged Wayland acceptance

```bash
./mmltk --test workspace-wayland -- workspace_wayland_product_sigint
./mmltk --test workspace-wayland --headless-compositor -- workspace_wayland_product_sigint
```

Both use the packaged runtime and the native acceptance executable. The
visible path mounts the active host Wayland session. The private path adds a
GPU-rendered Weston output and retains the same product assertions.
Build the package first with `./mmltk --build`.

The acceptance suite enumerates its required cases before executing the
selection. Its own cases retain their internal deadlines; the optional
`MMLTK_TEST_TIMEOUT_SECONDS` adds an executable deadline for the headless
supervisor. See [headless details](headless-wayland.md) for readiness,
shutdown, artifact locations, and the limits of virtual-output evidence.
Use [logging](logging.md) to investigate one captured run at a time.
