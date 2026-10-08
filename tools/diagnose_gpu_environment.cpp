module;
// Native counterpart of diagnose_gpu_environment.py. Build with the existing
// third_party/json/include, CUDA toolkit include directory, and Vulkan headers;
// link libdl. CUDA and Vulkan are resolved only inside bounded child processes,
// never linked as startup dependencies. No device/context/instance API is used.

#include <cuda.h>
#include <cuda_runtime_api.h>
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

#include <dlfcn.h>
#include <glob.h>
#include <sys/utsname.h>

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

export module mmltk.tools.diagnose_gpu_environment;
import mmltk.tools.tool_command;
import mmltk.tools.cpp_time;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
import mmltk.tools.cpp_shared_library;

namespace {
using mmltk::tools::json;
namespace fs = std::filesystem;
namespace support = mmltk::tools;

constexpr std::size_t text_limit = 8192;
constexpr std::size_t item_limit = 64;
constexpr int command_timeout = 5;
constexpr std::array environment_names{
    "LD_LIBRARY_PATH", "LD_PRELOAD", "CUDA_HOME", "CUDA_PATH", "CUDA_VISIBLE_DEVICES",
    "NVIDIA_VISIBLE_DEVICES", "NVIDIA_DRIVER_CAPABILITIES", "VK_DRIVER_FILES",
    "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES", "VK_LAYER_PATH",
    "VK_LOADER_DRIVERS_SELECT", "VK_LOADER_DRIVERS_DISABLE",
    "XDG_CONFIG_HOME", "XDG_CONFIG_DIRS", "XDG_DATA_HOME", "XDG_DATA_DIRS"};

enum class VersionKind { driver, runtime, vulkan };
struct VersionQuery {
    VersionKind kind;
    const char* name;
    const char* soname;
    const char* symbol;
};
constexpr std::array version_queries{
    VersionQuery{VersionKind::driver, "cuda_driver", "libcuda.so.1", "cuDriverGetVersion"},
    VersionQuery{VersionKind::runtime, "cuda_runtime", "libcudart.so", "cudaRuntimeGetVersion"},
    VersionQuery{VersionKind::vulkan, "vulkan_loader", "libvulkan.so.1", "vkEnumerateInstanceVersion"}};

// Match Python's UTF-8 replacement decoding before parsing manifests or
// trimming command output, including an incomplete codepoint at the byte cap.

std::string bounded_text(std::string_view value, std::size_t characters = text_limit) {
    auto text = support::text::decode_utf8(value);
    std::size_t index = 0;
    for (std::size_t count = 0; index < text.size() && count < characters; ++count) {
        ++index;
        while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xc0) == 0x80) ++index;
    }
    text.resize(index);
    return text;
}

json unavailable(std::string_view reason) {
    return {{"status", "unavailable"}, {"reason", bounded_text(reason)}};
}

json read_text(const fs::path& path, std::size_t limit = text_limit) {
    json result{{"path", path.string()}};
    support::FileDescriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) {
        result.update(unavailable(path.string() + ": " + std::strerror(errno)));
        return result;
    }
    std::string bytes(limit + 1, '\0');
    const auto [used, error] = support::read_into(fd.get(), std::span<char>(bytes));
    if (error) {
        result.update(unavailable(path.string() + ": " + std::strerror(error)));
        return result;
    }
    result.update({{"status", "observed"},
                   {"value", support::trim(support::text::decode_utf8(std::string_view(bytes).substr(0, std::min(used, limit))))},
                   {"truncated", used > limit}});
    return result;
}

json path_fact(const fs::path& path) {
    json result{{"path", path.string()}};
    std::error_code error;
    const auto resolved = fs::canonical(path, error);
    if (error) result.update(unavailable(path.string() + ": " + error.message()));
    else result.update({{"status", "observed"}, {"resolved", resolved.string()},
                        {"file_name", resolved.filename().string()}});
    return result;
}

