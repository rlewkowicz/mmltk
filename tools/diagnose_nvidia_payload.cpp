// Native counterpart of diagnose_nvidia_payload.py; inspect without mutation.
#include "cpp_linux.hpp"
#include "cpp_support.hpp"

#include <cstdint>
#include <dirent.h>
#include <iostream>
#include <memory>
#include <regex>
#include <sched.h>
#include <set>
#include <sstream>

namespace {
using namespace mmltk::tools;

void require(bool condition, const std::string& message) {
    if (!condition) throw ToolError(message);
}

std::string field(const json& value, const char* key) {
    return value.at(key).get<std::string>();
}

json load_manifest() {
    auto manifest = read_json("/diagnostic_metadata/nvidia-payload.json");
    const auto& metadata = manifest.at("donor_metadata");
    const auto& versions = manifest.at("versions");
    auto cuda = field(metadata, "cuda_version");
    const auto dot = cuda.rfind('.');
    if (dot != std::string::npos) cuda.resize(dot);
    require(cuda == field(versions, "cuda"),
            "CUDA donor build does not match the required toolkit release");
    for (const char* key : {"torch_distribution_version", "torch_runtime_version"}) {
        auto version = field(metadata, key);
        const auto suffix = version.find(".nv");
        if (suffix != std::string::npos) version.resize(suffix);
        require(version == field(versions, "torch"),
                std::string(key) + " does not match the required Torch source version");
    }
    require(manifest.at("modes") == json::array({"development", "runtime"}),
            "unexpected payload modes");
    const auto order = manifest.at("library_order").get<std::vector<std::string>>();
    require(order.size() == 4 && std::set<std::string>(order.begin(), order.end()) ==
                std::set<std::string>{"cuda_root", "torch_root", "native_root", "tensorrt_root"},
            "missing, duplicate, or unknown NVIDIA library search root");
    for (const char* key : {"cuda_root", "torch_root", "python_root", "native_root",
                            "tensorrt_root", "notice_root"}) {
        const fs::path root(field(manifest, key));
        require(root.is_absolute() && std::find(root.begin(), root.end(), fs::path("..")) == root.end(),
                "invalid normalized root: " + root.string());
    }
    return manifest;
}

bool within(const fs::path& input, const fs::path& base) {
    return mmltk::tools::path_within(mmltk::tools::lexical_path(input.string(), "", false), mmltk::tools::lexical_path(base.string(), "", false));
}

class DevelopmentSelection {
public:
    explicit DevelopmentSelection(const json& manifest)
        : manifest_(manifest), names_(manifest.at("excluded_names").get<std::vector<std::string>>()) {}

    bool operator()(fs::path path) {
        const auto excluded = excluded_components(path.string());
        if (!excluded.empty()) {
            const bool hpc_only = std::all_of(excluded.begin(), excluded.end(), [](const auto& name) {
                return name == "hpcx" || name == "hpc-x";
            });
            bool audited = false;
            if (hpc_only) {
                for (const auto& value : manifest_.at("audited_native_roots")) {
                    const auto raw = value.get<std::string>();
                    const fs::path root = mmltk::tools::lexical_path(raw, "", false);
                    if (within(path, root)) { audited = true; break; }
                    auto [entry, inserted] = resolved_roots_.try_emplace(raw);
                    if (inserted) entry->second = fs::weakly_canonical(fs::absolute(root));
                    const auto& resolved = entry->second;
                    if (within(path, resolved) || within(path, root.parent_path() / "share") ||
                        within(path, resolved.parent_path() / "share")) {
                        audited = true;
                        break;
                    }
                }
            }
            if (!audited) return false;
        }
        auto name = path.filename().string();
        constexpr std::string_view debugger = "cuda-gdb-python";
        if (name.starts_with(debugger)) {
            auto abi = name.substr(debugger.size());
            if (abi.ends_with("-tui")) abi.resize(abi.size() - 4);
            if (abi != field(manifest_, "python_abi")) return false;
        }
        const fs::path torch_root = mmltk::tools::lexical_path(field(manifest_, "torch_root"), "", false);
        const fs::path torch_source = mmltk::tools::lexical_path(field(manifest_, "torch_source"), "", false);
        if (within(path, torch_source)) {
            fs::path relative;
            auto position = path.begin();
            for (auto part = torch_source.begin(); part != torch_source.end(); ++part) ++position;
            for (; position != path.end(); ++position) relative /= *position;
            path = mmltk::tools::lexical_path((torch_root / relative).string(), "", false);
        }
        if (within(path, torch_root / "test")) return false;
        name = path.filename().string();
        if (path.parent_path() == torch_root / "lib" &&
            (name.ends_with("_test.so") || name == "libbackend_with_compiler.so")) return false;
        const auto helper = torch_root / "bin/torch_shm_manager";
        if (path == helper || path == helper.parent_path()) return true;
        return !within(path, helper.parent_path());
    }

private:
    std::set<std::string> excluded_components(std::string text) const {
        for (char& ch : text) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        std::set<std::string> result;
        // Equivalent to (?<![a-z0-9])(?:lib)?(name1|name2|...). The optional
        // prefix is greedy, alternatives retain manifest order, and matches
        // are nonoverlapping; an interior 'dali' in cudalibxt.h is not DALI.
        for (std::size_t offset = 0; offset < text.size();) {
            const char previous = offset ? text[offset - 1] : '/';
            if ((previous >= 'a' && previous <= 'z') || (previous >= '0' && previous <= '9')) {
                ++offset;
                continue;
            }
            std::size_t consumed = 0;
            for (const std::size_t prefix : {std::size_t(3), std::size_t(0)}) {
                if (prefix && text.compare(offset, prefix, "lib") != 0) continue;
                for (const auto& name : names_) {
                    if (text.compare(offset + prefix, name.size(), name) == 0) {
                        result.insert(name);
                        consumed = prefix + name.size();
                        break;
                    }
                }
                if (consumed) break;
            }
            offset += consumed ? consumed : 1;
        }
        return result;
    }

