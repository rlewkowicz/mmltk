#include "src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h"
#include <algorithm>
#include <utility>
#include <array>
#include <functional>
#include <span>
#include <stdexcept>
namespace mmltk::controller::explore_detail {
std::size_t GalleryThumbnailCache::WindowFirst(const std::size_t matching, const ExploreViewport& viewport) noexcept {
 const auto row = viewport.first_row > kNeighborRows ? viewport.first_row - kNeighborRows : 0U;
 return std::min(matching, static_cast<std::size_t>(row) * viewport.columns);
}
std::size_t GalleryThumbnailCache::WindowCount(const std::size_t matching, const ExploreViewport& viewport) noexcept {
 const auto end = std::min(matching, (static_cast<std::size_t>(viewport.first_row) + viewport.row_count + kNeighborRows) * viewport.columns);
 return end - WindowFirst(matching, viewport);
}
void GalleryThumbnailCache::Configure(const std::size_t count, Identity identity) {
 if (count > kMaximumCards) throw std::invalid_argument("Explore cache exceeds the viewport row bound");
 if (identity_ == identity && count <= entries_.size()) return;
 if (identity_ != identity) {
  if (identity_.SameSource(identity)) {
   for (auto& entry : entries_) entry.refresh_pending = true;
  } else
   entries_.clear();
  identity_ = std::move(identity);
  demand_slots_.clear();
  std::ranges::fill(pinned_, false);
 }
 if (entries_.size() < count) entries_.resize(count);
 Reserve(entries_.size());
 Reindex();
}
void GalleryThumbnailCache::Reserve(const std::size_t count) {
 if (count > kMaximumCards) throw std::invalid_argument("Explore cache exceeds the viewport row bound");
 entries_.reserve(count);
 buckets_.reserve(2U * count + 1U);
 next_.reserve(count);
 pinned_.reserve(count);
 demand_slots_.reserve(count);
 prior_demand_.reserve(count);
 undo_.reserve(count);
 undo_indices_.reserve(count);
}
void GalleryThumbnailCache::Reindex() {
 buckets_.assign(2U * entries_.size() + 1U, absent);
 next_.assign(entries_.size(), absent);
 pinned_.resize(entries_.size());
 undo_indices_.resize(entries_.size(), absent);
 for (std::size_t slot = 0U; slot < entries_.size(); ++slot) {
  if (entries_[slot].position == absent) continue;
  auto& head = buckets_[entries_[slot].compiled_index % buckets_.size()];
  next_[slot] = head;
  head = slot;
 }
}
std::size_t GalleryThumbnailCache::Lookup(const std::uint32_t image) const noexcept {
 if (buckets_.empty()) return absent;
 for (auto slot = buckets_[image % buckets_.size()]; slot != absent; slot = next_[slot])
  if (entries_[slot].compiled_index == image) return slot;
 return absent;
}
void GalleryThumbnailCache::Erase(const std::size_t slot) noexcept {
 if (entries_[slot].position == absent) return;
 auto* link = &buckets_[entries_[slot].compiled_index % buckets_.size()];
 while (*link != slot) link = &next_[*link];
 *link = next_[slot];
}
std::size_t GalleryThumbnailCache::Assign(const std::uint32_t image) {
 if (const auto found = Lookup(image); found != absent) return found;
 for (std::size_t attempts = 0U; attempts < entries_.size(); ++attempts) {
  const auto slot = eviction_++ % entries_.size();
  if (pinned_[slot]) continue;
  Save(slot);
  Erase(slot);
  entries_[slot] = {.position = 0U, .compiled_index = image};
  auto& head = buckets_[image % buckets_.size()];
  next_[slot] = head;
  head = slot;
  return slot;
 }
 throw std::logic_error("Explore cache demand exceeds retained physical slots");
}
void GalleryThumbnailCache::Admit(const std::span<const std::uint32_t> images, const std::size_t first) {
 if (images.size() > entries_.size()) throw std::length_error("Explore cache demand exceeds capacity");
 std::ranges::fill(pinned_, false);
 // Pin all hits before any eviction, including a hit later in the demand.
 for (const auto image : images)
  if (const auto slot = Lookup(image); slot != absent) pinned_[slot] = true;
 demand_slots_.resize(images.size());
 demand_first_ = first;
 for (std::size_t offset = 0U; offset < images.size(); ++offset) {
  const auto slot = Assign(images[offset]);
  pinned_[slot] = true;
  if (entries_[slot].position != first + offset) Save(slot);
  entries_[slot].position = first + offset;
  demand_slots_[offset] = slot;
 }
}
std::size_t GalleryThumbnailCache::Position(const std::uint32_t image) const noexcept {
 const auto slot = Lookup(image);
 if (slot == absent || !pinned_[slot]) return absent;
 return entries_[slot].position;
}
void GalleryThumbnailCache::Clear() noexcept {
 CommitUpdate();
 entries_.clear();
 buckets_.clear();
 next_.clear();
 pinned_.clear();
 demand_slots_.clear();
 identity_ = {};
 eviction_ = 0U;
}
const GalleryThumbnailCache::Entry* GalleryThumbnailCache::Find(const std::uint32_t compiled_index) const noexcept {
 const auto* entry = Retained(compiled_index);
 return entry && !entry->refresh_pending ? entry : nullptr;
}
const GalleryThumbnailCache::Entry* GalleryThumbnailCache::Retained(const std::uint32_t compiled_index) const noexcept {
 const auto slot = Lookup(compiled_index);
 return slot != absent && entries_[slot].meaning ? &entries_[slot] : nullptr;
}
void GalleryThumbnailCache::Restore(const std::size_t slot, Entry entry) noexcept {
 Erase(slot);
 entries_[slot] = std::move(entry);
 if (entries_[slot].position == absent) return;
 auto& head = buckets_[entries_[slot].compiled_index % buckets_.size()];
 next_[slot] = head;
 head = slot;
}
void GalleryThumbnailCache::Complete(const std::size_t position, const std::uint32_t compiled_index, std::shared_ptr<const GalleryTileMeaning> meaning, const std::uint64_t semantic_identity,
 const std::uint8_t bank, const std::uint8_t semantic_bank) {
 if (bank > 1U || semantic_bank > 1U) throw std::invalid_argument("Explore cache plane version is invalid");
 const auto slot = Assign(compiled_index);
 Save(slot);
 entries_.at(slot) = {.position = position, .compiled_index = compiled_index, .bank = bank, .semantic_bank = semantic_bank, .meaning = std::move(meaning), .semantic_identity = semantic_identity};
}
void GalleryThumbnailCache::UpdateSemantics(const std::size_t position, const std::uint64_t semantic_identity, const std::uint8_t semantic_bank) {
 if (semantic_bank > 1U) throw std::invalid_argument("Explore cache plane version is invalid");
 const auto slot = Slot(position);
 auto& entry = entries_.at(slot);
 if (!entry.meaning) throw std::logic_error("Explore semantic refresh requires retained clean pixels");
 Save(slot);
 entry.semantic_identity = semantic_identity;
 entry.semantic_bank = semantic_bank;
}
std::size_t GalleryThumbnailCache::MetadataBytes() const noexcept {
 return entries_.capacity() * sizeof(Entry) + undo_.capacity() * sizeof(Undo) +
        (buckets_.capacity() + next_.capacity() + demand_slots_.capacity() + prior_demand_.capacity() + undo_indices_.capacity()) * sizeof(std::size_t) + (pinned_.capacity() + 7U) / 8U;
}
void GalleryThumbnailCache::BeginUpdate() {
 if (updating_) throw std::logic_error("Explore cache update is already active");
 Reserve(entries_.size());
 prior_demand_.assign(demand_slots_.begin(), demand_slots_.end());
 prior_first_ = demand_first_;
 updating_ = true;
}
void GalleryThumbnailCache::Save(const std::size_t slot) {
 if (!updating_ || undo_indices_[slot] != absent) return;
 undo_indices_[slot] = undo_.size();
 undo_.push_back({slot, entries_[slot]});
}
const GalleryThumbnailCache::Entry& GalleryThumbnailCache::Protected(const std::size_t slot) const noexcept {
 return updating_ && undo_indices_[slot] != absent ? undo_[undo_indices_[slot]].entry : entries_[slot];
}
void GalleryThumbnailCache::CommitUpdate() noexcept {
 for (const auto& undo : undo_) undo_indices_[undo.slot] = absent;
 undo_.clear();
 prior_demand_.clear();
 updating_ = false;
}
void GalleryThumbnailCache::RollbackUpdate() noexcept {
 if (!updating_) return;
 for (auto& undo : undo_) Restore(undo.slot, std::move(undo.entry));
 demand_slots_.swap(prior_demand_);
 demand_first_ = prior_first_;
 std::ranges::fill(pinned_, false);
 for (const auto slot : demand_slots_) pinned_[slot] = true;
 CommitUpdate();
}
std::size_t GalleryThumbnailCache::MeaningBytes(const GalleryThumbnailCache* other, const std::span<const std::shared_ptr<const GalleryTileMeaning>> additional_meanings) const {
 if (additional_meanings.size() > 2U * kMaximumCards) throw std::length_error("Explore retained meaning accounting exceeds the owner bound");
 std::size_t bytes = MetadataBytes();
 struct Allocation {
  const void* identity;
  std::size_t bytes;
 };
 std::array<Allocation, 6U * kMaximumCards> allocations;
 std::size_t count = 0U;
 const auto meaning = [&](const auto& value) {
  if (value)
   allocations[count++] = {value.get(), GallerySharedBytes(value) + value->annotations.capacity() * sizeof(decltype(GalleryTileMeaning::annotations)::value_type) +
                                         value->runs.capacity() * sizeof(decltype(GalleryTileMeaning::runs)::value_type)};
 };
 const auto visit = [&](const auto& entries) {
  for (const auto& entry : entries) { meaning(entry.meaning); }
 };
 visit(entries_);
 for (const auto& undo : undo_) meaning(undo.entry.meaning);
 if (other && other != this) {
  bytes += other->MetadataBytes();
  visit(other->entries_);
  for (const auto& undo : other->undo_) meaning(undo.entry.meaning);
 }
 for (const auto& value : additional_meanings) meaning(value);
 auto retained = std::span{allocations}.first(count);
 std::ranges::sort(retained, [](const auto& left, const auto& right) { return std::less<const void*>{}(left.identity, right.identity); });
 const void* previous = nullptr;
 for (const auto& allocation : retained) {
  if (allocation.identity != previous) bytes += allocation.bytes;
  previous = allocation.identity;
 }
 return bytes;
}
std::uint8_t GalleryThumbnailCache::WritableBank(std::size_t position, const GalleryThumbnailCache* incumbent, bool semantic) const {
 const auto slot = Slot(position);
 if (incumbent && slot < incumbent->size()) {
  const auto& entry = incumbent->Physical(slot);
  if (entry.meaning) return 1U - (semantic ? entry.semantic_bank : entry.bank);
 }
 const auto& entry = Protected(slot);
 return entry.meaning ? 1U - (semantic ? entry.semantic_bank : entry.bank) : 0U;
}
}  // namespace mmltk::controller::explore_detail
