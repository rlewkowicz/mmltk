#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace {
constexpr std::string_view kProtocolMarkerPrefix = "MMLTK_HOST_API_PROTOCOL_";
constexpr std::string_view kWasmSuffix = "_bg.wasm";
[[nodiscard]] bool ascii_digit(const unsigned char byte) { return byte >= '0' && byte <= '9'; }
[[nodiscard]] bool ascii_hex_digit(const unsigned char byte) { return ascii_digit(byte) || (byte >= 'A' && byte <= 'F') || (byte >= 'a' && byte <= 'f'); }
[[nodiscard]] bool path_exists(const char* path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) { throw std::filesystem::filesystem_error("failed to inspect browser distribution path", path, error); }
    return exists;
}
[[nodiscard]] std::string read_protocol_marker(const char* marker_path) {
    std::ifstream input(marker_path, std::ios::binary);
    if (!input) { throw std::runtime_error("could not read generated native protocol marker"); }
    std::string marker{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (!marker.starts_with(kProtocolMarkerPrefix) || marker.size() == kProtocolMarkerPrefix.size() ||
        !std::ranges::all_of(marker.substr(kProtocolMarkerPrefix.size()), ascii_digit)) {
        throw std::runtime_error("generated native protocol marker is malformed");
    }
    return marker;
}
[[nodiscard]] bool asset_character(const unsigned char byte) {
    return ascii_digit(byte) || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || byte == '_' || byte == '-' || byte == '.' || byte == '/';
}
[[nodiscard]] bool hashed_wasm_name(const std::filesystem::path& path) {
    const std::string filename = path.filename();
    if (!filename.ends_with(kWasmSuffix)) { return false; }
    const std::string_view prefix(filename.data(), filename.size() - kWasmSuffix.size());
    const std::size_t separator = prefix.rfind('-');
    if (separator == std::string_view::npos || separator == 0U || separator + 9U > prefix.size()) { return false; }
    return std::ranges::all_of(prefix.substr(separator + 1U), ascii_hex_digit);
}
[[nodiscard]] std::string selected_wasm_reference(const std::string_view index) {
    std::set<std::string> references;
    std::size_t accepted_occurrences = 0U;
    for (std::size_t cursor = 0U; cursor != index.size();) {
        if (index[cursor] == '\\') {
            cursor += std::min<std::size_t>(2U, index.size() - cursor);
            continue;
        }
        const char quote = index[cursor];
        if (quote != '\'' && quote != '"') {
            ++cursor;
            continue;
        }
        const std::size_t content_begin = cursor + 1U;
        std::size_t content_end = content_begin;
        bool has_escape = false;
        while (content_end != index.size() && index[content_end] != quote) {
            if (index[content_end] == '\\') {
                has_escape = true;
                content_end += std::min<std::size_t>(2U, index.size() - content_end);
            } else {
                ++content_end;
            }
        }
        if (content_end == index.size()) { break; }
        const std::string_view token = index.substr(content_begin, content_end - content_begin);
        const std::size_t suffix_position = token.find(kWasmSuffix);
        if (suffix_position != std::string_view::npos) {
            if (has_escape || token.find(kWasmSuffix, suffix_position + kWasmSuffix.size()) != std::string_view::npos ||
                !std::ranges::all_of(token, [](const unsigned char byte) { return asset_character(byte); })) {
                throw std::runtime_error("browser index contains an escaped, unsupported, or ambiguous WebAssembly URL");
            }
            references.emplace(token);
            ++accepted_occurrences;
        }
        cursor = content_end + 1U;
    }
    std::size_t suffix_occurrences = 0U;
    for (std::size_t suffix_position = index.find(kWasmSuffix); suffix_position != std::string_view::npos;
         suffix_position = index.find(kWasmSuffix, suffix_position + kWasmSuffix.size())) {
        ++suffix_occurrences;
    }
    if (suffix_occurrences == 0U || suffix_occurrences != accepted_occurrences || references.size() != 1U) {
        throw std::runtime_error("browser index must select exactly one WebAssembly URL");
    }
    return *references.begin();
}
[[nodiscard]] std::filesystem::path admitted_relative_wasm(std::string reference) {
    if (reference.starts_with("//")) { throw std::runtime_error("browser index selects a network-path WebAssembly URL"); }
    if (reference.starts_with('/')) { reference.erase(reference.begin()); }
    const std::filesystem::path relative(reference);
    if (relative.empty() || relative.is_absolute() || !hashed_wasm_name(relative)) { throw std::runtime_error("browser index has an invalid WebAssembly URL"); }
    for (const auto& component : relative) {
        if (component == "." || component == "..") { throw std::runtime_error("browser index WebAssembly URL escapes its asset root"); }
    }
    return relative.lexically_normal();
}
[[nodiscard]] bool contained_by(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_component = root.begin();
    auto candidate_component = candidate.begin();
    while (root_component != root.end() && candidate_component != candidate.end() && *root_component == *candidate_component) {
        ++root_component;
        ++candidate_component;
    }
    return root_component == root.end() && candidate_component != candidate.end();
}
[[nodiscard]] std::filesystem::path selected_wasm(const std::filesystem::path& asset_root) {
    const std::filesystem::path index_path = asset_root / "index.html";
    std::ifstream input(index_path, std::ios::binary);
    if (!input) { throw std::runtime_error("browser bundle index is missing"); }
    const std::string index{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const std::filesystem::path relative = admitted_relative_wasm(selected_wasm_reference(index));
    const std::filesystem::path candidate = asset_root / relative;
    const std::filesystem::file_status selected_status = std::filesystem::symlink_status(candidate);
    if (selected_status.type() != std::filesystem::file_type::regular) {
        throw std::runtime_error("selected browser WebAssembly entry is not a non-symlink regular file");
    }
    std::size_t wasm_count = 0U;
    bool selected_path_found = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(asset_root)) {
        if (entry.path().extension() == ".wasm") {
            ++wasm_count;
            selected_path_found = selected_path_found || entry.path().lexically_normal() == candidate.lexically_normal();
        }
    }
    if (wasm_count != 1U || !selected_path_found) { throw std::runtime_error("browser bundle must contain only its selected WebAssembly pathname"); }
    const std::filesystem::path canonical_root = std::filesystem::canonical(asset_root);
    const std::filesystem::path canonical_candidate = std::filesystem::canonical(candidate);
    if (!contained_by(canonical_root, canonical_candidate)) { throw std::runtime_error("selected browser WebAssembly entry resolves outside its asset root"); }
    return canonical_candidate;
}
void validate_protocol_marker(const char* asset_root, const std::string_view marker) {
    const std::filesystem::path selected = selected_wasm(asset_root);
    std::ifstream input(selected, std::ios::binary);
    if (!input) { throw std::runtime_error("could not read selected browser WebAssembly bundle"); }
    const std::vector<char> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    std::size_t marker_count = 0U;
    std::size_t matching_marker_count = 0U;
    auto cursor = bytes.begin();
    while ((cursor = std::search(cursor, bytes.end(), kProtocolMarkerPrefix.begin(), kProtocolMarkerPrefix.end())) != bytes.end()) {
        const auto digits_begin = cursor + static_cast<std::ptrdiff_t>(kProtocolMarkerPrefix.size());
        auto digits_end = digits_begin;
        while (digits_end != bytes.end() && ascii_digit(static_cast<unsigned char>(*digits_end))) { ++digits_end; }
        if (digits_end != digits_begin) {
            ++marker_count;
            const std::string_view candidate(&*cursor, static_cast<std::size_t>(digits_end - cursor));
            matching_marker_count += candidate == marker ? 1U : 0U;
        }
        cursor = digits_end == cursor ? cursor + 1 : digits_end;
    }
    if (marker_count != 1U || matching_marker_count != 1U) {
        throw std::runtime_error("selected browser WebAssembly bundle has an invalid native protocol marker");
    }
}
void verify(const char* asset_root, const char* protocol_marker_path) {
    if (!path_exists(asset_root) || !std::filesystem::is_directory(asset_root)) {
        throw std::runtime_error("browser distribution is missing or is not a directory");
    }
    validate_protocol_marker(asset_root, read_protocol_marker(protocol_marker_path));
}
void publish(const char* staged_path, const char* current_path, const char* protocol_marker_path) {
    verify(staged_path, protocol_marker_path);
    if (!path_exists(current_path)) {
        if (::rename(staged_path, current_path) != 0) {
            throw std::runtime_error(std::string("initial browser distribution rename failed: ") + std::strerror(errno));
        }
        return;
    }
    if (::syscall(SYS_renameat2, AT_FDCWD, staged_path, AT_FDCWD, current_path, RENAME_EXCHANGE) != 0) {
        throw std::runtime_error(std::string("atomic browser distribution exchange failed: ") + std::strerror(errno));
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(staged_path, cleanup_error);
    if (cleanup_error) {
        std::cerr << "mmltk-browser-bundle-contract: published bundle but could not remove previous bundle at " << staged_path << ": "
                  << cleanup_error.message() << '\n';
    }
}
}  // namespace
int main(const int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: mmltk-browser-bundle-contract publish STAGED_PATH CURRENT_PATH PROTOCOL_MARKER_FILE\n"
                     "       mmltk-browser-bundle-contract verify ASSET_ROOT PROTOCOL_MARKER_FILE\n";
        return 2;
    }
    try {
        const std::string_view mode(argv[1]);
        if (mode == "publish" && argc == 5) {
            publish(argv[2], argv[3], argv[4]);
        } else if (mode == "verify" && argc == 4) {
            verify(argv[2], argv[3]);
        } else {
            std::cerr << "usage: mmltk-browser-bundle-contract publish STAGED_PATH CURRENT_PATH PROTOCOL_MARKER_FILE\n"
                         "       mmltk-browser-bundle-contract verify ASSET_ROOT PROTOCOL_MARKER_FILE\n";
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mmltk-browser-bundle-contract: " << error.what() << " (mode=" << std::string_view(argv[1]) << ")\n";
        return 1;
    }
}
