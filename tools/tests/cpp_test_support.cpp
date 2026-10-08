module;
#include <nlohmann/json.hpp>
#include <cstddef>
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <exception>
#include <iostream>
#include <regex>

export module mmltk.tools.tests.cpp_test_support;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
import mmltk.tools.tool_command;

export {
namespace mmltk::tools::testing {

struct Case { std::string_view name; void (*run)(); };

inline int run_cases(std::span<const Case> cases, int argc, char** argv) {
    const bool pattern = argc == 3 && std::string_view(argv[1]) == "--test-name-pattern";
    if (argc > 2 && !pattern) throw std::runtime_error("expected a case filter or --test-name-pattern PATTERN");
    const std::string_view filter = pattern ? argv[2] : argc > 1 ? argv[1] : "";
    const auto expression = pattern ? std::optional<std::regex>(std::string(filter)) : std::nullopt;
    const bool list = filter == "--list-tests";
    std::size_t selected = 0, failed = 0;
    for (const auto& test : cases) {
        if (list) { std::cout << test.name << '\n'; continue; }
        if (expression ? !std::regex_search(test.name.begin(), test.name.end(), *expression) : test.name.find(filter) == std::string_view::npos) continue;
        ++selected;
        try { test.run(); }
        catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    if (list) return 0;
    if (!selected && pattern) { std::cout << "0 cases selected\n"; return 0; }
    if (!selected) { std::cerr << "No matching tool cases\n"; return 2; }
    std::cout << selected - failed << '/' << selected << " cases passed\n";
    return failed == 0 ? 0 : 1;
}

// Fixtures for programs that accept only an executable path (PMD/Duplo) keep
// their logical argv[0] while entering the selected native suite in a child.
inline void write_fixture_program(const fs::path& path, command::Command selected) {
    write_text(path, "#!/bin/sh\nexec " + command::shell_invocation(selected, {}, path.filename().string()) + " \"$@\"\n");
    fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                    fs::perms::others_read | fs::perms::others_exec);
}

inline void require(bool condition, std::string_view expression, int line) {
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + std::string(expression));
}

enum class ErrorMatch { regex, substring };
inline void throws(const std::function<void()>& operation, std::string_view pattern = {},
                   ErrorMatch mode = ErrorMatch::regex) {
    try { operation(); }
    catch (const std::exception& error) {
        const bool matches = pattern.empty() || (mode == ErrorMatch::regex
            ? std::regex_search(error.what(), std::regex(std::string(pattern)))
            : std::string_view(error.what()).find(pattern) != std::string_view::npos);
        if (!matches) throw std::runtime_error("unexpected error: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("expected an exception");
}

inline json operator""_fixture(const char* source, std::size_t length) {
    return json::parse(std::string_view(source, length));
}

inline std::vector<std::string> detector_paths(const std::vector<std::string>& args) {
    const auto option = std::find(args.begin(), args.end(), "--file-list");
    if (option == args.end() || std::next(option) == args.end()) throw std::runtime_error("missing file list");
    std::ifstream list(*std::next(option));
    if (!list) throw std::runtime_error("cannot read detector file list");
    std::vector<std::string> result;
    std::string path;
    while (std::getline(list, path)) if (!path.empty()) result.push_back(path);
    return result;
}

} // namespace mmltk::tools::testing

}
