#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

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
// throw ToolError (from cpp_support.hpp) or a standard filesystem/system error.
std::string path_fingerprint(const std::filesystem::path& root);
// The record includes its terminating newline, as in the shell callback.
std::string hash_directory(const std::filesystem::path& root, const Row& row);
std::string fingerprint(const std::filesystem::path& root);

// Copy merges the directory into its absolute image destination. No rollback
// is performed after a failure, matching the original installation library.
int copy_directory(const std::filesystem::path& root, const Row& row);
int install(const std::filesystem::path& root);

} // namespace mmltk::tools::runtime_package
