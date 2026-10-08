// Native counterpart of diagnose_native_symbols.py. Requires PCRE2-8 (-lpcre2-8)
// and the existing third_party/json/include headers; no Python runtime is used.
#include "cpp_support.hpp"
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace support = mmltk::tools;
namespace fs = std::filesystem;
using support::json;

struct Arguments {
    std::string match = ".";
    bool mangled = false;
    int limit = 200;
    std::vector<std::string> addresses;
    std::vector<std::string> artifacts;
};

void usage(std::ostream& stream, const std::string& program) {
    stream << "usage: " << program
           << " [-h] [--match MATCH] [--mangled] [--limit LIMIT]\n"
           << "       [--address ADDRESS] artifacts [artifacts ...]\n";
}

[[noreturn]] void argument_error(const std::string& program, const std::string& message) {
    usage(std::cerr, program);
    throw support::ToolError(program + ": error: " + message, 2);
}

Arguments parse(int argc, char** argv, const std::string& program) {
    Arguments args;
    bool positional = false;
    for (int i = 1; i < argc; ++i) {
        std::string option = argv[i];
        if (!positional && option == "--") { positional = true; continue; }
        if (positional || option.empty() || option.front() != '-' || option == "-") {
            args.artifacts.push_back(option);
            continue;
        }
        const auto equals = option.find('=');
        std::string name = option.substr(0, equals);
        if (name.starts_with("--")) {
            std::vector<std::string> matches;
            for (const std::string candidate : {"--help", "--match", "--mangled", "--limit", "--address"})
                if (candidate.starts_with(name)) matches.push_back(candidate);
            if (matches.size() == 1) name = matches.front();
            else if (matches.size() > 1)
                argument_error(program, "ambiguous option: " + name + " could match " + matches[0] + ", " + matches[1]);
        }
        if (name == "--help" || name == "-h") {
            if (equals != std::string::npos) argument_error(program, "argument " + name + ": ignored explicit argument");
            usage(std::cout, program);
            std::cout << "\nRead native symbols or resolve ELF addresses with the development image's tools.\n\n"
                      << "positional arguments:\n  artifacts          Repository-relative or installed /opt artifacts (.a/.o, or ELF with --address)\n\n"
                      << "options:\n  -h, --help         show this help message and exit\n"
                      << "  --match MATCH      Regular expression over demangled symbol records\n"
                      << "  --mangled          Keep mangled names for linker tracing\n"
                      << "  --limit LIMIT      Maximum emitted symbol records\n"
                      << "  --address ADDRESS  Resolve a hexadecimal address in one ELF artifact; repeatable\n";
            throw support::ToolError("", 0);
        }
        if (name == "--mangled") {
            if (equals != std::string::npos) argument_error(program, "argument --mangled: ignored explicit argument");
            args.mangled = true;
            continue;
        }
        if (name != "--match" && name != "--limit" && name != "--address")
            argument_error(program, "unrecognized arguments: " + option);
        std::string value;
        if (equals != std::string::npos) value = option.substr(equals + 1);
        else {
            if (++i == argc) argument_error(program, "argument " + name + ": expected one argument");
            value = argv[i];
            if (value.size() > 1 && value.front() == '-' &&
                (value[1] < '0' || value[1] > '9'))
                argument_error(program, "argument " + name + ": expected one argument");
        }
        if (name == "--match") args.match = value;
        else if (name == "--address") args.addresses.push_back(value);
        else {
            try {
                auto clean = support::trim(value);
                const std::size_t first = !clean.empty() && (clean.front() == '+' || clean.front() == '-') ? 1 : 0;
                if (first == clean.size()) throw std::invalid_argument("integer");
                for (std::size_t index = first; index < clean.size(); ++index) {
                    const char ch = clean[index];
                    if (ch >= '0' && ch <= '9') continue;
                    if (ch == '_' && index > first && index + 1 < clean.size() &&
                        clean[index - 1] >= '0' && clean[index - 1] <= '9' &&
                        clean[index + 1] >= '0' && clean[index + 1] <= '9') continue;
                    throw std::invalid_argument("integer");
                }
                std::erase(clean, '_');
                std::size_t end = 0;
                const auto number = std::stoll(clean, &end);
                if (end != clean.size()) throw std::invalid_argument("integer");
                // Preserve validation for arbitrarily large integer arguments.
                args.limit = number < 1 ? 0 : number > 10000 ? 10001 : static_cast<int>(number);
            } catch (const std::out_of_range&) {
                args.limit = 10001;
            } catch (const std::invalid_argument&) {
                argument_error(program, "argument --limit: invalid int value: '" + value + "'");
            }
        }
    }
    if (args.artifacts.empty()) argument_error(program, "the following arguments are required: artifacts");
    if (args.limit < 1 || args.limit > 10000) argument_error(program, "--limit must be between 1 and 10000");
    if (!args.addresses.empty() && (args.artifacts.size() != 1 || args.addresses.size() > static_cast<std::size_t>(args.limit)))
        argument_error(program, "--address requires one artifact and at most --limit addresses");
    for (std::string_view address : args.addresses) {
        if (address.starts_with("0x")) address.remove_prefix(2);
        if (address.empty() || address.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos)
            argument_error(program, "--address requires hexadecimal addresses");
    }
    return args;
}

