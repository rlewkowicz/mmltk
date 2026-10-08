// Native counterpart of configure_cmake_graph.sh. Requires cpp_support.hpp,
// vendored nlohmann JSON headers, and OpenSSL libcrypto. CMake and the existing
// Python invariant checker remain external commands; no shell is involved.
#include "cpp_digest.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <openssl/evp.h>
#include <unistd.h>

namespace {
namespace support = mmltk::tools;
namespace fs = std::filesystem;
using support::ToolError;

using support::Sha256;

std::vector<std::string> configuration_inputs(const fs::path& root) {
    // Keep the exact input names and ordering used by existing cache markers.
    std::vector<std::string> result{
        "CMakePresets.json", "CMakeLists.txt", "docker/nvidia-payload.json",
        "docker/record_compiler_evidence.py", "tools/check_toolchain_invariants.py",
        "tools/configure_cmake_graph.sh"};
    std::vector<std::string> discovered;
    for (const auto* directory : {"cmake", "src"}) {
        // Like find's default -P traversal, neither descend into directory
        // symlinks nor select symlinks pointing to regular files.
        std::vector<fs::path> pending{root / directory};
        while (!pending.empty()) {
            auto path = std::move(pending.back());
            pending.pop_back();
            std::error_code error;
            const auto status = fs::symlink_status(path, error);
            if (!error && fs::is_regular_file(status)) {
                const auto name = path.filename().string();
                if (name == "CMakeLists.txt" || name.ends_with(".cmake"))
                    discovered.push_back(path.lexically_relative(root).generic_string());
            } else if (!error && fs::is_directory(status)) {
                fs::directory_iterator iterator(path, error), end;
                while (!error && iterator != end) {
                    pending.push_back(iterator->path());
                    iterator.increment(error);
                }
            }
            // find runs in a process substitution: diagnostics are visible,
            // but failure in one subtree does not discard its siblings.
            if (error) std::cerr << "find: " << path.string() << ": " << error.message() << '\n';
        }
    }
    std::sort(discovered.begin(), discovered.end());
    result.insert(result.end(), discovered.begin(), discovered.end());
    return result;
}

std::string marker_value(const fs::path& path) {
    auto value = support::read_text(path);
    // Bash command substitution drops trailing newlines, not other whitespace.
    return support::text::trim_newlines(std::move(value));
}

void command(const std::vector<std::string>& arguments) {
    const auto result = support::run(arguments);
    if (result.exit_code)
        throw ToolError(arguments.front() + " failed with exit status " +
                        std::to_string(result.exit_code), result.exit_code);
}

int configure(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <preset> <build-dir> [cmake-options...]\n";
        return 2;
    }
    const std::string preset = argv[1], build_dir = argv[2];
    const auto cache_root = support::env("MMLTK_CONTAINER_CACHE_ROOT");
    const auto toolchain = support::env("MMLTK_TOOLCHAIN_ID");
    if (cache_root.empty()) throw ToolError("MMLTK_CONTAINER_CACHE_ROOT is required");
    if (toolchain.empty()) throw ToolError("MMLTK_TOOLCHAIN_ID is required");
    // Deliberately retain the original lexical prefix guard and unnormalized
    // directory argument; existing callers supply container-owned cache paths.
    if (!build_dir.starts_with(cache_root + "/cmake/")) {
        std::cerr << "refusing CMake build directory outside the cache root: " << build_dir << '\n';
        return 2;
    }
    const auto root = support::repository_root(argv[0], __FILE__);
    const auto inputs = configuration_inputs(root);
    for (const auto& input : inputs) {
        if (!mmltk::tools::regular_file(root / input)) {
            std::cerr << "missing CMake configuration input: " << (root / input).string() << '\n';
            return 2;
        }
    }
    Sha256 source_digest;
    support::FileHasher file_digest;
    for (const auto& input : inputs) {
        source_digest.field(input);
        // sha256sum | cut writes a newline before the following explicit NUL.
        source_digest.field(file_digest(root / input) + "\n");
    }
    Sha256 configure_digest;
    configure_digest.field(preset);
    configure_digest.field(toolchain);
    configure_digest.field(source_digest.finish());
    for (int index = 3; index < argc; ++index) configure_digest.field(argv[index]);
    // printf '%s\0' "$@" emits one NUL even when there are no options.
    if (argc == 3) configure_digest.field("");
    const auto identity = configure_digest.finish();
    const fs::path build(build_dir);
    const auto toolchain_marker = build / ".mmltk-toolchain-identity";
    const auto configure_marker = build / ".mmltk-configure-identity";
    std::error_code error;
    if (fs::exists(build, error) &&
        (!mmltk::tools::regular_file(toolchain_marker) || marker_value(toolchain_marker) != toolchain))
        command({"cmake", "-E", "rm", "-rf", build_dir});
    const bool executed = !mmltk::tools::regular_file(build / "build.ninja") ||
        !mmltk::tools::regular_file(build / "CMakeCache.txt") || !mmltk::tools::regular_file(toolchain_marker) ||
        !mmltk::tools::regular_file(configure_marker) || marker_value(configure_marker) != identity;
    if (executed) {
        // Invalidate success first, retaining CMakeFiles, generated artifacts,
        // objects/modules, and Ninja history for compatible toolchain images.
        command({"cmake", "-E", "rm", "-f", (build / "CMakeCache.txt").string(),
                 configure_marker.string()});
        std::vector<std::string> arguments{"cmake", "--preset", preset, "-B", build_dir};
        for (int index = 3; index < argc; ++index) arguments.emplace_back(argv[index]);
        command(arguments);
    }
    command({"python3", (root / "tools/check_toolchain_invariants.py").string(),
             "--repo-root", root.string(), "--build-dir", build_dir});
    if (executed) {
        const auto suffix = ".next." + std::to_string(::getpid());
        const auto toolchain_next = toolchain_marker.string() + suffix;
        const auto configure_next = configure_marker.string() + suffix;
        support::write_text(toolchain_next, toolchain + "\n");
        support::write_text(configure_next, identity + "\n");
        fs::rename(toolchain_next, toolchain_marker);
        fs::rename(configure_next, configure_marker);
    }
    const std::string state = executed ? "executed" : "skipped";
    std::cerr << "mmltk: CMake configure " << state << " for " << preset << '\n';
    const auto trace = support::env("MMLTK_BUILD_TRACE_FILE");
    if (!trace.empty()) {
        const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ofstream stream(trace, std::ios::binary | std::ios::app);
        if (!stream) throw ToolError("cannot append " + trace);
        // Retain the shell tool's field order and literal preset interpolation.
        stream << "{\"timestamp_ns\":" << timestamp << ",\"pid\":" << ::getpid()
               << ",\"phase\":\"configure:" << preset
               << "\",\"event\":\"finish\",\"result\":\"" << state << "\"}\n";
        stream.close();
        if (!stream) throw ToolError("write failed: " + trace);
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try { return configure(argc, argv); }
    catch (const ToolError& error) {
        std::cerr << "configure_cmake_graph: " << error.what() << '\n';
        return error.status;
    }
    catch (const std::exception& error) {
        std::cerr << "configure_cmake_graph: " << error.what() << '\n';
        return 1;
    }
}
