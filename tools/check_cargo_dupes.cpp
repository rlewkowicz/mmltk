module;

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
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

export module mmltk.tools.check_cargo_dupes;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;

namespace {

using mmltk::tools::json;

bool equals_zero(const json& value) {
    // Python's original comparison also accepts false and floating-point zero.
    return (value.is_boolean() && !value.get<bool>()) ||
           (value.is_number() && value == 0);
}

int check(int argc, char** argv) {
    std::filesystem::current_path(argc > 1 && argv[1][0] != '\0' ? argv[1] : ".");

    std::vector<std::string> command{"cargo", "dupes", "--format", "json",
                                     "--min-lines", "5", "stats"};
    mmltk::tools::RunOptions options;
    options.capture_stdout = true;
    auto stats = mmltk::tools::run(command, options);
    if (stats.exit_code != 0) return stats.exit_code;

    // Bash command substitution discards NUL bytes and trailing newlines.
    if (stats.out.find('\0') != std::string::npos) {
        std::cerr << "check_cargo_dupes: warning: ignored null byte in input\n";
        std::erase(stats.out, '\0');
    }
    while (!stats.out.empty() && stats.out.back() == '\n') stats.out.pop_back();
    std::cout << stats.out << '\n';
    std::cout.flush();
    if (!std::cout) throw mmltk::tools::ToolError("cannot write duplicate statistics");

    bool clean = false;
    try {
        const auto parsed = json::parse(stats.out);
        clean = equals_zero(parsed.at("exact_duplicate_groups")) &&
                equals_zero(parsed.at("near_duplicate_groups"));
    } catch (const json::exception& error) {
        std::cerr << "check_cargo_dupes: invalid duplicate statistics: " << error.what() << '\n';
    }
    if (clean) return 0;

    command.back() = "report";
    const auto report = mmltk::tools::run(command);
    return report.exit_code != 0 ? report.exit_code : 1;
}

} // namespace

export int mmltk_tool_check_cargo_dupes(int argc, char** argv) {
    try {
        return check(argc, argv);
    } catch (const mmltk::tools::ToolError& error) {
        std::cerr << "check_cargo_dupes: " << error.what() << '\n';
        return error.status;
    } catch (const std::exception& error) {
        std::cerr << "check_cargo_dupes: " << error.what() << '\n';
        return 1;
    }
}