std::optional<fs::path> which(const std::string& name) {
    const auto usable = [](const fs::path& candidate) {
        std::error_code error;
        return ::access(candidate.c_str(), F_OK | X_OK) == 0 && !fs::is_directory(candidate, error) && !error;
    };
    if (name.find('/') != std::string::npos) return usable(name) ? std::optional<fs::path>(name) : std::nullopt;
    for (const auto& root : support::split(support::env("PATH", "/bin:/usr/bin"), ':')) {
        const auto candidate = fs::path(root) / name;
        if (usable(candidate)) return candidate;
    }
    return std::nullopt;
}

json command(const std::vector<std::string>& arguments, std::size_t limit = text_limit) {
    const auto executable = which(arguments.front());
    if (!executable) return unavailable(arguments.front() + " is not available in this image");
    try {
        auto invocation = arguments;
        invocation.front() = executable->string();
        support::RunOptions options;
        options.input = "";
        options.env["LC_ALL"] = "C";
        options.timeout = std::chrono::seconds(command_timeout);
        std::string output, errors;
        bool truncated = false;
        const auto retain = [&](std::string& target, std::string_view chunk) {
            const auto remaining = limit - target.size();
            truncated = truncated || chunk.size() > remaining;
            target.append(chunk.substr(0, remaining));
        };
        options.stdout_sink = [&](std::string_view chunk) { retain(output, chunk); };
        options.stderr_sink = [&](std::string_view chunk) { retain(errors, chunk); };
        const auto process = support::run(invocation, options);
        if (process.timed_out || process.exec_errno) {
            json receipt{{"argv", arguments}};
            receipt.update(unavailable(process.timed_out ? "command timed out after 5 seconds" :
                                      std::string(std::strerror(process.exec_errno))));
            return receipt;
        }
        return {{"status", process.exit_code == 0 ? "observed" : "unavailable"},
                {"executable", fs::weakly_canonical(*executable).string()}, {"argv", arguments},
                {"returncode", process.signal ? -process.signal : process.exit_code},
                {"stdout", support::trim(support::text::decode_utf8(output))}, {"stderr", support::trim(support::text::decode_utf8(errors))},
                {"truncated", truncated}};
    } catch (const std::exception& error) {
        json receipt{{"argv", arguments}};
        receipt.update(unavailable(error.what()));
        return receipt;
    }
}

std::vector<std::string> glob_paths(const fs::path& pattern, std::size_t limit = SIZE_MAX) {
    struct Matches {
        glob_t value{};
        ~Matches() { ::globfree(&value); }
    } matches;
    std::vector<std::string> paths;
    if (::glob(pattern.c_str(), GLOB_NOSORT, nullptr, &matches.value) != 0) return paths;
    for (std::size_t i = 0; i < std::min(limit, matches.value.gl_pathc); ++i)
        paths.emplace_back(matches.value.gl_pathv[i]);
    return paths;
}

void append_unique(std::vector<std::string>& values, std::string value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(std::move(value));
}

