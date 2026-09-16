#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>
#include "src/controller/subsystems/annotation/detail/annotation_document.h"
namespace mmltk::controller::subsystems::annotation::annotation_persistence {
// The transition owns ordering and classification; a backend owns only the
// platform effects. This keeps failure injection out of domain records.
template <class Backend>
concept AtomicSaveBackend = requires(Backend& backend, std::string_view path, std::span<const std::byte> bytes) {
    { backend.open_exclusive(path) } noexcept -> std::same_as<bool>;
    { backend.write_all(bytes) } noexcept -> std::same_as<bool>;
    { backend.sync_file() } noexcept -> std::same_as<bool>;
    { backend.close_file() } noexcept -> std::same_as<bool>;
    { backend.rename_file(path, path) } noexcept -> std::same_as<bool>;
    { backend.remove_file(path) } noexcept;
    { backend.sync_parent(path) } noexcept -> std::same_as<bool>;
};
template <AtomicSaveBackend Backend>
[[nodiscard]] DocumentSaveEffect AtomicSave(Backend& backend, const std::span<const std::byte> bytes, const std::string_view destination,
                                            const std::uint64_t document_revision, const std::uint64_t generation) noexcept {
    if (bytes.empty() || destination.empty() || generation == 0U) return DocumentSaveEffect::NotApplied;
    constexpr std::string_view marker{".mmltk-annotation-"};
    constexpr std::string_view suffix{".tmp"};
    std::array<char, domain::kWorkspacePathCapacity + marker.size() + 2U * 24U + suffix.size() + 2U> temporary{};
    if (destination.size() >= temporary.size()) return DocumentSaveEffect::NotApplied;
    auto* cursor = std::copy(destination.begin(), destination.end(), temporary.begin());
    cursor = std::copy(marker.begin(), marker.end(), cursor);
    const auto [revision_end, revision_error] = std::to_chars(cursor, temporary.data() + temporary.size() - suffix.size() - 1U, document_revision);
    if (revision_error != std::errc{}) return DocumentSaveEffect::NotApplied;
    *revision_end = '-';
    const auto [generation_end, generation_error] = std::to_chars(revision_end + 1, temporary.data() + temporary.size() - suffix.size() - 1U, generation);
    if (generation_error != std::errc{}) return DocumentSaveEffect::NotApplied;
    cursor = std::copy(suffix.begin(), suffix.end(), generation_end);
    const std::string_view temporary_path{temporary.data(), static_cast<std::size_t>(cursor - temporary.data())};
    if (!backend.open_exclusive(temporary_path)) return DocumentSaveEffect::NotApplied;
    const bool written = backend.write_all(bytes);
    const bool synced = written && backend.sync_file();
    const bool closed = backend.close_file();
    const bool prepared = written && synced && closed;
    if (!prepared || !backend.rename_file(temporary_path, destination)) {
        backend.remove_file(temporary_path);
        return DocumentSaveEffect::NotApplied;
    }
    return backend.sync_parent(destination) ? DocumentSaveEffect::Committed : DocumentSaveEffect::Uncertain;
}
}  // namespace mmltk::controller::subsystems::annotation::annotation_persistence
