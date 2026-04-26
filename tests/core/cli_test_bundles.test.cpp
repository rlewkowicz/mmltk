#include "support/catch2_compat.hpp"
#include "support/subprocess_test_utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace {

using namespace mmltk::testsupport;
namespace fs = std::filesystem;

fs::path make_temp_dir(const char* label) {
    const fs::path dir =
        fs::temp_directory_path() / (std::string(label) + "-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code error;
    fs::create_directories(dir, error);
    assert(!error);
    return dir;
}

void cleanup_temp_dir(const fs::path& dir) {
    std::error_code error;
    fs::remove_all(dir, error);
}

std::vector<std::string> read_text_lines(const fs::path& path) {
    std::ifstream stream(path);
    assert(stream.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

fs::path locate_repo_wrapper_path() {
    auto find_repo_wrapper = [](fs::path current, const fs::path& cli_path) -> fs::path {
        for (int depth = 0; depth < 8; ++depth) {
            const fs::path candidate = current / "mmltk";
            std::error_code error;
            if (fs::is_regular_file(candidate, error) && !error && candidate != cli_path &&
                fs::exists(current / "CMakeLists.txt") && fs::exists(current / "README.md")) {
                return candidate;
            }
            if (!current.has_parent_path() || current.parent_path() == current) {
                break;
            }
            current = current.parent_path();
        }
        return {};
    };

    const fs::path cli_path = mmltk_cli_path();
    if (const char* repo_root = std::getenv("MMLTK_REPO_ROOT"); repo_root != nullptr && repo_root[0] != '\0') {
        const fs::path wrapper = find_repo_wrapper(fs::path(repo_root), cli_path);
        if (!wrapper.empty()) {
            return wrapper;
        }
    }

    {
        const fs::path wrapper = find_repo_wrapper(fs::current_path(), cli_path);
        if (!wrapper.empty()) {
            return wrapper;
        }
    }

    {
        const fs::path wrapper = find_repo_wrapper(cli_path.parent_path(), cli_path);
        if (!wrapper.empty()) {
            return wrapper;
        }
    }

    throw std::runtime_error("failed to locate repo-root mmltk wrapper");
}

fs::path write_fake_docker_script(const fs::path& root) {
    const fs::path bin_dir = root / "bin";
    const fs::path script_path = bin_dir / "docker";
    std::error_code error;
    fs::create_directories(bin_dir, error);
    assert(!error);

    std::ofstream stream(script_path, std::ios::trunc);
    assert(stream.is_open());
    stream << "#!/usr/bin/env bash\n"
              "set -euo pipefail\n"
              "state_dir=\"${MMLTK_FAKE_DOCKER_STATE:?}\"\n"
              "command=\"${1:-}\"\n"
              "shift || true\n"
              "case \"${command}\" in\n"
              "version)\n"
              "    exit 0\n"
              "    ;;\n"
              "image)\n"
              "    if [[ \"${1:-}\" == \"inspect\" ]]; then\n"
              "        if [[ \" ${*} \" == *\" --format \"* ]]; then\n"
              "            printf 'sha256:fake-image\\n'\n"
              "        fi\n"
              "        exit 0\n"
              "    fi\n"
              "    ;;\n"
              "container)\n"
              "    if [[ \"${1:-}\" == \"inspect\" ]]; then\n"
              "        exit 1\n"
              "    fi\n"
              "    ;;\n"
              "run)\n"
              "    printf '%s\\n' \"$@\" > \"${state_dir}/run_args.txt\"\n"
              "    exit 0\n"
              "    ;;\n"
              "exec)\n"
              "    count=0\n"
              "    if [[ -f \"${state_dir}/exec_count.txt\" ]]; then\n"
              "        count=\"$(cat \"${state_dir}/exec_count.txt\")\"\n"
              "    fi\n"
              "    count=$((count + 1))\n"
              "    printf '%s' \"${count}\" > \"${state_dir}/exec_count.txt\"\n"
              "    printf '%s\\n' \"$@\" > \"${state_dir}/exec_${count}_args.txt\"\n"
              "    exit 0\n"
              "    ;;\n"
              "start|rm|inspect)\n"
              "    exit 0\n"
              "    ;;\n"
              "esac\n"
              "printf 'unexpected docker command: %s\\n' \"${command}\" >&2\n"
              "exit 1\n";
    stream.close();
    fs::permissions(script_path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace, error);
    assert(!error);
    return script_path;
}

std::string prepend_path_env(const fs::path& prefix_dir) {
    std::string value = prefix_dir.string();
    if (const char* current_path = std::getenv("PATH"); current_path != nullptr && current_path[0] != '\0') {
        value += ":";
        value += current_path;
    }
    return value;
}

void assert_contains_line(const std::vector<std::string>& lines, const std::string& expected) {
    assert(std::find(lines.begin(), lines.end(), expected) != lines.end());
}

void assert_contains_substring(const std::vector<std::string>& lines, const std::string& expected) {
    const auto match = std::find_if(lines.begin(), lines.end(),
                                    [&](const std::string& line) { return line.find(expected) != std::string::npos; });
    assert(match != lines.end());
}

void test_top_level_help_lists_test_bundle_docs() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--help",
    });
    assert(result.exit_code == 0);
    assert(result.output_text.find("--test BUNDLE") != std::string::npos);
    assert(result.output_text.find("core") != std::string::npos);
    assert(result.output_text.find("gui") != std::string::npos);
    assert(result.output_text.find("all") != std::string::npos);
}

