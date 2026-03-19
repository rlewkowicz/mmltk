#include "fastloader/rfdetr/checkpoint.h"
#include "test_rfdetr_checkpoint_fixture_support.h"
#include "test_rfdetr_parity_fixture_support.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

using fastloader::rfdetr::testsupport::ParityFixtureCase;
using fastloader::rfdetr::testsupport::kParityFixtureHiddenDim;
using fastloader::rfdetr::testsupport::kParityFixtureNumClasses;
using fastloader::rfdetr::testsupport::log_fixture_phase;
using fastloader::rfdetr::testsupport::parity_fixture_cases;
using fastloader::rfdetr::testsupport::write_minimal_upstream_checkpoint;

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* name_prefix) {
        std::string pattern = (fs::temp_directory_path() / (std::string(name_prefix) + ".XXXXXX")).string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        }
        path_ = fs::path(created);
    }

    ~ScopedTempDir() {
        if (!path_.empty()) {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
    }

    const fs::path& path() const {
        return path_;
    }

private:
    fs::path path_;
};

fs::path fastloader_cli_path() {
#ifndef FASTLOADER_TEST_FASTLOADER_CLI_PATH
#error "FASTLOADER_TEST_FASTLOADER_CLI_PATH must be defined for test_rfdetr_native_integration"
#endif
    const fs::path cli_path = FASTLOADER_TEST_FASTLOADER_CLI_PATH;
    if (!fs::exists(cli_path)) {
        throw std::runtime_error("configured fastloader_cli path does not exist: " + cli_path.string());
    }
    return cli_path;
}

std::string command_string(const std::vector<std::string>& args) {
    std::string command;
    for (size_t index = 0; index < args.size(); ++index) {
        if (index > 0) {
            command.push_back(' ');
        }
        command += args[index];
    }
    return command;
}

void run_subprocess(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw std::runtime_error("run_subprocess requires at least one argument");
    }

    std::vector<char*> raw_args;
    raw_args.reserve(args.size() + 1);
    for (const auto& arg : args) {
        raw_args.push_back(const_cast<char*>(arg.c_str()));
    }
    raw_args.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        ::execv(raw_args.front(), raw_args.data());
        std::fprintf(stderr, "execv failed: %s\n", std::strerror(errno));
        std::_Exit(127);
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        throw std::runtime_error(std::string("waitpid failed: ") + std::strerror(errno));
    }
    const std::string command = command_string(args);
    if (WIFEXITED(status)) {
        const int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
            return;
        }
        if (exit_code == 127) {
            throw std::runtime_error("subprocess exec failed for command: " + command);
        }
        throw std::runtime_error(
            "subprocess exited with code " + std::to_string(exit_code) + ": " + command);
    }
    if (WIFSIGNALED(status)) {
        throw std::runtime_error(
            "subprocess terminated by signal " + std::to_string(WTERMSIG(status)) + ": " + command);
    }
    throw std::runtime_error("subprocess ended unexpectedly: " + command);
}

void assert_native_checkpoint(const fs::path& path, const char* expected_preset) {
    const bool path_exists = fs::exists(path);
    assert(path_exists);
    assert(fastloader::rfdetr::is_native_checkpoint_file(path));

    const auto checkpoint = fastloader::rfdetr::load_checkpoint(path);
    assert(checkpoint.metadata.preset_name == expected_preset);
    assert(checkpoint.metadata.source_kind == "upstream-python");
    assert(checkpoint.metadata.num_classes == kParityFixtureNumClasses);
    assert(!checkpoint.state_dict.empty());

    bool found_query_feat = false;
    bool found_class_embed = false;
    for (const auto& entry : checkpoint.state_dict) {
        if (entry.name == "query_feat.weight") {
            found_query_feat = true;
        }
        if (entry.name == "class_embed.weight") {
            found_class_embed = true;
            assert(entry.tensor.size(0) == kParityFixtureNumClasses);
            assert(entry.tensor.size(1) == kParityFixtureHiddenDim);
        }
    }
    assert(found_query_feat);
    assert(found_class_embed);
}

void test_native_rfdetr_cli_checkpoint_smoke() {
    const ScopedTempDir temp_dir("fastloader_rfdetr_native_integration");
    const fs::path& root = temp_dir.path();
    const fs::path cli_path = fastloader_cli_path();

    const auto& fixtures = parity_fixture_cases();
    for (size_t index = 0; index < fixtures.size(); ++index) {
        const auto& fixture = fixtures[index];
        log_fixture_phase(
            "test_rfdetr_native_integration",
            index + 1,
            fixtures.size(),
            "normalize",
            fixture.preset_name);
        const fs::path upstream_path = root / "weights" / fixture.upstream_filename;
        const fs::path native_path =
            root / "weights" / (std::string(fixture.preset_name) + ".native.pt");

        write_minimal_upstream_checkpoint(upstream_path, fixture);
        run_subprocess({
            cli_path.string(),
            "rfdetr",
            "normalize-weights",
            "--input",
            upstream_path.string(),
            "--output",
            native_path.string(),
        });
        assert_native_checkpoint(native_path, fixture.preset_name);
    }
}

} // namespace

int main() {
    try {
        test_native_rfdetr_cli_checkpoint_smoke();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_rfdetr_native_integration error: %s\n", error.what());
    } catch (...) {
        std::fputs("test_rfdetr_native_integration error: unknown exception\n", stderr);
    }
    return 1;
}