json library_inventory() {
    static const std::regex library_name(R"(^lib(cuda(rt)?|vulkan|GLX_nvidia|nvidia-[\w-]+)\.so(\.|$))");
    auto cache = command({"ldconfig", "-p"}, 256 * 1024);
    std::vector<std::pair<std::string, std::string>> paths;
    std::set<std::string> seen;
    const auto add = [&](const std::string& path, const std::string& name) {
        if (seen.insert(path).second) paths.emplace_back(path, name);
    };
    for (const auto& line : support::lines(cache.value("stdout", ""))) {
        std::istringstream stream(line);
        const std::vector<std::string> fields(std::istream_iterator<std::string>{stream},
                                              std::istream_iterator<std::string>{});
        if (fields.size() >= 4 && std::regex_search(fields[0], library_name) && fields[fields.size() - 2] == "=>") {
            add(fields.back(), fields[0]);
            if (paths.size() == item_limit) break;
        }
    }
    std::string cache_output;
    for (const auto& [path, name] : paths) {
        if (!cache_output.empty()) cache_output += '\n';
        cache_output += name + " => " + path;
    }
    cache["stdout"] = cache_output;
    std::vector<std::string> roots;
    for (const auto& root : support::split(support::env("LD_LIBRARY_PATH"), ':', false)) {
        append_unique(roots, root);
        if (roots.size() == 16) break;
    }
    for (const auto* root : {"/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu",
                             "/usr/local/nvidia/lib64", "/usr/local/nvidia/lib", "/usr/lib/wsl/lib"})
        append_unique(roots, root);
    for (const auto& root : glob_paths("/usr/local/cuda*/lib64", 8)) append_unique(roots, root);
    bool limited = paths.size() == item_limit;
    for (const auto& root : roots) {
        if (paths.size() == item_limit) break;
        for (const auto& path : glob_paths(fs::path(root) / "lib*.so*")) {
            const auto name = fs::path(path).filename().string();
            if (!std::regex_search(name, library_name)) continue;
            add(path, name);
            if (paths.size() == item_limit) { limited = true; break; }
        }
    }
    auto files = json::array();
    for (const auto& [path, name] : paths) {
        json item{{"soname", name}};
        item.update(path_fact(path));
        files.push_back(std::move(item));
    }
    return {{"scope", "available linker-cache entries and conventional/LD_LIBRARY_PATH locations; availability is not loading"},
            {"linker_cache", cache}, {"searched_directories", roots}, {"limit_reached", limited}, {"files", files}};
}

json loaded_object(void* address) {
    const auto maps = read_text("/proc/self/maps", 128 * 1024);
    const auto numeric_address = reinterpret_cast<std::uintptr_t>(address);
    for (const auto& line : support::lines(maps.value("value", ""))) {
        std::istringstream stream(line);
        std::string range, permissions, offset, device, inode, path;
        if (!(stream >> range >> permissions >> offset >> device >> inode)) continue;
        std::getline(stream >> std::ws, path);
        if (path.empty()) continue;
        const auto separator = range.find('-');
        if (separator == std::string::npos) continue;
        const auto begin = std::stoull(range.substr(0, separator), nullptr, 16);
        const auto end = std::stoull(range.substr(separator + 1), nullptr, 16);
        if (begin <= numeric_address && numeric_address < end) return path_fact(path);
    }
    return unavailable("version entrypoint was not found in bounded /proc/self/maps");
}

json library_version(const VersionQuery& query, const std::string& requested) {
    json result{{"requested", requested}, {"query", query.symbol},
                {"scope", "diagnostic process resolution; no CUDA context or Vulkan instance is created"}};
    try {
        support::SharedLibrary library(requested.c_str());
        auto* address = library.symbol(query.symbol, true);
        result["loaded_object"] = loaded_object(address);
        int status = 0;
        json version;
        json raw;
        if (query.kind == VersionKind::vulkan) {
            std::uint32_t value = 0;
            status = static_cast<int>(reinterpret_cast<PFN_vkEnumerateInstanceVersion>(address)(&value));
            raw = value;
            version = {{"variant", value >> 29}, {"major", (value >> 22) & 0x7f},
                       {"minor", (value >> 12) & 0x3ff}, {"patch", value & 0xfff}};
        } else {
            int value = 0;
            if (query.kind == VersionKind::driver)
                status = static_cast<int>(reinterpret_cast<decltype(&cuDriverGetVersion)>(address)(&value));
            else status = static_cast<int>(reinterpret_cast<decltype(&cudaRuntimeGetVersion)>(address)(&value));
            raw = value;
            const std::int64_t major = value / 1000 - (value < 0 && value % 1000 != 0 ? 1 : 0);
            version = {{"major", major}, {"minor", (value - major * 1000) / 10}};
        }
        result["api_status"] = status;
        if (status) result.update(unavailable(std::string(query.symbol) + " returned status " + std::to_string(status)));
        else result.update({{"status", "observed"}, {"api_version_raw", raw}, {"api_version", version}});
    } catch (const std::exception& error) {
        result.update(unavailable(error.what()));
    }
    return result;
}

