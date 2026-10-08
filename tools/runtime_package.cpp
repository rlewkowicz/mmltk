module;

#include <array>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <sys/socket.h>

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <vector>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
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

export module mmltk.tools.runtime_package;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;

export {
namespace mmltk::tools::runtime_package {

enum class Layout { stage, image };

// Entry names retain the shell inventory's '?' optional-entry prefix. Source
// metadata is available to release fingerprint callers without another table.
struct Row {
    std::string_view path;
    std::string_view destination;
    std::string_view source_kind;
    std::string_view source_path;
    std::span<const std::string_view> entries;
};

std::span<const Row> inventory() noexcept;
// Ordered traversal stops at and returns the first nonzero callback status.
int inventory(const std::function<int(const Row&)>& callback);
bool check_directory(const std::filesystem::path& root, Layout layout, const Row& row);
bool complete(const std::filesystem::path& root, Layout layout = Layout::stage);

// Return lowercase SHA-256 without a trailing newline. Operational failures
// throw ToolError (from mmltk.tools.cpp_support) or a standard filesystem/system error.
std::string path_fingerprint(const std::filesystem::path& root);
// The record includes its terminating newline, as in the shell callback.
std::string hash_directory(const std::filesystem::path& root, const Row& row);
std::string fingerprint(const std::filesystem::path& root);

// Copy merges the directory into its absolute image destination. No rollback
// is performed after a failure, matching the original installation library.
int copy_directory(const std::filesystem::path& root, const Row& row);
int install(const std::filesystem::path& root);

} // namespace mmltk::tools::runtime_package

}

namespace mmltk::tools::runtime_package {
namespace {
using namespace std::literals;
constexpr std::array bin_entries{"mmltk"sv, "mmltk-browser-bundle-contract"sv,
    "mmltk-browser-host"sv, "?mmltk-rfdetr-onnx-info"sv, "?mmltk-rfdetr-onnx-simplify"sv};
constexpr std::array gcc_entries{"libstdc++.so.6"sv, "libgcc_s.so.1"sv};
constexpr std::array firefox_entries{"firefox"sv, "firefox-bin"sv, "libmozgtk.so"sv, "libxul.so"sv};
constexpr std::array gdrcopy_entries{"LICENSE"sv, "UPSTREAM.md"sv};
constexpr std::array arrow_entries{"LICENSE.txt"sv, "NOTICE.txt"sv, "snappy"sv,
    "zstd"sv, "zlib"sv, "xsimd"sv, "thrift"sv, "rapidjson"sv};
constexpr std::array model_entries{"ShiftLUT_fp32.onnx"sv, "RealPLKSR_fp16.onnx"sv};
constexpr std::array python_entries{"rfdetr_checkpoint_bridge.py"sv};
constexpr std::array browser_entries{"index.html"sv, "browser_protocol.marker"sv};
constexpr std::array<Row, 8> rows{{
    {"bin", "/opt/mmltk/bin", "release", ".", bin_entries},
    {"lib/gcc-16.2", "/opt/gcc-16.2/lib64", "image", ".", gcc_entries},
    {"lib/mmltk/firefox", "/opt/mmltk/lib/mmltk/firefox", "firefox", ".", firefox_entries},
    {"share/mmltk/licenses/gdrcopy", "/opt/mmltk/share/mmltk/licenses/gdrcopy",
        "repository", "third_party/gdrcopy", gdrcopy_entries},
    {"share/mmltk/licenses/arrow", "/opt/mmltk/share/mmltk/licenses/arrow",
        "image", ".", arrow_entries},
    {"models", "/opt/mmltk/models", "repository", "src/backend/imaging/upscale/assets", model_entries},
    {"share/mmltk/python", "/opt/mmltk/share/mmltk/python", "repository",
        "src/backend/models/rfdetr/training", python_entries},
    {"share/mmltk/browser-app", "/opt/mmltk/share/mmltk/browser-app", "browser",
        "generated/frontend/iced/browser_protocol.marker", browser_entries},
}};

// Stream archives directly into sha256sum: Firefox payloads need neither a
// payload-sized allocation nor an intermediate archive on disk. A socket lets
// send suppress SIGPIPE locally without altering the caller's signal policy.
class HashStream {
public:
    HashStream() {
        int sockets[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0)
            throw std::system_error(errno, std::generic_category(), "runtime package hash socketpair");
        FileDescriptor producer(sockets[0]), consumer(sockets[1]);
        input_ = detail::command_descriptor(producer.release());
        consumer = detail::command_descriptor(consumer.release());
        auto output = detail::pipe();
        posix_spawn_file_actions_t actions;
        detail::spawn_check(::posix_spawn_file_actions_init(&actions), "hash spawn actions");
        struct Cleanup {
            posix_spawn_file_actions_t* actions;
            ~Cleanup() { ::posix_spawn_file_actions_destroy(actions); }
        } cleanup{&actions};
        detail::spawn_check(::posix_spawn_file_actions_adddup2(&actions, consumer.get(), STDIN_FILENO), "hash stdin");
        detail::spawn_check(::posix_spawn_file_actions_adddup2(&actions, output.second.get(), STDOUT_FILENO), "hash stdout");
        for (const int fd : {input_.get(), consumer.get(), output.first.get(), output.second.get()})
            detail::spawn_check(::posix_spawn_file_actions_addclose(&actions, fd), "hash close");
        char executable[] = "sha256sum";
        char* arguments[]{executable, nullptr};
        const int error = ::posix_spawnp(&pid_, executable, &actions, nullptr, arguments, ::environ);
        if (error) throw ToolError("sha256sum: " + std::string(std::strerror(error)), error == ENOENT ? 127 : 126);
        output_ = std::move(output.first);
    }
    ~HashStream() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
    HashStream(const HashStream&) = delete;
    HashStream& operator=(const HashStream&) = delete;

