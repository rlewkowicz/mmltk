#!/usr/bin/env python3
"""Own a GPU-only Weston session and the packaged Wayland acceptance command."""

import argparse
import asyncio
import contextlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import sys
import tempfile
import time

STARTUP_TIMEOUT = 30
PROBE_TIMEOUT = 10
SHUTDOWN_TIMEOUT = 5


class SessionFailure(Exception):
    pass


def hardware_renderer(log):
    """Check the renderer actually initialized by Weston, not an EGL probe."""
    fields = {}
    for name in ("EGL vendor", "GL vendor", "GL renderer"):
        match = re.search(rf"\b{re.escape(name)}:\s*([^\n]+)", log)
        if not match:
            raise SessionFailure(f"Weston did not report {name}")
        fields[name] = match.group(1).strip()
    if not all("NVIDIA" in fields[name] for name in ("EGL vendor", "GL vendor")):
        raise SessionFailure(f"Weston requires NVIDIA EGL and GL: {fields}")
    if re.search(r"llvmpipe|softpipe|swrast|software", fields["GL renderer"], re.I):
        raise SessionFailure(f"Weston selected a software renderer: {fields}")
    if not re.search(r"\bdmabuf support:\s*modifiers\s*$", log, re.MULTILINE):
        raise SessionFailure("NVIDIA EGL does not expose DMA-BUF import modifiers")
    return fields


def require_wayland_protocols(report):
    for interface in ("wl_compositor", "wl_output", "xdg_wm_base", "zwp_linux_dmabuf_v1"):
        if not re.search(rf"interface:\s*'{interface}'", report):
            raise SessionFailure(f"headless compositor lacks {interface}")


def session_environment(runtime):
    environment = dict(os.environ)
    for name in (
        "DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "WAYLAND_SERVER_SOCKET",
        "DBUS_SESSION_BUS_ADDRESS", "MOZ_HEADLESS", "LIBGL_ALWAYS_SOFTWARE",
        "GALLIUM_DRIVER", "NOTIFY_SOCKET", "WATCHDOG_USEC", "WATCHDOG_PID",
        "LISTEN_FDS", "LISTEN_PID", "LISTEN_FDNAMES",
    ):
        environment.pop(name, None)
    environment.update(
        XDG_RUNTIME_DIR=str(runtime),
        XDG_SESSION_TYPE="wayland",
        WAYLAND_DISPLAY="wayland-mmltk",
        GDK_BACKEND="wayland",
        MOZ_ENABLE_WAYLAND="1",
        GBM_BACKEND="nvidia-drm",
        __EGL_VENDOR_LIBRARY_FILENAMES="/usr/share/glvnd/egl_vendor.d/10_nvidia.json",
    )
    return environment


def exit_status(returncode):
    return returncode if returncode >= 0 else 128 - returncode


async def wait_for_pidfd(descriptor):
    loop = asyncio.get_running_loop()
    exited = loop.create_future()

    def ready():
        loop.remove_reader(descriptor)
        if not exited.done():
            exited.set_result(None)

    loop.add_reader(descriptor, ready)
    try:
        await exited
    finally:
        loop.remove_reader(descriptor)


def group_pidfds(group, resources):
    """Pin the current live members, including children whose leader exited."""
    descriptors = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        pid = int(entry.name)
        with contextlib.suppress(ProcessLookupError, FileNotFoundError):
            if os.getpgid(pid) != group:
                continue
            # A zombie has released its resources; the container init reaps it.
            if (entry / "stat").read_text().rsplit(")", 1)[1].split()[0] == "Z":
                continue
            descriptor = os.pidfd_open(pid)
            resources.callback(os.close, descriptor)
            descriptors.append(descriptor)
    return descriptors