    const json& manifest_;
    std::vector<std::string> names_;
    std::map<std::string, fs::path> resolved_roots_;
};

json host_memory() {
    json memory = json::object();
    for (const auto& line : lines(read_text("/proc/meminfo"))) {
        const auto colon = line.find(':');
        require(colon != std::string::npos, "invalid /proc/meminfo record");
        const auto key = line.substr(0, colon);
        if (key == "MemTotal" || key == "MemAvailable") {
            std::istringstream input(line.substr(colon + 1));
            std::uint64_t amount;
            require(static_cast<bool>(input >> amount), "invalid /proc/meminfo memory amount");
            memory[key] = amount * 1024;
        }
    }
    return memory;
}

json includes(const fs::path& path) {
    // read_text() in Python performs universal newline translation.
    const auto bytes = read_text(path);
    std::string text;
    text.reserve(bytes.size());
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] == '\r') {
            text += '\n';
            if (index + 1 < bytes.size() && bytes[index + 1] == '\n') ++index;
        } else text += bytes[index];
    }
    // Strictly reject malformed UTF-8 just as the original text reader does.
    (void)json(text).dump();
    // Python's Unicode \s includes the C0 separators and these additional
    // Unicode whitespace characters, beyond the C++ classic locale's \s.
    static const std::string whitespace =
        "(?:[ \\t\\n\\r\\f\\v\\x1c-\\x1f]|\xc2\x85|\xc2\xa0|\xe1\x9a\x80|"
        "\xe2\x80[\x80-\x8a]|\xe2\x80\xa8|\xe2\x80\xa9|\xe2\x80\xaf|\xe2\x81\x9f|\xe3\x80\x80)";
    static const std::regex pattern("^" + whitespace + "*#" + whitespace + "*include" +
                                        whitespace + R"(*[<"]([^>"]+))",
                                    std::regex::ECMAScript | std::regex::multiline);
    json result = json::array();
    for (std::sregex_iterator match(text.begin(), text.end(), pattern), end; match != end; ++match)
        result.push_back((*match)[1].str());
    return result;
}

json headers(const std::set<std::string>& wanted, DevelopmentSelection& selected) {
    json records = json::object();
    for (const auto& name : wanted) records[name] = json::array();
    std::vector<fs::path> pending{"/opt", "/usr"};
    while (!pending.empty()) {
        const auto directory = std::move(pending.back());
        pending.pop_back();
        // os.walk ignores unreadable directories and never follows directory
        // symlinks, but includes symlinked files and errors on broken matches.
        std::unique_ptr<DIR, decltype(&::closedir)> stream(::opendir(directory.c_str()), &::closedir);
        if (!stream) continue;
        std::vector<fs::path> directories;
        std::vector<fs::path> matches;
        while (const auto* entry = ::readdir(stream.get())) {
            const std::string name(entry->d_name);
            if (name == "." || name == "..") continue;
            const auto path = directory / name;
            std::error_code error;
            if (fs::is_directory(path, error)) {
                if (!fs::is_symlink(path, error)) directories.push_back(path);
            } else if (wanted.contains(name)) matches.push_back(path);
        }
        stream.reset();
        for (const auto& path : matches) {
            const auto resolved = fs::canonical(path);
            const auto package = capture({"dpkg-query", "-S", resolved.string()});
            if (package.exec_errno)
                throw std::system_error(package.exec_errno, std::generic_category(), "dpkg-query");
            records[path.filename().string()].push_back({
                {"path", path.string()}, {"resolved", resolved.string()},
                {"development_selected", selected(resolved)},
                {"package", trim(package.out)}, {"includes", includes(path)}});
        }
        pending.insert(pending.end(), directories.rbegin(), directories.rend());
    }
    return records;
}

