#include "cli_bundled_tests.h"

#include "gui/subprocess_utils.h"
#include "runtime_paths.h"
#include "mmltk_logging.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <string_view>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mmltk::cli_support {

namespace {

bool is_executable_file(const std::filesystem::path& path) {
    std::error_code status_error;
    const std::filesystem::file_status status = std::filesystem::status(path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status)) {
        return false;
    }
    return ::access(path.c_str(), X_OK) == 0;
}

const BundledTestBundle* find_bundled_test_bundle(const std::vector<BundledTestBundle>& bundles,
                                                  std::string_view bundle_name) {
    for (const auto& bundle : bundles) {
        if (std::string_view(bundle.spec->name) == bundle_name) {
            return &bundle;
        }
    }
    return nullptr;
}

int run_bundled_test_bundle(const BundledTestBundle& bundle, const std::vector<std::string>& forwarded_args,
                            const bool announce_bundle) {
    if (announce_bundle) {
        mmltk::logging::logger("cli")->info("mmltk --test: running bundle `{}` ({})", bundle.spec->name,
                                            bundle.executable_path.string());
    }

    std::vector<std::string> argv_strings;
    argv_strings.reserve(1 + forwarded_args.size());
    argv_strings.push_back(bundle.executable_path.string());
    argv_strings.insert(argv_strings.end(), forwarded_args.begin(), forwarded_args.end());

    std::vector<char*> raw_argv;
    raw_argv.reserve(argv_strings.size() + 1);
    for (std::string& arg : argv_strings) {
        raw_argv.push_back(arg.data());
    }
    raw_argv.push_back(nullptr);

    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        throw std::runtime_error(std::string("failed to fork bundled test runner: ") + std::strerror(errno));
    }
    if (child_pid == 0) {
        ::execv(raw_argv.front(), raw_argv.data());
        mmltk::logging::logger("cli")->error("execv({}) failed: {}", raw_argv.front(), std::strerror(errno));
        std::_Exit(127);
    }

    const int status = mmltk::gui::subprocess::wait_child_process(child_pid);
    if (status < 0) {
        throw std::runtime_error(std::string("failed waiting for bundled test runner: ") + std::strerror(errno));
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

}  // namespace

ParsedTestRequest parse_test_request(int argc, char** argv) {
    ParsedTestRequest request;
    if (argc < 2) {
        return request;
    }

    const std::string_view first_arg = argv[1];
    constexpr std::string_view test_prefix = "--test=";
    int next_index = 2;
    if (first_arg == "--test") {
        request.requested = true;
        if (argc >= 3) {
            request.bundle_name = argv[2];
            next_index = 3;
        }
    } else if (first_arg.starts_with(test_prefix)) {
        request.requested = true;
        request.bundle_name = std::string(first_arg.substr(test_prefix.size()));
        next_index = 2;
    } else {
        return request;
    }

    request.forwarded_args.reserve(static_cast<size_t>(argc - next_index));
    for (int index = next_index; index < argc; ++index) {
        request.forwarded_args.emplace_back(argv[index]);
    }
    if (!request.forwarded_args.empty() && request.forwarded_args.front() == "--") {
        request.forwarded_args.erase(request.forwarded_args.begin());
    }
    return request;
}

std::vector<BundledTestBundle> discover_bundled_test_bundles() {
    const std::filesystem::path executable_dir = runtime_paths::current_executable_path().parent_path();
    std::vector<BundledTestBundle> bundles;
    bundles.reserve(std::size(kBundledTestBundleSpecs));
    for (const auto& spec : kBundledTestBundleSpecs) {
        const std::filesystem::path executable_path = executable_dir / spec.executable_name;
        if (is_executable_file(executable_path)) {
            bundles.push_back(BundledTestBundle{
                &spec,
                executable_path,
            });
        }
    }
    return bundles;
}

std::string bundled_test_help_text(const std::vector<BundledTestBundle>& bundles) {
    std::string help_text =
        "tests:\n"
        "  run `mmltk --test <bundle> [-- <Catch2 args...>]` to execute bundled Catch2 suites\n"
        "  use `mmltk --test list` to print the bundles available beside this executable\n"
        "  example: `mmltk --test core -- --list-tests`\n";
    if (bundles.empty()) {
        help_text += "  bundled test bundles beside this executable: none\n";
        return help_text;
    }
    help_text += "  bundled test bundles beside this executable:\n";
    for (const auto& bundle : bundles) {
        help_text += "    ";
        help_text += bundle.spec->name;
        help_text += "  ";
        help_text += bundle.spec->description;
        help_text += "\n";
    }
    help_text += "    all  Run every available bundled Catch2 suite\n";
    return help_text;
}

void print_bundled_test_bundles(FILE* stream, const std::vector<BundledTestBundle>& bundles) {
    std::fprintf(stream, "Bundled test bundles\n");
    if (bundles.empty()) {
        std::fprintf(stream, "  none found beside this mmltk executable\n");
    } else {
        for (const auto& bundle : bundles) {
            std::fprintf(stream, "  %-6s %s\n", bundle.spec->name, bundle.spec->description);
        }
        std::fprintf(stream, "  %-6s %s\n", "all", "Run every available bundled Catch2 suite");
    }
    std::fprintf(stream,
                 "\nUse: mmltk --test <bundle> [-- <Catch2 args...>]\n"
                 "Example: mmltk --test core -- --list-tests\n");
}

int handle_bundled_test_request(const ParsedTestRequest& request, const std::vector<BundledTestBundle>& bundles) {
    if (request.bundle_name.empty()) {
        mmltk::logging::logger("cli")->error("mmltk --test requires a bundle name or `list`");
        print_bundled_test_bundles(stderr, bundles);
        return 1;
    }
    if (request.bundle_name == "list" || request.bundle_name == "help" || request.bundle_name == "--help" ||
        request.bundle_name == "-h") {
        print_bundled_test_bundles(stdout, bundles);
        return 0;
    }
    if (request.bundle_name == "all") {
        if (bundles.empty()) {
            mmltk::logging::logger("cli")->error(
                "mmltk --test: no bundled test suites were found beside this executable");
            return 1;
        }
        int first_failure = 0;
        for (size_t index = 0; index < bundles.size(); ++index) {
            const int exit_code =
                run_bundled_test_bundle(bundles[index], request.forwarded_args, bundles.size() > 1U || index > 0U);
            if (exit_code != 0 && first_failure == 0) {
                first_failure = exit_code;
            }
        }
        return first_failure;
    }

    const BundledTestBundle* bundle = find_bundled_test_bundle(bundles, request.bundle_name);
    if (bundle == nullptr) {
        mmltk::logging::logger("cli")->error("mmltk --test: unknown test bundle `{}`", request.bundle_name);
        print_bundled_test_bundles(stderr, bundles);
        return 1;
    }
    return run_bundled_test_bundle(*bundle, request.forwarded_args, false);
}

}  // namespace mmltk::cli_support
