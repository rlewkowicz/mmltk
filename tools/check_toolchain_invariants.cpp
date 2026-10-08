module;
// Native counterpart of check_toolchain_invariants.py. Compiler evidence is
// consumed directly; no Python runtime or recorder process is required.

// A complete TOML parser preserves Cargo's quoted/dotted keys, inline tables,
// string escapes and whole-document syntax validation.
#include <toml++/toml.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>
#include <initializer_list>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

export module mmltk.tools.check_toolchain_invariants;
import mmltk.tools.cpp_support;
import mmltk.tools.tool_command;
import mmltk.tools.cpp_text;

namespace {
using namespace mmltk::tools;
using Arguments = std::vector<std::string>;
using Environment = std::map<std::string, std::string>;
using Names = std::set<std::string>;

const fs::path gcc_root("/opt/gcc-16.2");
const fs::path evidence_root("/usr/local/share/mmltk/compiler-evidence");
const Names cxx_suffixes{".cc", ".cpp", ".cxx", ".c++", ".C", ".cppm", ".ixx"};
const Names reflection_options{"-freflection", "-fno-reflection"};

void require(bool condition, const std::string& message) {
    if (!condition) throw ToolError(message);
}

bool truthy(const json& value) {
    if (value.is_null()) return false;
    if (value.is_string()) return !value.get_ref<const std::string&>().empty();
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number()) return value != 0;
    return !value.empty();
}

fs::path resolved(const fs::path& path) {
    return fs::weakly_canonical(fs::absolute(path.empty() ? fs::path(".") : path));
}

fs::path absolute(const fs::path& path, const fs::path& directory) {
    return resolved(path.is_absolute() ? path : directory / path);
}

std::string argument_list(const Arguments& values) {
    std::string result = "[";
    for (const auto& value : values) {
        if (result.size() > 1) result += ", ";
        result += mmltk::tools::text::byte_repr(value);
    }
    return result + ']';
}

std::string path_collection(const std::vector<fs::path>& values, bool set = false) {
    if (set && values.empty()) return "set()";
    std::string result = set ? "{" : "[";
    for (const auto& value : values) {
        if (result.size() > 1) result += ", ";
        result += "PosixPath(" + mmltk::tools::text::byte_repr(value.string()) + ')';
    }
    return result + (set ? '}' : ']');
}

using mmltk::tools::current_environment;

std::string lookup(const Environment& values, const std::string& name,
                   std::string fallback = {}) {
    const auto value = values.find(name);
    return value == values.end() ? std::move(fallback) : value->second;
}

fs::path executable(const std::string& name, const fs::path& directory,
                    const Environment& environment) {
    fs::path result;
    if (name.find('/') != std::string::npos) {
        result = absolute(name, directory);
    } else {
        // shutil.which resolves relative PATH components against the process
        // working directory, not the compile entry's working directory.
        const auto search_path = lookup(environment, "PATH", "/bin:/usr/bin");
        require(!search_path.empty(), "cannot resolve compiler/launcher " + name);
        for (const auto& part : split(search_path, ':')) {
            const auto candidate = fs::path(part) / name;
            if (fs::is_regular_file(candidate) && ::access(candidate.c_str(), X_OK) == 0) {
                result = resolved(candidate);
                break;
            }
        }
        require(!result.empty(), "cannot resolve compiler/launcher " + name);
    }
    require(fs::is_regular_file(result) && ::access(result.c_str(), X_OK) == 0,
            "compiler/launcher is not executable: " + result.string());
    return result;
}

