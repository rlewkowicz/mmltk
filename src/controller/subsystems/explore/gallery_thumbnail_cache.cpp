#include "src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h"

#include <algorithm>
#include <utility>
#include <array>
#include <functional>
#include <span>
#include <stdexcept>

namespace mmltk::controller::explore_detail {

std::size_t GalleryThumbnailCache::CardCount(const std::size_t matching, const std::uint32_t rows, const std::uint32_t columns) noexcept {
    return std::min(matching, (static_cast<std::size_t>(rows) + 14U) * columns);
}

std::size_t GalleryThumbnailCache::WindowFirst(const std::size_t matching, const ExploreViewport& viewport) noexcept {
    const auto first = static_cast<std::size_t>(viewport.first_row) * viewport.columns;
    const auto behind = 7U * static_cast<std::size_t>(viewport.columns);
    return std::min(first > behind ? first - behind : 0U, matching - CardCount(matching, viewport.row_count, viewport.columns));
}

void GalleryThumbnailCache::Configure(const std::size_t count, Identity identity) {
    if (count > kMaximumCards) throw std::invalid_argument("Explore cache exceeds the viewport row bound");
    if (identity_ == identity && entries_.size() == count) return;
    entries_.clear();
    entries_.resize(count);
    identity_ = std::move(identity);
}
void GalleryThumbnailCache::Reserve(const std::size_t count) {
    if (count > kMaximumCards) throw std::invalid_argument("Explore cache exceeds the viewport row bound");
    entries_.reserve(count);
}

void GalleryThumbnailCache::Clear() noexcept {
    entries_.clear();
    identity_ = {};
}

const GalleryThumbnailCache::Entry* GalleryThumbnailCache::Find(const std::size_t position,
                                                                const std::uint32_t compiled_index) const noexcept {
    if (entries_.empty()) return nullptr;
    const auto& entry = entries_[Slot(position)];
    return entry.position == position && entry.compiled_index == compiled_index && entry.meaning ? &entry : nullptr;
}

void GalleryThumbnailCache::Complete(const std::size_t position, const std::uint32_t compiled_index,
                                     std::shared_ptr<const GalleryTileMeaning> meaning, const std::uint64_t semantic_identity,
                                     const std::uint8_t bank, const std::uint8_t semantic_bank) {
    if (bank > 1U || semantic_bank > 1U) throw std::invalid_argument("Explore cache plane version is invalid");
    entries_.at(Slot(position)) = {.position = position,
                                   .compiled_index = compiled_index,
                                   .bank = bank,
                                   .semantic_bank = semantic_bank,
                                   .meaning = std::move(meaning),
                                   .semantic_identity = semantic_identity};
}

std::size_t GalleryThumbnailCache::MeaningBytes(
    const GalleryThumbnailCache* other, const std::span<const std::shared_ptr<const GalleryTileMeaning>> additional_meanings) const {
    if (additional_meanings.size() > 2U * kMaximumCards)
        throw std::length_error("Explore retained meaning accounting exceeds the owner bound");
    std::size_t bytes = entries_.capacity() * sizeof(Entry);
    struct Allocation {
        const void* identity;
        std::size_t bytes;
    };
    std::array<Allocation, 4U * kMaximumCards> allocations;
    std::size_t count = 0U;
    const auto meaning = [&](const auto& value) {
        if (value)
            allocations[count++] = {value.get(),
                                    GallerySharedBytes(value) +
                                        value->annotations.capacity() * sizeof(decltype(GalleryTileMeaning::annotations)::value_type) +
                                        value->runs.capacity() * sizeof(decltype(GalleryTileMeaning::runs)::value_type)};
    };
    const auto visit = [&](const auto& entries) {
        for (const auto& entry : entries) {
            meaning(entry.meaning);
        }
    };
    visit(entries_);
    if (other && other != this) {
        bytes += other->entries_.capacity() * sizeof(Entry);
        visit(other->entries_);
    }
    for (const auto& value : additional_meanings)
        meaning(value);
    auto retained = std::span{allocations}.first(count);
    std::ranges::sort(retained,
                      [](const auto& left, const auto& right) { return std::less<const void*>{}(left.identity, right.identity); });
    const void* previous = nullptr;
    for (const auto& allocation : retained) {
        if (allocation.identity != previous) bytes += allocation.bytes;
        previous = allocation.identity;
    }
    return bytes;
}

}  // namespace mmltk::controller::explore_detail