void test_test_list_lists_available_bundles() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--test",
        "list",
    });
    assert(result.exit_code == 0);
    assert(result.output_text.find("Bundled test bundles") != std::string::npos);
    assert(result.stdout_text.find("Bundled test bundles") != std::string::npos);
    assert(result.output_text.find("core") != std::string::npos);
    assert(result.output_text.find("gui") != std::string::npos);
    assert(result.output_text.find("all") != std::string::npos);
}

void test_unknown_test_bundle_reports_available_bundles() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--test",
        "missing",
    });
    assert(result.exit_code == 1);
    assert(result.output_text.find("unknown test bundle") != std::string::npos);
    assert(result.stdout_text.empty());
    assert(result.stderr_text.find("unknown test bundle") != std::string::npos);
    assert(result.stderr_text.find("Bundled test bundles") != std::string::npos);
    assert(result.output_text.find("core") != std::string::npos);
    assert(result.output_text.find("gui") != std::string::npos);
}

void test_core_test_bundle_forwards_catch2_args() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--test",
        "core",
        "--",
        "--list-tests",
    });
    assert(result.exit_code == 0);
    assert(result.output_text.find("test_roundtrip_end_to_end") != std::string::npos);
    assert(result.output_text.find("test_worker_pool_clamps_and_pins_threads") != std::string::npos);
}

void test_all_test_bundle_runs_all_available_suites() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--test",
        "all",
        "--",
        "--list-tests",
    });
    assert(result.exit_code == 0);
    assert(result.output_text.find("test_roundtrip_end_to_end") != std::string::npos);
    assert(result.output_text.find("test_env_fallback") != std::string::npos);
}

void test_subprocess_capture_keeps_stdout_and_stderr_separate() {
    const SubprocessResult result = run_subprocess_capture_output({
        "/bin/sh",
        "-c",
        "printf 'stdout-line\\n'; printf 'stderr-line\\n' >&2",
    });
    assert(result.exit_code == 0);
    assert(result.stdout_text == "stdout-line\n");
    assert(result.stderr_text == "stderr-line\n");
    assert(result.output_text.find("stdout-line\n") != std::string::npos);
    assert(result.output_text.find("stderr-line\n") != std::string::npos);
}

void test_log_file_flag_creates_requested_log_file() {
    const fs::path temp_dir = make_temp_dir("mmltk-log-file-flag");
    const fs::path log_path = temp_dir / "explicit.log";

    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--log-file",
        log_path.string(),
        "--help",
    });

    assert(result.exit_code == 0);
    assert(fs::exists(log_path));
    cleanup_temp_dir(temp_dir);
}

void test_log_dir_flag_creates_default_log_file() {
    const fs::path temp_dir = make_temp_dir("mmltk-log-dir-flag");

    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--log-dir",
        temp_dir.string(),
        "--help",
    });

    assert(result.exit_code == 0);
    assert(fs::exists(temp_dir / "mmltk.log"));
    cleanup_temp_dir(temp_dir);
}