Arguments expand_arguments(const Arguments& arguments, const fs::path& directory,
                           std::vector<fs::path> active = {}) {
    Arguments expanded;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        std::optional<Arguments> files;
        if (argument.starts_with('@')) {
            files = Arguments{argument.substr(1)};
        } else if (argument == "--options-file" || argument == "-optf") {
            ++index;
            require(index < arguments.size(), "missing value for " + argument);
            files = split(arguments[index], ',');
        } else if (argument.starts_with("--options-file=") || argument.starts_with("-optf=")) {
            files = split(argument.substr(argument.find('=') + 1), ',');
        }
        if (!files) {
            expanded.push_back(argument);
            continue;
        }
        for (const auto& name : *files) {
            const auto path = absolute(name, directory);
            require(active.size() < 32 && std::find(active.begin(), active.end(), path) == active.end(),
                    "recursive response file: " + path.string());
            require(fs::is_regular_file(path), "missing response file: " + path.string());
            active.push_back(path);
            auto nested = expand_arguments(shell_words(read_text(path)), directory, active);
            active.pop_back();
            expanded.insert(expanded.end(), std::make_move_iterator(nested.begin()),
                            std::make_move_iterator(nested.end()));
        }
    }
    return expanded;
}

struct CompilerCommand {
    fs::path compiler;
    Arguments arguments;
    fs::path directory;
    Environment environment;
    fs::path source;
};

CompilerCommand compiler_command(const json& entry) {
    CompilerCommand result;
    result.directory = resolved(entry.at("directory").get<std::string>());
    const auto found = entry.find("arguments");
    auto arguments = expand_arguments(
        found != entry.end() && truthy(*found)
            ? found->get<Arguments>() : shell_words(entry.at("command").get<std::string>()),
        result.directory);
    result.environment = current_environment();
    std::size_t index = 0;
    if (arguments.size() >= 3 && fs::path(arguments[0]).filename() == "cmake") {
        require(arguments[1] == "-E" && arguments[2] == "env", "unsupported CMake compiler launcher");
        index = 3;
    } else if (!arguments.empty() && fs::path(arguments[0]).filename() == "env") {
        index = 1;
    }
    static const std::regex assignment(R"([A-Za-z_][A-Za-z_0-9]*=[\s\S]*)");
    while (index < arguments.size()) {
        const auto& argument = arguments[index];
        if (argument == "--") {
            ++index;
            break;
        }
        if (argument == "-u" || argument == "--unset") {
            require(index + 1 < arguments.size(), "missing env unset name");
            result.environment.erase(arguments[index + 1]);
            index += 2;
        } else if (argument.starts_with("--unset=")) {
            result.environment.erase(argument.substr(argument.find('=') + 1));
            ++index;
        } else if (std::regex_match(argument, assignment)) {
            const auto equal = argument.find('=');
            result.environment[argument.substr(0, equal)] = argument.substr(equal + 1);
            ++index;
        } else break;
    }
    while (index < arguments.size()) {
        const auto basename = fs::path(arguments[index]).filename().string();
        if (basename == "ccache" || basename == "sccache") {
            executable(arguments[index++], result.directory, result.environment);
        } else if (index + 2 < arguments.size() && arguments[index + 1] == "--tool" &&
                   arguments[index + 2] == mmltk::tools::command::name(mmltk::tools::command::Command::module_safe_compiler_launcher)) {
            executable(arguments[index], result.directory, result.environment);
            index += 3;
            if (index < arguments.size() && arguments[index] == "--argv0") {
                require(index + 1 < arguments.size(), "missing launcher logical program name");
                index += 2;
            }
            require(index < arguments.size() && arguments[index] == "--", "missing native launcher delimiter");
            ++index;
        } else break;
    }
    require(index < arguments.size(), "missing compiler after launcher");
    result.compiler = executable(arguments[index], result.directory, result.environment);
    result.arguments.assign(std::make_move_iterator(arguments.begin() + static_cast<std::ptrdiff_t>(index + 1)),
                            std::make_move_iterator(arguments.end()));
    result.source = absolute(entry.at("file").get<std::string>(), result.directory);
    return result;
}

Environment cmake_cache(const fs::path& build_dir) {
    Environment result;
    const std::regex pattern(R"(([^#/:][^:]*):[^=]+=(.*))");
    for (const auto& line : lines(read_text(build_dir / "CMakeCache.txt"))) {
        std::smatch match;
        if (std::regex_search(line, match, pattern, std::regex_constants::match_continuous))
            result[match[1].str()] = match[2].str();
    }
    return result;
}