void usage(std::ostream& out, const std::string& program) {
    out << "usage: " << program << " [-h] [--header-list HEADER_LIST] [headers ...]\n";
}

std::set<std::string> arguments(int argc, char** argv) {
    std::vector<std::string> names;
    std::string header_list;
    bool positional_only = false;
    bool positional_closed = false;
    for (int index = 1; index < argc; ++index) {
        const std::string arg(argv[index]);
        if (!positional_only && arg == "--") { positional_only = true; continue; }
        const auto equal = arg.find('=');
        const auto option = arg.substr(0, equal);
        if (!positional_only && (option == "--h" || option == "--he"))
            throw ToolError("ambiguous option: " + option + " could match --help, --header-list", 2);
        if (!positional_only && (arg == "-h" || arg == "--help" || arg == "--hel")) {
            usage(std::cout, fs::path(argv[0]).filename().string());
            std::cout << "\npositional arguments:\n  headers\n\noptions:\n"
                         "  -h, --help            show this help message and exit\n"
                         "  --header-list HEADER_LIST\n";
            throw ToolError("", 0);
        }
        if (!positional_only && option.starts_with("--") && option.size() > 2 &&
            std::string_view("--header-list").starts_with(option)) {
            if (!names.empty()) positional_closed = true;
            if (equal != std::string::npos) header_list = arg.substr(equal + 1);
            else {
                if (index + 1 == argc || (argv[index + 1][0] == '-' && argv[index + 1][1]))
                    throw ToolError("argument --header-list: expected one argument", 2);
                header_list = argv[++index];
            }
        } else if (!positional_only && arg.starts_with('-') && arg != "-") {
            throw ToolError("unrecognized arguments: " + arg, 2);
        } else {
            if (positional_closed) throw ToolError("unrecognized arguments: " + arg, 2);
            names.push_back(arg);
        }
    }
    if (!header_list.empty()) {
        const auto extra = split(header_list, ',');
        names.insert(names.end(), extra.begin(), extra.end());
    }
    if (names.empty()) throw ToolError("at least one public header filename is required", 2);
    for (const auto& name : names)
        if (name.empty() || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") !=
                                std::string::npos)
            throw ToolError("headers must be public filenames, without directory paths", 2);
    return {names.begin(), names.end()};
}

// json.dumps(sort_keys=True) sorts recursively, escapes non-ASCII, and uses
// ', ' / ': ' separators even without indentation.
std::string report_text(const json& report) {
    const nlohmann::json sorted = report;
    const auto compact = sorted.dump(-1, ' ', true);
    std::string result;
    mmltk::tools::text::JsonStringState string;
    for (const char ch : compact) {
        result += ch;
        if (!string.consume(ch) && (ch == ',' || ch == ':')) result += ' ';
    }
    return result;
}
} // namespace

int main(int argc, char** argv) {
    const std::string program = fs::path(argv[0]).filename().string();
    try {
        const auto wanted = arguments(argc, argv);
        const auto manifest = load_manifest();
        const auto memory = host_memory();
        DevelopmentSelection selected(manifest);
        const auto records = headers(wanted, selected);
        const auto image = optional_env("MMLTK_DIAGNOSTIC_IMAGE");
        require(image.has_value(), "MMLTK_DIAGNOSTIC_IMAGE is not set");
        const auto image_id = optional_env("MMLTK_DIAGNOSTIC_IMAGE_ID");
        const auto cpus = mmltk::tools::CpuAffinity(128, "CPU affinity mask is too large").count();
        const fs::path memory_limit("/sys/fs/cgroup/memory.max");
        std::error_code error;
        const auto limit = fs::is_regular_file(memory_limit, error)
            ? json(trim(read_text(memory_limit))) : json(nullptr);
        std::cout << report_text({{"image", *image}, {"image_id", image_id ? json(*image_id) : json(nullptr)},
                                 {"available_cpus", cpus}, {"host_memory_bytes", memory},
                                 {"container_memory_limit", limit}, {"headers", records}}) << '\n';
        return 0;
    } catch (const ToolError& error) {
        if (error.status == 2) usage(std::cerr, program);
        if (error.status) std::cerr << program << ": error: " << error.what() << '\n';
        return error.status;
    } catch (const std::exception& error) {
        std::cerr << program << ": error: " << error.what() << '\n';
        return 1;
    }
}