json loaded_versions(const json& inventory) {
    auto versions = json::object();
    for (const auto& query : version_queries) {
        std::string requested = query.soname;
        if (query.kind == VersionKind::runtime) {
            std::optional<std::string> candidate;
            bool exact = false;
            for (const auto& file : inventory.at("files")) {
                const auto name = file.at("soname").get<std::string>();
                if (name.starts_with("libcudart.so")) {
                    if (!candidate) candidate = name;
                    exact = exact || name == query.soname;
                }
            }
            if (!exact && candidate) requested = *candidate;
        }
        const auto receipt = command(support::command::self_invocation(support::command::Command::diagnose_gpu_environment, {"--library-version", query.name, requested}));
        if (receipt.at("status") != "observed" || receipt.value("truncated", false)) {
            versions[query.name] = receipt;
            continue;
        }
        try { versions[query.name] = json::parse(receipt.at("stdout").get<std::string>()); }
        catch (const json::parse_error&) {
            versions[query.name] = unavailable("version child returned invalid JSON");
            versions[query.name]["receipt"] = receipt;
        }
    }
    return versions;
}

json vulkan_manifests(const json& inventory) {
    std::vector<std::string> roots{"/etc/vulkan/icd.d", "/usr/local/share/vulkan/icd.d", "/usr/share/vulkan/icd.d"};
    for (const auto& [variable, fallback] : std::array{
             std::pair{"XDG_CONFIG_HOME", ".config"}, std::pair{"XDG_DATA_HOME", ".local/share"}}) {
        auto location = support::env(variable);
        if (location.empty() && !support::env("HOME").empty()) location = (fs::path(support::env("HOME")) / fallback).string();
        if (!location.empty()) append_unique(roots, (fs::path(location) / "vulkan/icd.d").string());
    }
    for (const auto* variable : {"XDG_CONFIG_DIRS", "XDG_DATA_DIRS"}) {
        const auto entries = support::split(support::env(variable), ':', false);
        for (std::size_t i = 0; i < std::min<std::size_t>(8, entries.size()); ++i)
            append_unique(roots, (fs::path(entries[i]) / "vulkan/icd.d").string());
    }
    std::vector<std::pair<std::string, std::string>> filenames;
    std::set<std::string> seen;
    const auto add = [&](const std::string& path, const std::string& discovery) {
        if (filenames.size() < item_limit && seen.insert(path).second) filenames.emplace_back(path, discovery);
    };
    for (const auto* variable : {"VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES"}) {
        const auto entries = support::split(support::env(variable), ':', false);
        for (std::size_t i = 0; i < std::min(item_limit, entries.size()); ++i) add(entries[i], variable);
    }
    for (const auto& root : roots)
        for (const auto& path : glob_paths(fs::path(root) / "*.json", item_limit)) add(path, "directory");
    auto manifests = json::array();
    for (const auto& [filename, discovered_by] : filenames) {
        json entry{{"manifest", path_fact(filename)}, {"discovered_by", discovered_by}};
        const auto text = read_text(filename);
        try {
            if (text.at("status") != "observed" || text.value("truncated", false)) {
                entry["declaration"] = text.at("status") != "observed" ? text : unavailable("manifest exceeds byte limit");
            } else {
                const auto document = json::parse(text.at("value").get<std::string>());
                const auto& icd = document.at("ICD");
                const auto& library_value = icd.at("library_path");
                if (!library_value.is_string() || library_value.get_ref<const std::string&>().empty())
                    throw support::ToolError("ICD library_path must be nonempty text");
                const auto library = library_value.get<std::string>();
                entry["declaration"] = {{"status", "observed"}, {"file_format_version", document.value("file_format_version", json(nullptr))},
                                        {"library_path", library}, {"declared_api_version", icd.value("api_version", json(nullptr))}};
                auto candidates = json::array();
                if (library.find('/') != std::string::npos) {
                    const auto candidate = fs::path(library).is_absolute() ? fs::path(library) : fs::path(filename).parent_path() / library;
                    candidates.push_back(path_fact(candidate));
                } else {
                    for (const auto& item : inventory.at("files"))
                        if (item.at("soname") == library) candidates.push_back(item);
                }
                entry["available_objects"] = candidates.empty() ? unavailable("declared library not found in bounded inventory") : candidates;
            }
        } catch (const std::exception& error) {
            entry["declaration"] = unavailable(error.what());
        }
        manifests.push_back(std::move(entry));
    }
    return {{"scope", "available ICD declarations; no ICD selection or Vulkan instance creation was performed"},
            {"searched_directories", roots}, {"limit_reached", filenames.size() == item_limit}, {"manifests", manifests}};
}