Arguments option_values(const Arguments& arguments, const Names& names) {
    Arguments values;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (names.contains(argument)) {
            ++index;
            require(index < arguments.size(), "missing value for " + argument);
            values.push_back(arguments[index]);
        } else {
            for (const auto& name : names) {
                if (argument.starts_with(name + '=')) {
                    values.push_back(argument.substr(name.size() + 1));
                    break;
                }
            }
        }
    }
    return values;
}

json payload_manifest() {
    // The source location supports a counterpart built away from tools/. An
    // installed executable uses the same image manifest as the Python checker.
    for (const auto& base : {resolved(__FILE__).parent_path().parent_path(),
                             executable_path().parent_path().parent_path()}) {
        const auto path = base / "docker/nvidia-payload.json";
        if (fs::is_regular_file(path)) return read_json(path);
    }
    return read_json("/usr/share/mmltk/nvidia-payload.json").at("manifest");
}

class CompilerPolicy {
public:
    explicit CompilerPolicy(const json& manifest)
        : cuda_root_(manifest.at("cuda_root").get<std::string>()),
          cuda_version_(manifest.at("versions").at("cuda").get<std::string>()) {
        const auto last_dot = cuda_version_.rfind('.');
        if (last_dot != std::string::npos) cuda_version_.resize(last_dot);
    }

    void gcc(const fs::path& compiler, const std::string& language) {
        const auto expected = gcc_root / "bin" / (language == "C" ? "gcc" : "g++");
        require(compiler == resolved(expected),
                language + " requires " + expected.string() + ", found " + compiler.string());
        require(version(compiler) == "16.2.0", compiler.string() + " is not GCC 16.2.0");
    }

    void nvcc(const fs::path& compiler) {
        require(compiler == resolved(cuda_root_ / "bin/nvcc"),
                "CUDA requires " + cuda_root_.string() + "/bin/nvcc, found " + compiler.string());
        const auto& text = version(compiler, true);
        const std::string needle = "release " + cuda_version_ + ',';
        bool release = false;
        for (auto offset = text.find(needle); offset != std::string::npos; offset = text.find(needle, offset + 1)) {
            using mmltk::tools::text::word;
            if (offset == 0 || !word(static_cast<unsigned char>(text[offset - 1]))) { release = true; break; }
        }
        require(release, compiler.string() + " is not NVCC " + cuda_version_);
    }

