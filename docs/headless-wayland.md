# Headless Wayland hardware validation

[Wiki index](README.md) · [Validation](validation.md) · [Logging](logging.md) · [Build prerequisites](build.md)

Run the existing packaged Firefox/NVIDIA acceptance suite with a private
GPU-rendered Weston output:

```sh
./mmltk --test workspace-wayland --headless-compositor
./mmltk --test workspace-wayland --headless-compositor -- workspace_wayland_retained
```

Omit `--headless-compositor` to use the existing visible host Wayland session.
The headless option creates a cached validation image from the exact packaged
runtime image. It installs pinned Weston packages, a Wayland inspection client,
and a validation-only input-seat module compiled in the build image. It does
not rebuild or replace the packaged application or Firefox.

Two focused checks avoid building the native acceptance graph:

```sh
./mmltk --test headless-compositor
./mmltk --test headless-compositor-tool
```

The first starts the real NVIDIA compositor, checks the initialized EGL/GL
vendor and DMA-BUF import-modifier extension, performs a Wayland roundtrip,
checks the output, shell and DMA-BUF protocol globals and an input seat with
both pointer and keyboard capabilities, then shuts down.
It validates compositor availability; only `workspace-wayland` validates the
application's native import, WebGPU/Vulkan, swapchain, presentation and shutdown.
The second exercises supervisor ownership, readiness, failure, deadline and
signal handling in the existing build image without GPU access.

`headless-compositor` also accepts a command after `--`; its default command
is `/usr/bin/true`. This is the wrapper entrypoint for diagnostics that need
the packaged runtime and private compositor:

```sh
./mmltk --test headless-compositor -- /usr/bin/wayland-info
```

Each compositor invocation records `weston.log`, `weston-stderr.log`,
`wayland-info.log`, and lifecycle `supervisor.jsonl` in a unique
`build/validation/headless-compositor-*` directory printed by the runner.
The acceptance executable retains its normal artifacts and assertions.
`./mmltk --logs build/validation --recursive -q headless` includes lifecycle
records; pass the printed directory to inspect one compositor run.

Weston uses a 1920×1080, scale-one virtual output and the desktop shell, with
locking, panel and idle blanking disabled. The process owns a private runtime
directory and socket; it does not connect to the host display or session bus.
Startup waits up to 30 seconds for Weston's `systemd-notify.so` READY datagram,
then up to 10 seconds for the protocol roundtrip. The supervised command has
no whole-executable deadline. The acceptance executable retains its own
[granular progress and settlement deadlines](validation.md#selection-environment-deadlines-and-debugging),
and the supervisor continues monitoring compositor death and interruption.
Owned process groups receive TERM at shutdown and KILL after a five-second
grace period. Linux process descriptors wait for surviving descendants even
when their group leader has exited. A second bounded wait follows KILL;
compositor loss, failed preflight and unclean shutdown fail the wrapper.

Headless containers inherit a one-byte soft and hard core limit. Linux uses
that exact value to suppress piped OS core handlers as well as ordinary core
files; a zero limit alone does not suppress piped handlers such as
systemd-coredump. This policy applies to the compositor, probe, acceptance
command and their descendants without changing the host's core configuration.
Firefox also retains its application-owned crash-reporter disable setting.
See the [Linux core-handler implementation](https://github.com/torvalds/linux/blob/master/fs/coredump.c).

NVIDIA's driver must support the surfaceless EGL platform, DMA-BUF imports and
modifiers exposed by Weston. The runner forces the NVIDIA EGL vendor and GL
renderer and fails if those capabilities are unavailable. It never selects
Pixman, a software renderer, X11 or Firefox's headless mode. Successful startup
does not prove that every exported native format/modifier is accepted: those
negotiations remain covered by the unchanged hardware assertions. A headless
output also does not exercise physical KMS scanout, monitor timing or the host
desktop compositor; retain visible runs when those are required.

## Validation input seat

Ubuntu Weston `13.0.0-4build3` is pinned for both the module builder's
`libweston-13-dev` and the validation runtime's `weston`/`libweston-13-0`.
[headless_weston_seat.cpp](../tools/headless_weston_seat.cpp) uses the packaged
public structures and four version-scoped exported lifecycle functions:
`weston_seat_init`, `weston_seat_init_pointer`, `weston_seat_init_keyboard`,
and `weston_seat_release`. Their declarations are private Weston API, so a
package update requires rechecking those signatures and lifetimes.

The module installs a `default` seat and releases it through the compositor's
destroy signal. Partial construction also releases initialized state. It loads
from `/usr/lib/weston/mmltk-headless-seat.so` before the readiness module.
[Dockerfile.wayland-validation](../docker/Dockerfile.wayland-validation) owns
the package pin and module build; [headless_compositor.py](../tools/headless_compositor.py)
owns session startup, seat/protocol readiness, and teardown.

This synthetic seat provides the capabilities required by Firefox/GTK and
the browser-driven acceptance workflows. It does not certify a physical input
device or host-desktop focus behavior. A successful compositor preflight
proves setup and teardown; the retained application scenarios still own the
interaction and image assertions.