void test_env_log_file_creates_requested_log_file() {
    const fs::path temp_dir = make_temp_dir("mmltk-log-file-env");
    const fs::path log_path = temp_dir / "env.log";

    const SubprocessResult result = run_subprocess_capture_output({
        "env",
        "MMLTK_LOG_FILE=" + log_path.string(),
        mmltk_cli_path(),
        "--help",
    });

    assert(result.exit_code == 0);
    assert(fs::exists(log_path));
    cleanup_temp_dir(temp_dir);
}

void test_env_log_dir_creates_default_log_file() {
    const fs::path temp_dir = make_temp_dir("mmltk-log-dir-env");

    const SubprocessResult result = run_subprocess_capture_output({
        "env",
        "MMLTK_LOG_DIR=" + temp_dir.string(),
        mmltk_cli_path(),
        "--help",
    });

    assert(result.exit_code == 0);
    assert(fs::exists(temp_dir / "mmltk.log"));
    cleanup_temp_dir(temp_dir);
}

void test_invalid_log_level_flag_reports_error_on_stderr() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--log-level",
        "banana",
        "--help",
    });
    assert(result.exit_code == 1);
    assert(result.stderr_text.find("invalid MMLTK log level: banana") != std::string::npos);
}

void test_invalid_log_level_env_reports_error_on_stderr() {
    const SubprocessResult result = run_subprocess_capture_output({
        "env",
        "MMLTK_LOG_LEVEL=banana",
        mmltk_cli_path(),
        "--help",
    });
    assert(result.exit_code == 1);
    assert(result.stderr_text.find("invalid MMLTK log level: banana") != std::string::npos);
}

void prepare_fake_docker_state(const fs::path& temp_dir, const fs::path& state_dir) {
    std::error_code error;
    fs::create_directories(state_dir, error);
    assert(!error);
    write_fake_docker_script(temp_dir);
}

void test_wrapper_env_logging_overrides_are_forwarded_to_docker_exec() {
    const fs::path temp_dir = make_temp_dir("mmltk-wrapper-log-env");
    const fs::path state_dir = temp_dir / "state";
    const fs::path log_path = temp_dir / "wrapper.log";
    const fs::path log_dir = temp_dir / "logs";
    prepare_fake_docker_state(temp_dir, state_dir);

    const SubprocessResult result = run_subprocess_capture_output({
        "env",
        "PATH=" + prepend_path_env(temp_dir / "bin"),
        "MMLTK_FAKE_DOCKER_STATE=" + state_dir.string(),
        "MMLTK_IMAGE=fake-mmltk",
        "MMLTK_LOG_LEVEL=debug",
        "MMLTK_LOG_FILE=" + log_path.string(),
        "MMLTK_LOG_DIR=" + log_dir.string(),
        locate_repo_wrapper_path().string(),
        "--help",
    });

    assert(result.exit_code == 0);
    const std::vector<std::string> exec_args = read_text_lines(state_dir / "exec_2_args.txt");
    assert_contains_line(exec_args, "-e");
    assert_contains_line(exec_args, "MMLTK_LOG_LEVEL=debug");
    assert_contains_line(exec_args, "MMLTK_LOG_FILE=/host" + log_path.string());
    assert_contains_line(exec_args, "MMLTK_LOG_DIR=/host" + log_dir.string());
    assert_contains_line(exec_args, "/opt/mmltk/bin/mmltk");
    assert_contains_line(exec_args, "--help");

    cleanup_temp_dir(temp_dir);
}

void test_wrapper_cli_logging_flags_are_forwarded_to_container_command() {
    const fs::path temp_dir = make_temp_dir("mmltk-wrapper-log-cli");
    const fs::path state_dir = temp_dir / "state";
    const fs::path log_path = temp_dir / "explicit.log";
    const fs::path log_dir = temp_dir / "logdir";
    prepare_fake_docker_state(temp_dir, state_dir);

    const SubprocessResult result = run_subprocess_capture_output({
        "env",
        "PATH=" + prepend_path_env(temp_dir / "bin"),
        "MMLTK_FAKE_DOCKER_STATE=" + state_dir.string(),
        "MMLTK_IMAGE=fake-mmltk",
        locate_repo_wrapper_path().string(),
        "--log-level",
        "trace",
        "--log-file",
        log_path.string(),
        "--log-dir",
        log_dir.string(),
        "--help",
    });

    assert(result.exit_code == 0);
    const std::vector<std::string> exec_args = read_text_lines(state_dir / "exec_2_args.txt");
    assert_contains_line(exec_args, "/opt/mmltk/bin/mmltk");
    assert_contains_line(exec_args, "--log-level");
    assert_contains_line(exec_args, "trace");
    assert_contains_line(exec_args, "--log-file");
    assert_contains_line(exec_args, "/host" + log_path.string());
    assert_contains_line(exec_args, "--log-dir");
    assert_contains_line(exec_args, "/host" + log_dir.string());
    assert_contains_line(exec_args, "--help");

    cleanup_temp_dir(temp_dir);
}