    void cache(const fs::path& build_dir) {
        const auto values = cmake_cache(build_dir);
        for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
                 {"CMAKE_CUDA_STANDARD", "23"}, {"CMAKE_CUDA_STANDARD_REQUIRED", "ON"},
                 {"CMAKE_CUDA_EXTENSIONS", "OFF"}, {"MMLTK_CUDA_VERSION", cuda_version_}})
            require(lookup(values, name) == value,
                    "configured " + name + " must be " + value + ", found " + lookup(values, name, "None"));
        for (const std::string name : {"CUDAToolkit_ROOT", "MMLTK_CUDA_TOOLKIT_ROOT"})
            require(absolute(lookup(values, name), build_dir) == resolved(cuda_root_),
                    "configured " + name + " must resolve to " + cuda_root_.string());
        const auto environment = current_environment();
        for (const auto& [name, language] : std::vector<std::pair<std::string, std::string>>{
                 {"CMAKE_C_COMPILER", "C"}, {"CMAKE_CXX_COMPILER", "CXX"}, {"CMAKE_CUDA_HOST_COMPILER", "CXX"}})
            gcc(executable(lookup(values, name), build_dir, environment), language);
        nvcc(executable(lookup(values, "CMAKE_CUDA_COMPILER"), build_dir, environment));
        if (values.contains("CMAKE_ASM_NASM_COMPILER")) {
            nasm_compiler_ = executable(values.at("CMAKE_ASM_NASM_COMPILER"), build_dir, environment);
            require(*nasm_compiler_ == resolved("/usr/bin/nasm"),
                    "ASM_NASM requires /usr/bin/nasm, found " + nasm_compiler_->string());
        }
        require(mmltk::tools::includes(shell_words(lookup(values, "CMAKE_CUDA_FLAGS")), "--allow-unsupported-compiler"),
                "configured CUDA flags lack --allow-unsupported-compiler");
    }

    std::string command(const json& entry, const fs::path& repo_root,
                        const Names& first_party_targets = {}, std::string_view dependency = {}) {
        const auto [compiler, arguments, directory, environment, source] = compiler_command(entry);
        const auto suffix = source.extension().string();
        const auto standards = option_values(arguments, {"-std", "--std"});
        if (suffix == ".cu" || compiler.filename() == "nvcc") {
            require(cxx_suffixes.contains(suffix) || suffix == ".c" || suffix == ".cu",
                    "unsupported CUDA compilation category: " + source.string());
            nvcc(compiler);
            for (const std::string name : {"NVCC_PREPEND_FLAGS", "NVCC_APPEND_FLAGS", "NVCC_CCBIN"})
                require(lookup(environment, name).empty(), "ambient " + name + " obscures configured CUDA policy");
            const auto cpp23 = [](const std::string& value) { return value == "c++23"; };
            require(!standards.empty() && std::all_of(standards.begin(), standards.end(), cpp23),
                    "CUDA requires only -std=c++23, found " + argument_list(standards));
            require(mmltk::tools::includes(arguments, "--allow-unsupported-compiler") || mmltk::tools::includes(arguments, "-allow-unsupported-compiler"),
                    "CUDA command lacks unsupported-host override");
            const auto hosts = option_values(arguments, {"-ccbin", "--compiler-bindir"});
            require(!hosts.empty(), "CUDA command lacks explicit host compiler");
            for (auto host : hosts) {
                auto path = absolute(host, directory);
                if (fs::is_directory(path)) host = (path / "g++").string();
                gcc(executable(host, directory, environment), "CXX");
            }
            Arguments forwarded;
            for (const auto& value : option_values(arguments, {"-Xcompiler", "--compiler-options"})) {
                for (const auto& part : shell_words(value)) {
                    auto flags = split(part, ',');
                    forwarded.insert(forwarded.end(), std::make_move_iterator(flags.begin()),
                                     std::make_move_iterator(flags.end()));
                }
            }
            forwarded = expand_arguments(forwarded, directory);
            const auto reflection = [](const std::string& flag) {
                return reflection_options.contains(flag.substr(0, flag.find('=')));
            };
            require(!std::any_of(arguments.begin(), arguments.end(), reflection) &&
                    !std::any_of(forwarded.begin(), forwarded.end(), reflection),
                    "CUDA must not receive reflection flags");
            require(!mmltk::tools::includes(arguments, "-ansi") && !mmltk::tools::includes(forwarded, "-ansi"),
                    "CUDA must not override C++23 with -ansi");
            const auto host_standards = option_values(forwarded, {"-std", "--std"});
            require(std::all_of(host_standards.begin(), host_standards.end(), cpp23),
                    "CUDA forwards a conflicting host dialect: " + argument_list(host_standards));
            return "CUDA";
        }
        if (suffix == ".asm") {
            require(mmltk::tools::path_within(source, repo_root / "third_party/rapidgzip/src/external/isa-l"),
                    "unsupported assembly source: " + source.string());
            require(nasm_compiler_.has_value() && compiler == *nasm_compiler_,
                    "ISA-L assembly requires configured NASM, found " + compiler.string());
            return "ASM_NASM";
        }
        require(cxx_suffixes.contains(suffix) || suffix == ".c",
                "unsupported compilation category: " + source.string());
        const std::string language = cxx_suffixes.contains(suffix) ? "CXX" : "C";
        gcc(compiler, language);
        bool first_party = mmltk::tools::path_within(source, repo_root) && !mmltk::tools::path_within(source, repo_root / "third_party");
        auto outputs = option_values(arguments, {"-o", "--output-file"});
        if (entry.contains("output") && truthy(entry.at("output")))
            outputs.push_back(entry.at("output").get<std::string>());
        for (const auto& output : outputs) {
            const auto path = mmltk::tools::lexical_path(output, ".", false);
            for (auto component = path.begin(); component != path.end(); ++component) {
                auto next = std::next(component);
                if (*component == "CMakeFiles" && next != path.end() && first_party_targets.contains(next->string()))
                    first_party = true;
            }
        }
        if (language == "CXX" && (first_party || dependency == "onnx" || dependency == "simdjson")) {
            const auto owner = first_party ? std::string("first-party C++") : std::string(dependency) + " C++";
            require(!standards.empty() && (standards.back() == "c++26" || standards.back() == "c++2c"),
                    owner + " requires C++26 without extensions, found " + argument_list(standards));
            require(!mmltk::tools::includes(arguments, "-ansi"), owner + " must not override C++26 with -ansi");
        }
        if (language == "CXX" && first_party) {
            const auto last = std::find_if(arguments.rbegin(), arguments.rend(),
                                          [](const auto& flag) { return reflection_options.contains(flag); });
            require(last != arguments.rend() && *last == "-freflection",
                    "first-party C++ requires effective -freflection");
        }
        return language;
    }