class Pattern {
    std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)> code_{nullptr, pcre2_code_free};
    std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> data_{nullptr, pcre2_match_data_free};
public:
    Pattern(const std::string& expression, const std::string& program) {
        // Python's \Z is a strict end-of-string assertion; PCRE2 spells it \z.
        std::string translated;
        for (std::size_t i = 0; i < expression.size(); ++i) {
            translated += expression[i];
            if (expression[i] == '\\' && i + 1 < expression.size()) {
                const char escaped = expression[++i];
                translated += escaped == 'Z' ? 'z' : escaped;
            }
        }
        int error = 0;
        PCRE2_SIZE offset = 0;
        code_.reset(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(translated.data()), translated.size(),
                                 PCRE2_UTF | PCRE2_UCP, &error, &offset, nullptr));
        if (!code_) {
            PCRE2_UCHAR message[256]{};
            pcre2_get_error_message(error, message, sizeof(message));
            argument_error(program, std::string(reinterpret_cast<char*>(message)) + " at position " + std::to_string(offset));
        }
        data_.reset(pcre2_match_data_create_from_pattern(code_.get(), nullptr));
        if (!data_) throw std::bad_alloc();
    }
    bool search(std::string_view text) {
        const int result = pcre2_match(code_.get(), reinterpret_cast<PCRE2_SPTR>(text.data()),
                                       text.size(), 0, 0, data_.get(), nullptr);
        if (result < 0 && result != PCRE2_ERROR_NOMATCH)
            throw support::ToolError("regular expression matching failed: " + std::to_string(result));
        return result >= 0;
    }
};

// Match json.dumps' default ASCII escaping, insertion order and separators.
void emit(const json& record) {
    std::cout << '{';
    bool first = true;
    for (auto item = record.begin(); item != record.end(); ++item) {
        if (!first) std::cout << ", ";
        first = false;
        std::cout << json(item.key()).dump(-1, ' ', true) << ": " << item.value().dump(-1, ' ', true);
    }
    std::cout << "}\n";
}

