#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

struct SubprocessResult {
    int exit_code = -1;
    std::string stderr_text;
};

std::string fastloader_cli_path() {
#ifndef FASTLOADER_TEST_FASTLOADER_CLI_PATH
#error "FASTLOADER_TEST_FASTLOADER_CLI_PATH must be defined for test_rfdetr_cli_aliases"
#endif
    return FASTLOADER_TEST_FASTLOADER_CLI_PATH;
}

SubprocessResult run_subprocess_capture_stderr(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw std::runtime_error("run_subprocess_capture_stderr requires at least one argument");
    }

    int stderr_pipe[2];
    if (::pipe(stderr_pipe) != 0) {
        throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
    }

    std::vector<char*> raw_args;
    raw_args.reserve(args.size() + 1);
    for (const auto& arg : args) {
        raw_args.push_back(const_cast<char*>(arg.c_str()));
    }
    raw_args.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        ::close(stderr_pipe[0]);
        if (::dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            std::fprintf(stderr, "dup2 failed: %s\n", std::strerror(errno));
            std::_Exit(127);
        }
        ::close(stderr_pipe[1]);
        ::execv(raw_args.front(), raw_args.data());
        std::fprintf(stderr, "execv failed: %s\n", std::strerror(errno));
        std::_Exit(127);
    }

    ::close(stderr_pipe[1]);
    std::string stderr_text;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t bytes_read = ::read(stderr_pipe[0], buffer.data(), buffer.size());
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(stderr_pipe[0]);
            throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
        }
        stderr_text.append(buffer.data(), static_cast<size_t>(bytes_read));
    }
    ::close(stderr_pipe[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        throw std::runtime_error(std::string("waitpid failed: ") + std::strerror(errno));
    }
    if (!WIFEXITED(status)) {
        throw std::runtime_error("subprocess did not exit normally");
    }

    return SubprocessResult{
        WEXITSTATUS(status),
        std::move(stderr_text),
    };
}

void test_evaluate_aliases_require_compiled() {
    const std::string cli_path = fastloader_cli_path();
    for (const char* alias : {"evaluate", "eval", "val"}) {
        const SubprocessResult result = run_subprocess_capture_stderr({
            cli_path,
            "rfdetr",
            alias,
        });
        assert(result.exit_code == 1);
        assert(result.stderr_text.find("rfdetr evaluate requires --compiled") != std::string::npos);
    }
}

void test_validate_still_routes_to_validate() {
    const SubprocessResult result = run_subprocess_capture_stderr({
        fastloader_cli_path(),
        "rfdetr",
        "validate",
    });
    assert(result.exit_code == 1);
    assert(result.stderr_text.find("rfdetr validate requires --compiled") != std::string::npos);
}

} // namespace

int main() {
    try {
        test_evaluate_aliases_require_compiled();
        test_validate_still_routes_to_validate();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_rfdetr_cli_aliases error: %s\n", error.what());
    } catch (...) {
        std::fputs("test_rfdetr_cli_aliases error: unknown exception\n", stderr);
    }
    return 1;
}