private:
    const std::string& version(const fs::path& compiler, bool cuda = false) {
        const auto key = std::make_pair(compiler, cuda);
        if (const auto found = versions_.find(key); found != versions_.end()) return found->second;
        Arguments arguments{compiler.string()};
        if (cuda) arguments.emplace_back("--version");
        else { arguments.emplace_back("-dumpfullversion"); arguments.emplace_back("-dumpversion"); }
        RunOptions options;
        options.merge_stderr = true;
        const auto result = capture(arguments, options);
        if (result.exec_errno)
            throw ToolError(compiler.string() + ": " + std::strerror(result.exec_errno));
        require(result.exit_code == 0,
                "Command '" + argument_list(arguments) + "' returned non-zero exit status " +
                std::to_string(result.signal ? -result.signal : result.exit_code) + '.');
        return versions_.emplace(key, trim(result.out)).first->second;
    }

    fs::path cuda_root_;
    std::string cuda_version_;
    std::optional<fs::path> nasm_compiler_;
    std::map<std::pair<fs::path, bool>, std::string> versions_;
};

void check_dependency_evidence(CompilerPolicy& policy) {
    for (const std::string name : {"cargo-tools", "simdjson", "onnx", "cppcheck"}) {
        const auto path = evidence_root / (name + ".json");
        const auto evidence = read_json(path);
        require(evidence.at("gcc_version") == "16.2.0", "wrong compiler evidence: " + path.string());
        for (const std::string language : {"C", "CXX"})
            policy.gcc(evidence.at("configured_compilers").at(language).get<std::string>(), language);
        if (name != "cargo-tools") {
            const auto& commands = evidence.at("compile_commands");
            require(truthy(commands), "empty dependency evidence: " + path.string());
            for (const auto& entry : commands) policy.command(entry, "/workspace", {}, name);
        }
    }
}

void check_dependency_declarations(CompilerPolicy& policy, const fs::path& repo_root) {
    const std::regex gcc14(R"(\b(?:gcc|g\+\+|cc|c\+\+)-14\b)");
    const auto docker = repo_root / "docker";
    if (fs::is_directory(docker)) {
        for (const auto& entry : fs::directory_iterator(docker)) {
            const auto path = entry.path();
            if (!path.filename().string().starts_with("Dockerfile") || path.filename() == "Dockerfile.gcc") continue;
            std::size_t number = 0;
            for (const auto& line : lines(read_text(path))) {
                ++number;
                if (trim(line).starts_with('#')) continue;
                require(!std::regex_search(line, gcc14),
                        "GCC 14 outside bootstrap: " + path.string() + ':' + std::to_string(number));
            }
        }
    }
    const auto cargo_config = repo_root / "src/frontend/iced/.cargo/config.toml";
    const auto cargo = toml::parse_file(cargo_config.string());
    const auto linker = cargo["target"]["x86_64-unknown-linux-gnu"]["linker"].value<std::string>();
    require(linker.has_value(), cargo_config.string() + ": missing or non-string Linux linker");
    try {
        policy.gcc(executable(*linker, cargo_config.parent_path(), current_environment()), "C");
    } catch (const std::exception& error) {
        throw ToolError(cargo_config.string() + ": Linux linker=" + mmltk::tools::text::byte_repr(*linker) + ": " + error.what());
    }
}