std::vector<std::vector<std::string>> csv_rows(std::string_view text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, start = true;
    for (const auto& line : support::lines(text)) {
        if (line.empty() && !quoted && row.empty() && field.empty()) {
            rows.emplace_back();
            if (rows.size() > item_limit) break;
            continue;
        }
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char ch = line[i];
            if (quoted) {
                if (ch == '"' && i + 1 < line.size() && line[i + 1] == '"') { field += '"'; ++i; }
                else if (ch == '"') quoted = false;
                else field += ch;
            } else if (ch == ',' ) {
                row.push_back(std::exchange(field, {}));
                start = true;
            } else if (ch == '"' && start) {
                quoted = true;
                start = false;
            } else { field += ch; start = false; }
        }
        if (!quoted) {
            row.push_back(std::exchange(field, {}));
            rows.push_back(std::exchange(row, {}));
            start = true;
        }
        if (rows.size() > item_limit) break;
    }
    if (rows.size() <= item_limit && (quoted || !row.empty() || !field.empty())) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return rows;
}

json gpu_identity() {
    constexpr std::array fields{"index", "name", "uuid", "pci.bus_id", "driver_version"};
    auto receipt = command({"nvidia-smi", "--query-gpu=index,name,uuid,pci.bus_id,driver_version", "--format=csv,noheader,nounits"});
    if (receipt.at("status") != "observed") return receipt;
    const auto rows = csv_rows(receipt.at("stdout").get<std::string>());
    auto devices = json::array();
    for (std::size_t i = 0; i < std::min(item_limit, rows.size()); ++i) {
        if (rows[i].size() != fields.size()) {
            auto result = unavailable("unexpected nvidia-smi identity columns");
            result["receipt"] = receipt;
            return result;
        }
        auto device = json::object();
        for (std::size_t column = 0; column < fields.size(); ++column) {
            const auto value = support::trim(rows[i][column]);
            device[fields[column]] = value == "N/A" || value == "[N/A]" || value == "[Not Supported]" || value == "[Unknown Error]" ?
                unavailable("nvidia-smi returned " + value) : json(value);
        }
        devices.push_back(std::move(device));
    }
    receipt.erase("stdout");
    return {{"status", "observed"}, {"query", receipt}, {"devices", devices}, {"limit_reached", rows.size() > item_limit}};
}

json virtualization_facts() {
    auto detection = json::object();
    for (const auto* scope : {"container", "vm"}) {
        auto receipt = command({"systemd-detect-virt", std::string("--") + scope});
        if (receipt.value("returncode", 0) == 1 && receipt.value("stdout", "") == "none") receipt["status"] = "not_detected";
        detection[scope] = std::move(receipt);
    }
    auto dmi = json::object();
    for (const auto* name : {"sys_vendor", "product_name", "product_version", "bios_vendor"})
        dmi[name] = read_text(fs::path("/sys/class/dmi/id") / name);
    return {{"scope", "container-visible facts and explicit detector results; absence does not establish bare metal or an outer host OS"},
            {"detection", detection}, {"hypervisor_type", read_text("/sys/hypervisor/type")},
            {"container_marker", read_text("/run/systemd/container")}, {"dxg_device", path_fact("/dev/dxg")}, {"dmi", dmi}};
}

