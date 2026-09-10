"""Standard ownership and hardware-contract tests; no GPU required."""

import asyncio
import importlib.util
import json
import os
from pathlib import Path
import resource
import select
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "headless_compositor", Path(__file__).parents[1] / "headless_compositor.py",
)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

HARDWARE_LOG = (
    "EGL vendor: NVIDIA\nGL vendor: NVIDIA Corporation\n"
    "GL renderer: NVIDIA GeForce RTX\n    dmabuf support: modifiers\n"
)
PROTOCOLS = "\n".join(
    f"interface: '{name}', version: 4, name: 1"
    for name in ("wl_compositor", "wl_output", "xdg_wm_base", "zwp_linux_dmabuf_v1")
)
COMPOSITOR = """
import os, signal, socket, sys
from pathlib import Path
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
signal.signal(signal.SIGUSR1, lambda *_: sys.exit(73))
Path(os.environ["XDG_RUNTIME_DIR"], "compositor.pid").write_text(str(os.getpid()))
Path(sys.argv[1]).write_text(sys.argv[2])
if sys.argv[3] == "fail":
    sys.exit(19)
if sys.argv[3] != "silent":
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as ready:
        ready.sendto(b"READY=1", os.environ["NOTIFY_SOCKET"])
while True:
    signal.pause()
"""


class HardwareContractTests(unittest.TestCase):
    def test_wrapper_disables_os_core_reporting_for_fixture_children(self):
        self.assertEqual(resource.getrlimit(resource.RLIMIT_CORE), (1, 1))

    def test_fatal_signal_preserves_status_without_an_os_core_dump(self):
        pid = os.posix_spawn(
            sys.executable, (sys.executable, "-c", "import os; os.abort()"), os.environ,
        )
        _, status = os.waitpid(pid, 0)
        self.assertTrue(os.WIFSIGNALED(status))
        self.assertEqual(os.WTERMSIG(status), signal.SIGABRT)
        self.assertFalse(os.WCOREDUMP(status))

    def test_packaged_enumeration_bypasses_compositor_but_execution_owns_one(self):
        wrapper = (Path(__file__).parents[2] / "mmltk").read_text()
        function = wrapper.split("run_workspace_wayland_test_executable() {", 1)[1].split("\nrun_tests() {", 1)[0]
        setup = """
repo_root=/workspace
cache_root=/cache
container_cache_root=/cache
image_name=packaged
wayland_validation_image_name=validation
test_run_container_name=runner
test_headless_compositor=1
gui_runtime_dir_container=/runtime
gui_wayland_socket_container_path=/runtime/wayland-test
uid=1000
gid=1000
common_group_args=()
bind_mount_specs=()
docker() { return 0; }
prepare_owned_dirs() { :; }
prepare_workspace_wayland_test_mounts() { :; }
build_common_group_args() { :; }
gui_runtime_tmpfs_spec() { echo /runtime; }
append_execution_policy_args() { :; }
append_native_environment() { :; }
append_wrapper_labels() { :; }
append_native_logging_environment() { :; }
append_explicit_test_environment() { :; }
run_owned_docker_command() { printf '%s\\n' "$@"; }
"""
        for argument in ("--list-tests", "workspace_wayland_retained"):
            script = setup + "\nrun_workspace_wayland_test_executable() {" + function
            script += f"\nrun_workspace_wayland_test_executable /packaged/test {argument}\n"
            result = subprocess.run(("bash", "-c", script), check=True, capture_output=True, text=True)
            self.assertEqual("/workspace/tools/headless_compositor.py" in result.stdout, argument != "--list-tests")
            self.assertIn("/packaged/test", result.stdout)
            self.assertIn(argument, result.stdout)

    def test_requires_nvidia_vendor_and_modifier_support(self):
        self.assertEqual(runner.hardware_renderer(HARDWARE_LOG)["GL vendor"], "NVIDIA Corporation")
        for log in (
            HARDWARE_LOG.replace("EGL vendor: NVIDIA", "EGL vendor: Mesa"),
            HARDWARE_LOG.replace("NVIDIA GeForce RTX", "llvmpipe"),
            HARDWARE_LOG.replace("dmabuf support: modifiers", "dmabuf support: basic"),
            "",
        ):
            with self.subTest(log=log), self.assertRaises(runner.SessionFailure):
                runner.hardware_renderer(log)

    def test_requires_real_wayland_dmabuf_output_and_shell(self):
        runner.require_wayland_protocols(PROTOCOLS)
        for line in PROTOCOLS.splitlines():
            with self.subTest(line=line), self.assertRaises(runner.SessionFailure):
                runner.require_wayland_protocols(PROTOCOLS.replace(line, ""))

    def test_private_environment_owns_display_and_renderer(self):
        with patch.dict(os.environ, {
            "DISPLAY": ":0", "MOZ_HEADLESS": "1", "WAYLAND_SOCKET": "4",
            "DBUS_SESSION_BUS_ADDRESS": "host", "LIBGL_ALWAYS_SOFTWARE": "1",
            "__EGL_VENDOR_LIBRARY_FILENAMES": "mesa.json", "MMLTK_GUI_PIXEL_TRACE": "1",
        }):
            env = runner.session_environment("/tmp/private")
        self.assertEqual(env["XDG_RUNTIME_DIR"], "/tmp/private")
        self.assertEqual(env["MMLTK_GUI_PIXEL_TRACE"], "1")
        self.assertTrue(env["__EGL_VENDOR_LIBRARY_FILENAMES"].endswith("10_nvidia.json"))
        for name in ("DISPLAY", "MOZ_HEADLESS", "WAYLAND_SOCKET",
                     "DBUS_SESSION_BUS_ADDRESS", "LIBGL_ALWAYS_SOFTWARE"):
            self.assertNotIn(name, env)


class SessionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        self.mode = "ready"
        self.log = HARDWARE_LOG
        self.protocols = PROTOCOLS
        self.children = []
        self.original_start = asyncio.create_subprocess_exec

        async def start(*command, **kwargs):
            if command[0] == "/usr/bin/weston":
                log_path = next(value[6:] for value in command if value.startswith("--log="))
                command = (sys.executable, "-c", COMPOSITOR, log_path, self.log, self.mode)
            elif command[0] == "/usr/bin/wayland-info":
                command = (sys.executable, "-c", f"print({self.protocols!r})")
            process = await self.original_start(*command, **kwargs)
            self.children.append(process)
            return process

        self.patches = (
            patch.object(asyncio, "create_subprocess_exec", start),
            patch.dict(os.environ, XDG_RUNTIME_DIR=str(self.root)),
            patch.object(runner, "STARTUP_TIMEOUT", 0.3),
            patch.object(runner, "SHUTDOWN_TIMEOUT", 0.3),
        )
        for active in self.patches:
            active.start()

    async def asyncTearDown(self):
        for active in reversed(self.patches):
            active.stop()
        for process in self.children:
            self.assertIsNotNone(process.returncode, f"unreaped child {process.pid}")
        self.assertFalse(list(self.root.glob("headless-*")), "runtime directory leaked")
        self.temporary.cleanup()

    async def run_command(self, code):
        return await runner.run_session(
            (sys.executable, "-c", code), self.artifacts,
        )

    def events(self):
        return [json.loads(line) for line in (self.artifacts / "supervisor.jsonl").read_text().splitlines()]

    async def test_normal_completion_and_command_status(self):
        self.assertEqual(await self.run_command("raise SystemExit(7)"), 7)
        events = self.events()
        self.assertIn("headless.ready", [event["event"] for event in events])
        self.assertEqual(events[-2]["returncode"], 0)
        self.assertEqual(events[-1]["exit_code"], 7)

    async def test_command_death_propagates_without_a_session_deadline(self):
        code = "import os, signal; os.kill(os.getpid(), signal.SIGKILL)"
        self.assertEqual(await self.run_command(code), 137)
        self.assertEqual(self.events()[-1]["exit_code"], 137)

    async def test_success_requires_clean_compositor_shutdown(self):
        self.assertEqual(await self.run_command("raise SystemExit(0)"), 0)
        self.assertEqual(self.events()[-1]["exit_code"], 0)

    async def test_startup_failure_does_not_launch_client(self):
        self.mode = "fail"
        self.assertEqual(await self.run_command("raise SystemExit(0)"), 1)
        self.assertEqual(len(self.children), 1)
        self.assertIn("19", next(event["message"] for event in self.events()
                                 if event["event"] == "headless.failed"))

    async def test_missing_ready_times_out_and_reaps(self):
        self.mode = "silent"
        self.assertEqual(await self.run_command("raise SystemExit(0)"), 1)
        self.assertEqual(len(self.children), 1)

    async def test_software_renderer_prevents_client_start(self):
        self.log = HARDWARE_LOG.replace("NVIDIA GeForce RTX", "llvmpipe")
        self.assertEqual(await self.run_command("raise SystemExit(0)"), 1)
        self.assertEqual(len(self.children), 1)

    async def test_missing_dmabuf_prevents_acceptance_start(self):
        self.protocols = PROTOCOLS.replace("zwp_linux_dmabuf_v1", "missing_protocol")
        self.assertEqual(await self.run_command("raise SystemExit(0)"), 1)
        self.assertEqual(len(self.children), 2)

    async def test_compositor_loss_aborts_running_test(self):
        code = """
import os, signal
from pathlib import Path
pid = int(Path(os.environ["XDG_RUNTIME_DIR"], "compositor.pid").read_text())
os.kill(pid, signal.SIGUSR1)
signal.pause()
"""
        self.assertEqual(await self.run_command(code), 1)
        self.assertIn("73", next(event["message"] for event in self.events()
                                 if event["event"] == "headless.failed"))

    async def test_cancellation_terminates_child(self):
        code = "import os, signal; os.kill(os.getppid(), signal.SIGTERM); signal.pause()"
        self.assertEqual(await self.run_command(code), 143)
        self.assertEqual(self.children[-1].returncode, -signal.SIGTERM)

    async def test_stubborn_child_is_killed_after_grace_period(self):
        code = "import os, signal; signal.signal(signal.SIGTERM, signal.SIG_IGN); os.kill(os.getppid(), signal.SIGTERM); signal.pause()"
        self.assertEqual(await self.run_command(code), 143)
        self.assertEqual(self.children[-1].returncode, -signal.SIGKILL)
        self.assertIn("headless.cleanup.escalated", [event["event"] for event in self.events()])

    async def test_exited_leader_does_not_abandon_stubborn_descendant(self):
        await self.check_exited_leader_descendant(stubborn=True)

    async def test_exited_leader_allows_descendant_to_finish_on_term(self):
        await self.check_exited_leader_descendant(stubborn=False)

    async def check_exited_leader_descendant(self, *, stubborn):
        pid_file = self.artifacts / "descendant.pid"
        code = f"""
import os, signal
from pathlib import Path
read_fd, write_fd = os.pipe()
pid = os.fork()
if pid == 0:
    os.close(read_fd)
    signal.signal(signal.SIGTERM, {"signal.SIG_IGN" if stubborn else "lambda *_: os._exit(0)"})
    os.write(write_fd, b"ready")
    os.close(write_fd)
    signal.pause()
    os._exit(0)
os.close(write_fd)
os.read(read_fd, 5)
os.close(read_fd)
Path({str(pid_file)!r}).write_text(str(pid))
"""
        try:
            result = await self.run_command(code)
            self.assertEqual(result, int(stubborn))
            self.assertEqual("headless.cleanup.escalated" in
                             [event["event"] for event in self.events()], stubborn)
            pid = int(pid_file.read_text())
            try:
                descriptor = os.pidfd_open(pid)
            except ProcessLookupError:
                return
            try:
                self.assertTrue(select.select([descriptor], [], [], 0)[0],
                                "descendant is still running")
            finally:
                os.close(descriptor)
        finally:
            if pid_file.exists():
                try:
                    os.kill(int(pid_file.read_text()), signal.SIGKILL)
                except ProcessLookupError:
                    pass

    async def test_missing_command_cleans_started_compositor(self):
        self.assertEqual(await runner.run_session(
            ("/no/such/mmltk-test-command",), self.artifacts,
        ), 1)

    async def test_signal_is_propagated_and_all_children_retire(self):
        code = "import os, signal; os.kill(os.getppid(), signal.SIGTERM); signal.pause()"
        self.assertEqual(await self.run_command(code), 143)
        self.assertEqual(self.events()[-1]["exit_code"], 143)


if __name__ == "__main__":
    unittest.main()