Arguments pch_environment(const Arguments& arguments, const fs::path& source, const fs::path& directory) {
    const Names paired{"-o", "--output-file", "-MF", "-MT", "-MQ", "-x", "-include"};
    const Names mechanics{"-c", "-MD", "-MMD", "-MP", "-fpch-preprocess", "-fpch-instantiate-templates", "-Winvalid-pch"};
    Arguments result;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (paired.contains(argument)) { ++index; continue; }
        const bool is_source = !argument.starts_with('-') && absolute(argument, directory) == source;
        if (!mechanics.contains(argument) && !is_source) result.push_back(argument);
    }
    return result;
}

struct PchSignature {
    fs::path compiler;
    Arguments options;
    Environment environment;
    bool operator==(const PchSignature&) const = default;
};

struct PchOwner {
    fs::path owner;
    std::vector<std::pair<fs::path, PchSignature>> creations;
    std::map<fs::path, PchSignature> uses;
};

void check_precompiled_headers(const json& entries, const Names& first_party_targets) {
    std::vector<PchOwner> owners;
    std::map<fs::path, std::size_t> owner_indices;
    for (const auto& entry : entries) {
        const auto [compiler, arguments, directory, environment, source] = compiler_command(entry);
        const auto outputs = option_values(arguments, {"-o", "--output-file"});
        const auto output_entry = entry.find("output");
        const bool explicit_output = output_entry != entry.end() && truthy(*output_entry);
        require(explicit_output || !outputs.empty(), "missing compile output: " + source.string());
        const auto output = absolute(explicit_output ? output_entry->get<std::string>() : outputs.back(), directory);
        std::optional<fs::path> owner;
        for (auto parent = output.parent_path(); !parent.empty();) {
            if (parent.filename().string().ends_with(".dir") && parent.parent_path().filename() == "CMakeFiles") {
                owner = parent;
                break;
            }
            const auto next = parent.parent_path();
            if (next == parent) break;
            parent = next;
        }
        if (!owner || !first_party_targets.contains(owner->filename().string())) continue;
        const auto includes = option_values(arguments, {"-include", "-include-pch", "-imacros"});
        std::vector<fs::path> pch_includes;
        for (const auto& include : includes)
            if (fs::path(include).filename().string().starts_with("cmake_pch."))
                pch_includes.push_back(absolute(include, directory));
        const bool creation = output.extension() == ".gch";
        const auto policy_path = *owner / "mmltk-pch-policy.txt";
        if (std::find(source.begin(), source.end(), fs::path("header-isolation")) != source.end())
            require(includes.empty() && !creation,
                    "header isolation must remain unforced and PCH-free: " + source.string());
        const auto suffix = source.extension().string();
        const bool excluded = suffix == ".c" || suffix == ".cu" || suffix == ".cppm" || suffix == ".ixx" ||
            std::any_of(arguments.begin(), arguments.end(), [](const auto& argument) {
                return argument.starts_with("-fmodules") || argument.starts_with("-fmodule-mapper");
            });
        require(!excluded || (!creation && pch_includes.empty()),
                "excluded compilation received a PCH: " + source.string());
        if (!fs::is_regular_file(policy_path)) {
            require(!creation && pch_includes.empty(), "PCH without an owning registration: " + source.string());
            continue;
        }
        const auto [position, inserted] = owner_indices.emplace(*owner, owners.size());
        if (inserted) owners.push_back(PchOwner{*owner, {}, {}});
        auto& state = owners[position->second];
        if (!creation && pch_includes.empty()) continue;
        require(pch_includes.size() == 1,
                "expected one target-local PCH include for " + source.string() + ": " + path_collection(pch_includes));
        const auto& header = pch_includes.front();
        require(header.parent_path() == *owner && header.filename() == "cmake_pch.hxx",
                "cross-target or unexpected PCH artifact: " + header.string());
        require(mmltk::tools::includes(arguments, "-Werror=invalid-pch") && mmltk::tools::includes(arguments, "-Winvalid-pch") &&
                !mmltk::tools::includes(arguments, "-Wno-invalid-pch") && !mmltk::tools::includes(arguments, "-Wno-error=invalid-pch"),
                "invalid-PCH diagnostics must be fatal: " + source.string());
        PchSignature signature{compiler, pch_environment(arguments, source, directory), environment};
        if (creation) {
            require(source.filename() == "cmake_pch.hxx.cxx" && output == fs::path(header.string() + ".gch") &&
                    option_values(arguments, {"-x"}) == Arguments{"c++-header"},
                    "unexpected PCH creation command: " + source.string());
            state.creations.emplace_back(header, std::move(signature));
        } else {
            require(!state.uses.contains(source), "duplicate PCH use: " + source.string());
            state.uses.emplace(source, std::move(signature));
        }
    }
    for (const auto& state : owners) {
        std::set<fs::path> expected, excluded;
        std::vector<fs::path> headers;
        for (const auto& line : lines(read_text(state.owner / "mmltk-pch-policy.txt"))) {
            const auto tab = line.find('\t');
            require(tab != std::string::npos, "not enough values to unpack (expected 2, got 1)");
            const auto kind = line.substr(0, tab);
            const auto value = mmltk::tools::lexical_path(line.substr(tab + 1), ".", false);
            if (kind == "use") expected.insert(value);
            else if (kind == "skip") excluded.insert(value);
            else if (kind == "header") headers.push_back(value);
        }
        require(state.creations.size() == 1, "expected one PCH creation for " + state.owner.string());
        const auto& [header, signature] = state.creations.front();
        std::vector<fs::path> missing, extra;
        for (const auto& path : expected) if (!state.uses.contains(path)) missing.push_back(path);
        for (const auto& [path, value] : state.uses) if (!expected.contains(path)) extra.push_back(path);
        require(missing.empty() && extra.empty() && expected.size() >= 2,
                "PCH consumption differs from registration for " + state.owner.string() + ": missing=" +
                path_collection(missing, true) + ", extra=" + path_collection(extra, true));
        require(std::none_of(excluded.begin(), excluded.end(), [&](const auto& path) { return state.uses.contains(path); }),
                "excluded source consumes a PCH for " + state.owner.string());
        require(std::all_of(state.uses.begin(), state.uses.end(), [&](const auto& use) { return use.second == signature; }),
                "PCH compiler, options, definitions or environment differ for " + state.owner.string());
        const auto text = read_text(header);
        const std::regex include(R"pch(^\s*#include "([^"]+)")pch", std::regex_constants::ECMAScript | std::regex_constants::multiline);
        std::vector<fs::path> actual_headers;
        for (auto match = std::sregex_iterator(text.begin(), text.end(), include); match != std::sregex_iterator(); ++match)
            actual_headers.push_back(mmltk::tools::lexical_path((*match)[1].str(), ".", false));
        require(actual_headers == headers, "generated PCH groups differ from registration for " + state.owner.string());
    }
}

struct Options {
    std::optional<fs::path> build_dir;
    std::optional<fs::path> repo_root;
    bool dependencies_only = false;
    bool help = false;
};

void usage(std::ostream& output, const std::string& program) {
    output << "usage: " << program << " [-h] [--build-dir BUILD_DIR] [--repo-root REPO_ROOT]\n"
           << "       [--dependencies-only]\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    const Arguments names{"--build-dir", "--repo-root", "--dependencies-only", "--help"};
    const std::regex negative_number(R"(-([0-9]+|[0-9]*\.[0-9]+))");
    Arguments unknown;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "-h") { options.help = true; return options; }
        if (argument == "--") {
            for (++index; index < argc; ++index) unknown.emplace_back(argv[index]);
            break;
        }
        const auto equal = argument.find('=');
        const auto prefix = argument.substr(0, equal);
        const auto name = std::find_if(names.begin(), names.end(), [&](const auto& item) {
            return prefix.size() > 2 && item.starts_with(prefix);
        });
        if (name == names.end()) { unknown.push_back(argument); continue; }
        if (*name == "--help" || *name == "--dependencies-only") {
            if (equal != std::string::npos)
                throw ToolError("argument " + *name + ": ignored explicit argument " + mmltk::tools::text::byte_repr(argument.substr(equal + 1)), 2);
            if (*name == "--help") { options.help = true; return options; }
            options.dependencies_only = true;
            continue;
        }
        std::string value;
        if (equal != std::string::npos) value = argument.substr(equal + 1);
        else {
            if (index + 1 >= argc ||
                (std::string_view(argv[index + 1]).starts_with('-') && std::string_view(argv[index + 1]) != "-" &&
                 !std::regex_match(argv[index + 1], negative_number)))
                throw ToolError("argument " + *name + ": expected one argument", 2);
            value = argv[++index];
        }
        if (*name == "--build-dir") options.build_dir = value;
        else options.repo_root = value;
    }
    if (!unknown.empty()) {
        std::string message = "unrecognized arguments:";
        for (const auto& argument : unknown) message += ' ' + argument;
        throw ToolError(std::move(message), 2);
    }
    return options;
}

