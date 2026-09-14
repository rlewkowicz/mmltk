#include "src/controller/subsystems/explore/detail/gallery_atlas.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include "src/common/types/generation.h"

namespace mmltk::controller::explore_detail {

ExploreAtlasLayout GalleryAtlas::Begin(const mmltk::frameworks::gpu::ImagePlaneView clean,
                                       const mmltk::frameworks::gpu::ImagePlaneView semantic, const ExploreViewport& viewport,
                                       const GalleryThumbnailCache::Identity& pixels) {
    Rollback();
    if (!clean.valid() || !semantic.valid() || clean.allocation.owner == 0U || semantic.allocation.owner == 0U ||
        clean.allocation.identity == 0U || semantic.allocation.identity == 0U || pixels.extent == 0U || viewport.columns == 0U ||
        viewport.row_count == 0U || clean.descriptor.width != static_cast<std::uint64_t>(viewport.columns) * pixels.extent ||
        clean.descriptor.height % pixels.extent != 0U || clean.descriptor.width != semantic.descriptor.width ||
        clean.descriptor.height != semantic.descriptor.height)
        throw std::invalid_argument("Explore atlas allocation/layout is invalid");
    auto found = std::ranges::find_if(allocations_, [&](const auto& value) { return value.clean.owner == clean.allocation.owner; });
    if (found == allocations_.end()) found = std::ranges::find_if(allocations_, [](const auto& value) { return value.clean.owner == 0U; });
    if (found == allocations_.end()) throw std::logic_error("Explore atlas exceeds its output pool");
    const auto rows = clean.descriptor.height / pixels.extent;
    if (rows < viewport.row_count || static_cast<std::uint64_t>(rows) * viewport.columns > kExploreVisibleItemCapacity)
        throw std::invalid_argument("Explore atlas lacks bounded visible row capacity");
    if (found->clean != clean.allocation || found->semantic != semantic.allocation || !found->pixels.SameSource(pixels) ||
        found->columns != viewport.columns || found->rows != rows) {
        found->cells.assign(static_cast<std::size_t>(rows) * viewport.columns, {});
        found->clean = clean.allocation;
        found->semantic = semantic.allocation;
        found->pixels = pixels;
        found->columns = viewport.columns;
        found->rows = rows;
        found->layout_generation = mmltk::common::types::advance_monotonic_identity(found->layout_generation);
        found->revision = 0U;
    }
    writes_.reserve(2U * found->cells.size());
    active_ = &*found;
    layout_ = {viewport.first_row, viewport.row_count, rows, viewport.first_row % rows, viewport.columns, pixels.extent};
    return layout_;
}
std::size_t GalleryAtlas::Physical(const std::size_t logical) const noexcept {
    return ((layout_.row_origin + logical / layout_.columns) % layout_.row_capacity) * layout_.columns + logical % layout_.columns;
}
bool GalleryAtlas::Contains(const std::size_t physical, const std::shared_ptr<const GalleryTileMeaning>& meaning,
                            const std::uint64_t semantics) const {
    const auto& cell = active_->cells.at(physical);
    return cell.valid && cell.meaning == meaning && cell.semantics == semantics && !cell.placeholder;
}
bool GalleryAtlas::Empty(const std::size_t physical, const bool placeholder) const {
    const auto& cell = active_->cells.at(physical);
    return cell.valid && !cell.meaning && cell.placeholder == placeholder;
}
bool GalleryAtlas::ContainsClean(const std::size_t physical, const std::shared_ptr<const GalleryTileMeaning>& meaning) const {
    const auto& cell = active_->cells.at(physical);
    return cell.valid && cell.meaning == meaning && !cell.placeholder;
}
void GalleryAtlas::Touch(const std::size_t physical) { active_->cells.at(physical) = {}; }
void GalleryAtlas::Stage(const std::size_t physical, std::shared_ptr<const GalleryTileMeaning> meaning, const std::uint64_t semantics,
                         const bool placeholder) {
    writes_.push_back({physical, {std::move(meaning), semantics, true, placeholder}});
}
mmltk::frameworks::gpu::ImageWorkspaceCoverage GalleryAtlas::WorkspaceCoverage(
    const mmltk::frameworks::gpu::ImageWorkspaceObservation& output) {
    if (!active_ || active_->clean.owner != output.product_owner) return {};
    pending_revision_ = output.product_revision;
    coverage_.clear();
    coverage_.reserve(active_->cells.size());
    for (const auto& write : writes_) {
        const auto x = static_cast<std::int32_t>((write.physical % layout_.columns) * layout_.card_extent);
        const auto y = static_cast<std::int32_t>((write.physical / layout_.columns) * layout_.card_extent);
        const auto extent = static_cast<std::int32_t>(layout_.card_extent);
        coverage_.push_back({x, y, x + extent, y + extent});
    }
    return {output.workspace ? output.workspace->identity() : 0U, coverage_, false, {output.product_owner, active_->revision}};
}
void GalleryAtlas::Commit() noexcept {
    if (active_) active_->revision = pending_revision_;
    if (active_)
        for (auto& write : writes_)
            active_->cells[write.physical] = std::move(write.cell);
    Rollback();
}
void GalleryAtlas::Rollback() noexcept {
    writes_.clear();
    coverage_.clear();
    active_ = nullptr;
    pending_revision_ = 0U;
}
void GalleryAtlas::Invalidate(const mmltk::frameworks::gpu::ImageAllocation facts) noexcept {
    if (facts.owner == 0U || facts.identity == 0U) return;
    for (auto& allocation : allocations_)
        if (allocation.clean.owner == facts.owner) std::ranges::fill(allocation.cells, Cell{});
}
void GalleryAtlas::Clear() noexcept {
    Rollback();
    for (auto& allocation : allocations_)
        allocation = {};
    layout_ = {};
}
std::size_t GalleryAtlas::MetadataBytes() const noexcept {
    std::size_t bytes = writes_.capacity() * sizeof(Write) + coverage_.capacity() * sizeof(mmltk::frameworks::gpu::ImageWorkspaceRegion);
    for (const auto& allocation : allocations_)
        bytes += allocation.cells.capacity() * sizeof(Cell);
    return bytes;
}
std::size_t GalleryAtlas::AppendMeanings(const std::span<std::shared_ptr<const GalleryTileMeaning>> target) const {
    std::size_t count = 0U;
    const auto append = [&](const Cell& cell) {
        if (!cell.meaning) return;
        if (count == target.size()) throw std::logic_error("Explore atlas meaning inventory exceeds capacity");
        target[count++] = cell.meaning;
    };
    for (const auto& allocation : allocations_)
        for (const auto& cell : allocation.cells)
            append(cell);
    for (const auto& write : writes_)
        append(write.cell);
    return count;
}

}  // namespace mmltk::controller::explore_detail