class HeadlessSession:
    def __init__(self, artifacts, environment, timeout):
        self.artifacts = artifacts
        self.environment = environment
        self.timeout = timeout
        self.compositor = None
        self.compositor_exit = None
        self.children = []
        self.stopped = asyncio.get_running_loop().create_future()
        self.events = (artifacts / "supervisor.jsonl").open("w", buffering=1)

    def event(self, event, **fields):
        self.events.write(json.dumps({
            "event": f"headless.{event}", "steady_ns": time.monotonic_ns(),
            "pid": os.getpid(), **fields,
        }) + "\n")

    async def start(self, command, **kwargs):
        process = await asyncio.create_subprocess_exec(
            *command, env=kwargs.pop("env", self.environment),
            start_new_session=True, **kwargs,
        )
        self.children.append(process)
        return process

    async def guarded(self, awaitable, timeout):
        task = asyncio.ensure_future(awaitable)
        try:
            done, _ = await asyncio.wait(
                (task, self.compositor_exit, self.stopped),
                timeout=timeout, return_when=asyncio.FIRST_COMPLETED,
            )
            if self.stopped in done:
                raise SessionFailure(f"session interrupted by signal {self.stopped.result()}")
            if self.compositor_exit in done:
                code = self.compositor_exit.result()
                raise SessionFailure(f"Weston exited unexpectedly with status {code}")
            if task not in done:
                raise SessionFailure(f"operation exceeded {timeout:g}s deadline")
            return task.result()
        finally:
            if not task.done():
                task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task

    async def ready(self, notification):
        loop = asyncio.get_running_loop()
        while True:
            message = await loop.sock_recv(notification, 4096)
            if b"READY=1" in message.splitlines():
                return

    async def run(self, command):
        runtime = Path(self.environment["XDG_RUNTIME_DIR"])
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as notification:
            notification.bind(str(runtime / "notify"))
            notification.setblocking(False)
            weston_environment = dict(self.environment, NOTIFY_SOCKET=str(runtime / "notify"))
            weston_log = self.artifacts / "weston.log"
            weston_command = (
                "/usr/bin/weston", "--backend=headless", "--renderer=gl",
                "--width=1920", "--height=1080", "--scale=1", "--idle-time=0",
                "--shell=desktop", "--modules=systemd-notify.so",
                f"--config={Path(__file__).with_name('headless-weston.ini')}",
                f"--socket={self.environment['WAYLAND_DISPLAY']}",
                f"--log={weston_log}",
            )
            self.event("starting", command=weston_command, test_command=command)
            with (self.artifacts / "weston-stderr.log").open("wb") as errors:
                self.compositor = await self.start(
                    weston_command, env=weston_environment, stdout=errors, stderr=errors,
                )
                self.compositor_exit = asyncio.create_task(self.compositor.wait())
                await self.guarded(self.ready(notification), STARTUP_TIMEOUT)
                renderer = hardware_renderer(weston_log.read_text(errors="replace"))
                # A real client roundtrip follows READY, proving the compositor's
                # event loop and required protocol globals are usable.
                with (self.artifacts / "wayland-info.log").open("w+b") as report:
                    probe = await self.start(
                        ("/usr/bin/wayland-info",), stdout=report, stderr=report,
                    )
                    code = await self.guarded(probe.wait(), PROBE_TIMEOUT)
                    if code:
                        raise SessionFailure(f"wayland-info exited with status {code}")
                    report.seek(0)
                    require_wayland_protocols(report.read().decode(errors="replace"))
                    self.children.remove(probe)
                self.event("ready", compositor_pid=self.compositor.pid, renderer=renderer)
                print(f"mmltk: headless NVIDIA compositor ready; artifacts: {self.artifacts}",
                      file=sys.stderr, flush=True)
                process = await self.start(command)
                self.event("test.started", child_pid=process.pid)
                code = await self.guarded(process.wait(), self.timeout)
                self.event("test.exited", child_pid=process.pid, returncode=code)
                return exit_status(code)

    async def stop_group(self, process):
        clean = True
        # A fresh snapshot after TERM also catches descendants created during a
        # parent's shutdown handler. PID descriptors wait on exit without polling.
        for number in (signal.SIGTERM, signal.SIGKILL):
            with contextlib.ExitStack() as resources:
                descriptors = group_pidfds(process.pid, resources)
                if number == signal.SIGKILL and not descriptors:
                    break
                if number == signal.SIGKILL:
                    clean = False
                    self.event("cleanup.escalated", child_pid=process.pid)
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, number)
                try:
                    await asyncio.wait_for(asyncio.gather(
                        process.wait(),
                        *(wait_for_pidfd(descriptor) for descriptor in descriptors),
                    ), SHUTDOWN_TIMEOUT)
                except asyncio.TimeoutError:
                    if number == signal.SIGKILL:
                        self.event("cleanup.failed", child_pid=process.pid,
                                   message="process group did not exit after SIGKILL")
                    clean = False
        return clean

    async def close(self):
        clean = True
        for process in reversed(self.children):
            was_running = process.returncode is None
            try:
                clean = await self.stop_group(process) and clean
            except OSError as error:
                clean = False
                self.event("cleanup.failed", child_pid=process.pid, message=str(error))
                # A failed procfs inspection must not strand the remaining owners.
                self.event("cleanup.escalated", child_pid=process.pid)
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGKILL)
                with contextlib.suppress(asyncio.TimeoutError):
                    await asyncio.wait_for(process.wait(), SHUTDOWN_TIMEOUT)
            if process is self.compositor:
                self.event("compositor.exited", returncode=process.returncode,
                           shutdown_requested=was_running)
                if not was_running or process.returncode != 0:
                    clean = False
        return clean


async def run_session(command, artifacts, timeout):
    with tempfile.TemporaryDirectory(prefix="headless-", dir=os.environ["XDG_RUNTIME_DIR"]) as runtime:
        session = HeadlessSession(artifacts, session_environment(runtime), timeout)
        loop = asyncio.get_running_loop()

        def stop(number):
            if not session.stopped.done():
                session.stopped.set_result(number)

        signals = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
        for number in signals:
            loop.add_signal_handler(number, stop, number)
        code = 1
        try:
            code = await session.run(command)
        except (SessionFailure, OSError) as error:
            session.event("failed", message=str(error))
            print(f"mmltk: headless compositor failed: {error}; artifacts: {artifacts}",
                  file=sys.stderr, flush=True)
        finally:
            if not await session.close() and code == 0:
                code = 1
            if session.stopped.done():
                code = 128 + session.stopped.result()
            session.event("finished", exit_code=code)
            session.events.close()
            for number in signals:
                loop.remove_signal_handler(number)
        return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    command = arguments.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("a command is required")
    try:
        value = os.environ.get("MMLTK_TEST_TIMEOUT_SECONDS")
        timeout = int(value) if value is not None else None
        if timeout is not None and timeout <= 0:
            raise ValueError
    except ValueError:
        parser.error("MMLTK_TEST_TIMEOUT_SECONDS must be a positive integer")
    root = Path(os.environ["MMLTK_REPO_ROOT"]) / "build" / "validation"
    root.mkdir(parents=True, exist_ok=True)
    artifacts = Path(tempfile.mkdtemp(prefix=f"headless-compositor-{os.getpid()}-", dir=root))
    return asyncio.run(run_session(command, artifacts, timeout))


if __name__ == "__main__":
    sys.exit(main())