int check(int argc, char** argv) {
    // The original loads the authoritative manifest before option parsing.
    const auto manifest = payload_manifest();
    Options options;
    const std::string program = fs::path(argv[0]).filename().string();
    try { options = parse_options(argc, argv); }
    catch (const ToolError& error) {
        usage(std::cerr, program);
        std::cerr << program << ": error: " << error.what() << '\n';
        return error.status;
    }
    if (options.help) {
        usage(std::cout, program);
        std::cout << "\nValidate the effective container compilation policy, including cached graphs.\n\n"
                  << "options:\n  -h, --help            show this help message and exit\n"
                  << "  --build-dir BUILD_DIR\n  --repo-root REPO_ROOT\n  --dependencies-only\n";
        return 0;
    }
    CompilerPolicy policy(manifest);
    if (options.dependencies_only) { check_dependency_evidence(policy); return 0; }
    require(options.build_dir.has_value(), "--build-dir is required");
    const auto build_dir = resolved(*options.build_dir);
    require(options.repo_root.has_value(), "--repo-root is required");
    const auto repo_root = resolved(*options.repo_root);
    policy.cache(build_dir);
    check_dependency_declarations(policy, repo_root);
    check_dependency_evidence(policy);
    const auto entries = read_json(build_dir / "compile_commands.json");
    require(truthy(entries), "empty compile database");
    Names first_party_targets;
    for (const auto& name : lines(read_text(build_dir / "mmltk-first-party-targets.txt")))
        if (!name.empty()) first_party_targets.insert(name + ".dir");
    require(!first_party_targets.empty(), "missing registered first-party targets");
    std::map<std::string, std::size_t> counts{{"C", 0}, {"CXX", 0}, {"CUDA", 0}, {"ASM_NASM", 0}};
    for (const auto& entry : entries) {
        try { ++counts.at(policy.command(entry, repo_root, first_party_targets)); }
        catch (const std::exception& error) {
            throw ToolError(entry.value("file", std::string("<missing source>")) + ": " + error.what());
        }
    }
    check_precompiled_headers(entries, first_party_targets);
    std::cout << "mmltk: compiler invariants verified: {";
    bool first = true;
    for (const auto& [name, count] : counts) {
        if (!first) std::cout << ", ";
        first = false;
        std::cout << '"' << name << "\": " << count;
    }
    std::cout << "}\n";
    return 0;
}
} // namespace

export int mmltk_tool_check_toolchain_invariants(int argc, char** argv) {
    try { return check(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << "mmltk: compiler invariant failure: " << error.what() << '\n';
        return 1;
    }
}