void test_wrapper_gui_tmpfs_uses_target_uid_gid() {
    const fs::path temp_dir = make_temp_dir("mmltk-wrapper-gui-tmpfs");
    const fs::path state_dir = temp_dir / "state";
    const fs::path runtime_dir = temp_dir / "runtime";
    const fs::path wayland_socket_path = runtime_dir / "wayland-0";
    std::error_code error;
    fs::create_directories(state_dir, error);
    assert(!error);
    fs::create_directories(runtime_dir, error);
    assert(!error);
    {
        std::ofstream stream(wayland_socket_path, std::ios::trunc);
        assert(stream.is_open());
        stream << "fake-wayland-socket";
    }
    write_fake_docker_script(temp_dir);

    const SubprocessResult result = run_subprocess_capture_output({
        "env",
        "PATH=" + prepend_path_env(temp_dir / "bin"),
        "MMLTK_FAKE_DOCKER_STATE=" + state_dir.string(),
        "MMLTK_IMAGE=fake-mmltk",
        "WAYLAND_DISPLAY=wayland-0",
        "XDG_RUNTIME_DIR=" + runtime_dir.string(),
        locate_repo_wrapper_path().string(),
        "--gui",
    });

    assert(result.exit_code == 0);
    const std::vector<std::string> run_args = read_text_lines(state_dir / "run_args.txt");
    const std::string expected_tmpfs =
        "/tmp/mmltk-gui-runtime:rw,mode=700,uid=" + std::to_string(::getuid()) + ",gid=" + std::to_string(::getgid());
    assert_contains_line(run_args, "--tmpfs");
    assert_contains_line(run_args, expected_tmpfs);
    assert_contains_substring(run_args, "com.mmltk.runtime=");
    assert_contains_substring(run_args, "tmpfs=" + expected_tmpfs);

    cleanup_temp_dir(temp_dir);
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[core][cli][test_bundles]", test_top_level_help_lists_test_bundle_docs);
MMLTK_REGISTER_TEST_CASE("[core][cli][test_bundles]", test_test_list_lists_available_bundles);
MMLTK_REGISTER_TEST_CASE("[core][cli][test_bundles]", test_unknown_test_bundle_reports_available_bundles);
MMLTK_REGISTER_TEST_CASE("[core][cli][test_bundles]", test_core_test_bundle_forwards_catch2_args);
MMLTK_REGISTER_TEST_CASE("[core][cli][test_bundles]", test_all_test_bundle_runs_all_available_suites);
MMLTK_REGISTER_TEST_CASE("[core][cli][subprocess]", test_subprocess_capture_keeps_stdout_and_stderr_separate);
MMLTK_REGISTER_TEST_CASE("[core][cli][logging]", test_log_file_flag_creates_requested_log_file);
MMLTK_REGISTER_TEST_CASE("[core][cli][logging]", test_log_dir_flag_creates_default_log_file);
MMLTK_REGISTER_TEST_CASE("[core][cli][logging]", test_env_log_file_creates_requested_log_file);
MMLTK_REGISTER_TEST_CASE("[core][cli][logging]", test_env_log_dir_creates_default_log_file);
MMLTK_REGISTER_TEST_CASE("[core][cli][logging]", test_invalid_log_level_flag_reports_error_on_stderr);
MMLTK_REGISTER_TEST_CASE("[core][cli][logging]", test_invalid_log_level_env_reports_error_on_stderr);
MMLTK_REGISTER_TEST_CASE("[core][cli][logging][wrapper]",
                         test_wrapper_env_logging_overrides_are_forwarded_to_docker_exec);
MMLTK_REGISTER_TEST_CASE("[core][cli][logging][wrapper]",
                         test_wrapper_cli_logging_flags_are_forwarded_to_container_command);
MMLTK_REGISTER_TEST_CASE("[core][cli][wrapper][gui]", test_wrapper_gui_tmpfs_uses_target_uid_gid);