json report() {
    const auto inventory = library_inventory();
    auto modules = read_text("/proc/modules", 64 * 1024);
    if (modules.at("status") == "observed") {
        std::string selected;
        for (const auto& line : support::lines(modules.at("value").get<std::string>())) {
            std::istringstream stream(line);
            std::string name;
            stream >> name;
            if (name.starts_with("nvidia") || name.starts_with("nouveau") || name.starts_with("dxgkrnl")) {
                if (!selected.empty()) selected += '\n';
                selected += line;
            }
        }
        modules["value"] = std::move(selected);
    }
    utsname kernel{};
    if (::uname(&kernel) != 0) throw std::system_error(errno, std::generic_category(), "uname");
    const int group_count = ::getgroups(0, nullptr);
    if (group_count < 0) throw std::system_error(errno, std::generic_category(), "getgroups");
    std::vector<gid_t> groups(static_cast<std::size_t>(group_count));
    if (group_count && ::getgroups(group_count, groups.data()) < 0)
        throw std::system_error(errno, std::generic_category(), "getgroups");
    auto environment = json::object();
    for (const auto* name : environment_names) {
        const auto value = support::optional_env(name);
        environment[name] = value ? json(bounded_text(*value)) : unavailable("not set");
    }
    const auto image = support::optional_env("MMLTK_DIAGNOSTIC_IMAGE");
    const auto image_id = support::optional_env("MMLTK_DIAGNOSTIC_IMAGE_ID");
    return {{"schema_version", 1}, {"observed_at", support::utc_timestamp(support::TimestampFormat::python, "cannot format current UTC time")},
            {"scope", "read-only metadata and library version APIs; no application, CUDA context/device selection, Vulkan instance, allocation, or GPU workload"},
            {"limits", {{"file_bytes", text_limit}, {"inventory_items", item_limit}, {"command_timeout_seconds", command_timeout}}},
            {"container", {{"image", image ? json(*image) : json(nullptr)}, {"image_id", image_id ? json(*image_id) : json(nullptr)},
                           {"hostname", kernel.nodename}, {"uid", ::getuid()}, {"gid", ::getgid()}, {"groups", groups},
                           {"os_release", read_text("/etc/os-release")}, {"cgroup", read_text("/proc/self/cgroup")},
                           {"docker_marker", path_fact("/.dockerenv")}}},
            {"kernel", {{"sysname", kernel.sysname}, {"release", kernel.release}, {"version", kernel.version}, {"machine", kernel.machine}}},
            {"environment", environment},
            {"nvidia", {{"gpus", gpu_identity()}, {"proc_driver_version", read_text("/proc/driver/nvidia/version")},
                        {"module_version", read_text("/sys/module/nvidia/version")}, {"loaded_kernel_modules", modules}}},
            {"libraries", inventory}, {"loaded_version_queries", loaded_versions(inventory)}, {"vulkan", vulkan_manifests(inventory)},
            {"package_versions", command({"dpkg-query", "-W", "-f=${binary:Package}\t${Version}\n",
                                          "libvulkan*", "libnvidia*", "cuda-cudart*", "nvidia-*"})},
            {"virtualization", virtualization_facts()}};
}

void print_json(const json& value, int indent = -1) {
    // std::map-backed JSON preserves the original recursively sorted keys.
    std::cout << nlohmann::json(value).dump(indent, ' ', true, nlohmann::json::error_handler_t::replace) << '\n';
}
} // namespace

export int mmltk_tool_diagnose_gpu_environment(int argc, char** argv) {
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--library-version") {
            for (const auto& query : version_queries) {
                if (std::string_view(argv[2]) == query.name) {
                    print_json(library_version(query, argv[3]));
                    return 0;
                }
            }
        }
        if (argc != 1) {
            std::cerr << "Use ./mmltk --diagnose-gpu-environment [runtime|wayland-validation|development]\n";
            return 1;
        }
        print_json(report(), 2);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "diagnose_gpu_environment: " << error.what() << '\n';
        return 1;
    }
}