bool within(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

int python_status(const support::ProcessResult& result) {
    return result.signal ? -result.signal : result.exit_code;
}

std::string strip_right(std::string line) {
    // Python str.rstrip includes Unicode whitespace in demangled names.
    std::size_t end = line.size();
    while (end) {
        auto begin = end - 1;
        while (begin && (static_cast<unsigned char>(line[begin]) & 0xc0U) == 0x80U) --begin;
        if (end - begin > 4) break;
        std::uint32_t point = static_cast<unsigned char>(line[begin]);
        if (end - begin > 1) {
            point &= (1U << (7 - (end - begin))) - 1;
            for (auto i = begin + 1; i < end; ++i) point = (point << 6) | (static_cast<unsigned char>(line[i]) & 0x3fU);
        }
        const bool whitespace = (point >= 9 && point <= 13) || (point >= 0x1c && point <= 0x20) ||
            point == 0x85 || point == 0xa0 || point == 0x1680 || (point >= 0x2000 && point <= 0x200a) ||
            point == 0x2028 || point == 0x2029 || point == 0x202f || point == 0x205f || point == 0x3000;
        if (!whitespace) break;
        end = begin;
    }
    line.resize(end);
    return line;
}

int diagnose(const Arguments& args, Pattern& pattern, const std::string& program) {
    std::vector<std::string> paths;
    for (const auto& artifact : args.artifacts) {
        const auto path = fs::canonical(fs::path("/workspace") / artifact);
        if ((!within(path, "/workspace") && !within(path, "/opt")) || !fs::is_regular_file(path))
            argument_error(program, "expected a repository or installed /opt file: " + artifact);
        if (!args.addresses.empty()) {
            std::ifstream input(path, std::ios::binary);
            if (!input) throw support::ToolError("cannot read " + path.string());
            char magic[4]{};
            input.read(magic, sizeof(magic));
            if (input.gcount() != 4 || std::string_view(magic, 4) != "\x7f" "ELF")
                argument_error(program, "expected an ELF artifact for --address: " + artifact);
        } else if (path.extension() != ".a" && path.extension() != ".o") {
            argument_error(program, "expected an .a or .o file: " + artifact);
        }
        paths.push_back(path.string());
    }
    if (!args.addresses.empty()) {
        std::vector<std::string> command{"addr2line", "-C", "-f", "-p", "-e", paths.front()};
        command.insert(command.end(), args.addresses.begin(), args.addresses.end());
        support::RunOptions options;
        options.timeout = std::chrono::seconds(110);
        const auto result = support::capture(command, options);
        if (result.exec_errno) throw support::ToolError(support::trim(result.err));
        if (result.timed_out) throw support::ToolError("addr2line timed out after 110 seconds");
        const auto lines = support::lines(result.out);
        for (std::size_t i = 0; i < std::min(lines.size(), args.addresses.size()); ++i)
            emit({{"owner", "native-symbols"}, {"event", "address"}, {"artifact", paths.front()},
                  {"address", args.addresses[i]}, {"message", lines[i]}});
        std::cerr << result.err;
        return python_status(result);
    }
    std::vector<std::string> command{"/opt/gcc-16.2/bin/gcc-nm", "-A"};
    if (!args.mangled) command.emplace_back("-C");
    command.insert(command.end(), paths.begin(), paths.end());
    std::uint64_t matched = 0;
    int emitted = 0;
    std::string pending;
    bool carriage_return = false;
    const auto line = [&] {
        if (pattern.search(pending)) {
            ++matched;
            if (emitted < args.limit) {
                emit({{"owner", "native-symbols"}, {"event", "symbol"}, {"message", strip_right(pending)}});
                ++emitted;
            }
        }
        pending.clear();
    };
    support::RunOptions options;
    options.stdout_sink = [&](std::string_view bytes) {
        for (char byte : bytes) {
            if (carriage_return && byte == '\n') { carriage_return = false; continue; }
            carriage_return = byte == '\r';
            pending += carriage_return ? '\n' : byte;
            if (byte == '\n' || carriage_return) line();
        }
    };
    const auto result = support::run(command, options);
    if (result.exec_errno) throw support::ToolError("could not execute " + command.front());
    if (!pending.empty()) line();
    const int status = python_status(result);
    emit({{"owner", "native-symbols"}, {"event", "summary"}, {"matched", matched},
          {"emitted", emitted}, {"exit_code", status}});
    return status;
}
} // namespace

int main(int argc, char** argv) {
    const std::string program = fs::path(argv[0]).filename().string();
    try {
        const auto args = parse(argc, argv, program);
        Pattern pattern(args.match, program);
        return diagnose(args, pattern, program);
    } catch (const support::ToolError& error) {
        if (*error.what()) std::cerr << error.what() << '\n';
        return error.status;
    } catch (const std::exception& error) {
        std::cerr << program << ": " << error.what() << '\n';
        return 1;
    }
}