    void append(std::string_view bytes) {
        while (!bytes.empty()) {
            const auto count = ::send(input_.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::system_error(count < 0 ? errno : EIO,
                std::generic_category(), "runtime package sha256sum input");
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
    }
    std::string finish() {
        input_.reset();
        std::string result;
        std::array<char, 256> bytes{};
        for (;;) {
            const auto count = ::read(output_.get(), bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::system_error(errno, std::generic_category(), "runtime package sha256sum output");
            if (!count) break;
            result.append(bytes.data(), static_cast<std::size_t>(count));
        }
        int status;
        while (::waitpid(pid_, &status, 0) < 0) {
            if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "runtime package sha256sum wait");
        }
        pid_ = -1;
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (code) throw ToolError("runtime package sha256sum failed (exit " + std::to_string(code) + ")", code);
        if (result.size() < 65 || result[64] != ' ' ||
            result.substr(0, 64).find_first_not_of("0123456789abcdef") != std::string::npos)
            throw ToolError("runtime package sha256sum returned an invalid digest");
        return result.substr(0, 64);
    }
private:
    FileDescriptor input_, output_;
    pid_t pid_ = -1;
};
} // namespace

std::span<const Row> inventory() noexcept { return rows; }

int inventory(const std::function<int(const Row&)>& callback) {
    for (const auto& row : rows)
        if (const int status = callback(row)) return status;
    return 0;
}

bool check_directory(const fs::path& root, Layout layout, const Row& row) {
    const auto directory = root / (layout == Layout::image
        ? fs::path(row.destination).relative_path() : fs::path(row.path));
    std::error_code error;
    if (!fs::is_directory(directory, error)) return false;
    for (const auto entry : row.entries) {
        if (entry.starts_with('?')) continue;
        if (!fs::exists(directory / entry, error)) return false;
    }
    if (row.path == "bin") {
        fs::directory_iterator iterator(directory, error), end;
        if (error) return false;
        for (; iterator != end; iterator.increment(error)) {
            if (error) return false;
            // Following symlinks matches find -L, including error rejection.
            const bool regular = iterator->is_regular_file(error);
            if (error && error != std::errc::no_such_file_or_directory) return false;
            const auto name = iterator->path().filename().string();
            if (regular && (name == "mmltk_acceptance" ||
                (name.size() >= 12 && name.starts_with("mmltk_") && name.ends_with("_tests"))) &&
                ::faccessat(AT_FDCWD, iterator->path().c_str(), X_OK, AT_EACCESS) == 0)
                return false;
        }
        if (error) return false;
    }
    return true;
}

bool complete(const fs::path& root, Layout layout) {
    return inventory([&](const Row& row) { return check_directory(root, layout, row) ? 0 : 1; }) == 0;
}

std::string path_fingerprint(const fs::path& root) {
    HashStream hash;
    std::error_code error;
    const bool exists = fs::exists(root, error);
    const bool link = fs::is_symlink(fs::symlink_status(root, error));
    if (!exists && !link) {
        hash.append("missing\n");
        return hash.finish();
    }
    fs::path directory;
    std::string member;
    if (fs::is_directory(root, error)) {
        if (link) {
            auto target = fs::read_symlink(root).string();
            // Bash command substitution removes all trailing newlines.
            while (!target.empty() && target.back() == '\n') target.pop_back();
            hash.append(std::string_view("root-link\0", 10));
            hash.append(target);
            hash.append(std::string_view("\0", 1));
        }
        directory = root;
        member = ".";
    } else {
        directory = root.parent_path();
        if (directory.empty()) directory = ".";
        member = root.filename().string();
    }
    RunOptions options;
    options.stdout_sink = [&](std::string_view bytes) { hash.append(bytes); };
    const auto result = run({"tar", "--format=gnu", "--sort=name", "--mtime=@0",
        "--owner=0", "--group=0", "--numeric-owner", "--hard-dereference",
        "--transform=flags=r;s,^[^/]*,entry,", "--create", "--file=-",
        "--directory=" + directory.string(), "--", member}, options);
    if (result.exit_code) throw ToolError("runtime package tar failed (exit " +
        std::to_string(result.exit_code) + ")", result.exit_code);
    return hash.finish();
}

std::string hash_directory(const fs::path& root, const Row& row) {
    return std::string(row.path) + " " + path_fingerprint(root / row.path) + "\n";
}

std::string fingerprint(const fs::path& root) {
    HashStream hash;
    for (const auto& row : inventory()) hash.append(hash_directory(root, row));
    return hash.finish();
}

int copy_directory(const fs::path& root, const Row& row) {
    const std::string destination(row.destination);
    if (const int status = run({"mkdir", "-p", "--", destination}).exit_code) return status;
    return run({"cp", "-a", "--no-preserve=ownership", "--",
        (root / row.path).string() + "/.", destination + "/"}).exit_code;
}

int install(const fs::path& root) {
    if (!complete(root)) {
        std::fputs("Packaged release is incomplete or contains test executables\n", stderr);
        return 1;
    }
    if (const int status = inventory([&](const Row& row) { return copy_directory(root, row); }))
        return status;
    return complete("/", Layout::image) ? 0 : 1;
}

} // namespace mmltk::tools::runtime_package

export int mmltk_tool_runtime_package(int argc, char** argv) {
    namespace package = mmltk::tools::runtime_package;
    using mmltk::tools::ToolError;
    try {
        if (argc < 2) throw ToolError("runtime-package requires an operation", 2);
        const std::string_view operation = argv[1];
        if (operation == "inventory") {
            if (argc != 2) throw ToolError("inventory takes no arguments", 2);
            // Preserve the wrapper's NUL-framed row protocol, including the
            // empty field terminating each row and optional-entry markers.
            return package::inventory([](const package::Row& row) {
                for (auto field : {row.path, row.destination, row.source_kind, row.source_path})
                    std::cout << field << '\0';
                for (auto field : row.entries) std::cout << field << '\0';
                std::cout << '\0';
                return std::cout ? 0 : 1;
            });
        }
        if (argc < 3) throw ToolError("runtime-package operation requires a root", 2);
        const std::filesystem::path root = argv[2];
        if (operation == "complete") {
            if (argc > 4 || (argc == 4 && std::string_view(argv[3]) != "stage" && std::string_view(argv[3]) != "image"))
                throw ToolError("complete expects ROOT [stage|image]", 2);
            return package::complete(root, argc == 4 && std::string_view(argv[3]) == "image" ? package::Layout::image : package::Layout::stage) ? 0 : 1;
        }
        if (argc != 3) throw ToolError("runtime-package operation expects one root", 2);
        if (operation == "path-fingerprint") std::cout << package::path_fingerprint(root) << '\n';
        else if (operation == "fingerprint") std::cout << package::fingerprint(root) << '\n';
        else if (operation == "install") return package::install(root);
        else throw ToolError("unknown runtime-package operation: " + std::string(operation), 2);
        return std::cout ? 0 : 1;
    } catch (const ToolError& error) {
        std::cerr << "runtime-package: " << error.what() << '\n';
        return error.status;
    } catch (const std::exception& error) {
        std::cerr << "runtime-package: " << error.what() << '\n';
        return 1;
    }
}
