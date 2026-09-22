#include "src/controller/subsystems/annotation/detail/annotation_mask.h"
#include "src/controller/contracts/annotation_limits.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>
namespace mmltk::controller::subsystems::annotation {
namespace c = contracts;
namespace {
using Runs = std::vector<c::AnnotationMaskRun>;
void normalize(Runs& runs) {
 std::ranges::sort(runs);
 std::size_t count = 0;
 for (auto run : runs) {
  if (count && runs[count - 1].row == run.row && static_cast<unsigned>(runs[count - 1].last) + 1 >= run.first)
   runs[count - 1].last = std::max(runs[count - 1].last, run.last);
  else
   runs[count++] = run;
 }
 runs.resize(count);
 if (count > c::kAnnotationMaskRunCapacity) throw std::length_error("Mask run capacity exceeded");
}
Runs complement(const Runs& runs, std::uint16_t width, std::uint16_t height) {
 Runs result;
 std::size_t next = 0;
 for (unsigned y = 0; y < height; ++y) {
  unsigned x = 0;
  while (next < runs.size() && runs[next].row == y) {
   auto run = runs[next++];
   if (run.first > x) result.push_back({static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(run.first - 1)});
   x = static_cast<unsigned>(run.last) + 1;
  }
  if (x < width) result.push_back({static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(width - 1)});
 }
 return result;
}
// Connected components are resolved between adjacent rows, without allocating an image bitmap.
struct Components {
 std::vector<std::size_t> parent;
 explicit Components(const Runs& runs) : parent(runs.size()) {
  std::iota(parent.begin(), parent.end(), 0);
  std::size_t previous_begin = 0, previous_end = 0, begin = 0;
  while (begin < runs.size()) {
   auto end = begin + 1;
   while (end < runs.size() && runs[end].row == runs[begin].row) ++end;
   if (previous_end && static_cast<unsigned>(runs[previous_begin].row) + 1 == runs[begin].row) {
    auto left = previous_begin;
    for (auto index = begin; index < end; ++index) {
     while (left < previous_end && runs[left].last < runs[index].first) ++left;
     for (auto other = left; other < previous_end && runs[other].first <= runs[index].last; ++other) parent[root(index)] = root(other);
    }
   }
   previous_begin = begin;
   previous_end = end;
   begin = end;
  }
 }
 std::size_t root(std::size_t index) {
  while (parent[index] != index) {
   parent[index] = parent[parent[index]];
   index = parent[index];
  }
  return index;
 }
};
class DiskReach final {
public:
 explicit DiskReach(std::uint16_t radius) : radius_(radius) { reach_.fill(-1); }
 int At(int dy) {
  auto& reach = reach_[static_cast<std::size_t>(std::abs(dy))];
  if (reach < 0) reach = static_cast<int>(std::sqrt(static_cast<double>(radius_) * radius_ - dy * dy));
  return reach;
 }

private:
 std::uint16_t radius_;
 std::array<int, c::kMaxAnnotationMaskCleanupRadius + 1> reach_;
};
Runs dilate(const Runs& runs, std::uint16_t radius, std::uint16_t width, std::uint16_t height, DiskReach& disk) {
 Runs result;
 for (auto run : runs) {
  const int low = std::max(0, static_cast<int>(run.row) - radius);
  const int high = std::min(static_cast<int>(height) - 1, static_cast<int>(run.row) + radius);
  for (int y = low; y <= high; ++y) {
   const int dy = y - run.row;
   const int reach = disk.At(dy);
   result.push_back({static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(std::max(0, static_cast<int>(run.first) - reach)),
    static_cast<std::uint16_t>(std::min(static_cast<int>(width) - 1, static_cast<int>(run.last) + reach))});
  }
  // Bound transient storage while consolidating actual support.
  if (result.size() > c::kAnnotationMaskRunCapacity * 2) normalize(result);
 }
 normalize(result);
 return result;
}
}  // namespace
namespace {
void update_mask_bounds(c::AnnotationObject& object) {
 object.mask.present = !object.mask.runs.empty();
 object.box = {};
 if (!object.mask.present) return;
 auto first = object.mask.runs.front().first, last = object.mask.runs.front().last;
 for (auto run : object.mask.runs) {
  first = std::min(first, run.first);
  last = std::max(last, run.last);
 }
 object.box = {{static_cast<float>(first), static_cast<float>(object.mask.runs.front().row)}, {static_cast<float>(last) + 1, static_cast<float>(object.mask.runs.back().row) + 1}};
}
}  // namespace
void normalize_mask(c::AnnotationObject& object) {
 normalize(object.mask.runs);
 update_mask_bounds(object);
}
void MaskRows::Assign(const c::AnnotationObject& object) {
 if (!index_ || !index_.unique()) index_ = std::make_shared<Index>();
 for (auto& block : index_->blocks) {
  if (!block) continue;
  if (!block.unique()) block = std::make_shared<Block>();
  for (auto& row : block->rows) {
   if (row && row.unique())
    row->clear();
   else
    row.reset();
  }
 }
 index_->run_count = 0U;
 for (const auto run : object.mask.runs) {
  const auto block_index = run.row / kRowsPerBlock;
  if (index_->blocks.size() <= block_index) index_->blocks.resize(block_index + 1U);
  auto& block = index_->blocks[block_index];
  if (!block) block = std::make_shared<Block>();
  auto& row = block->rows[run.row % kRowsPerBlock];
  if (!row) row = std::make_shared<Runs>();
  row->push_back(run);
  ++index_->run_count;
 }
}
void MaskRows::Materialize(c::AnnotationObject& object) const {
 auto& runs = object.mask.runs;
 runs.clear();
 if (index_) {
  runs.reserve(index_->run_count);
  for (const auto& block : index_->blocks) {
   if (!block) continue;
   for (const auto& row : block->rows)
    if (row) runs.insert(runs.end(), row->begin(), row->end());
  }
 }
 update_mask_bounds(object);
}
void MaskRows::Stroke(c::AnnotationPoint from, c::AnnotationPoint to, std::uint16_t radius, std::uint16_t width, std::uint16_t height, bool erase, MaskScratch& scratch) {
 auto& stroke = scratch.stroke;
 stroke.clear();
 const float dx = to.x - from.x, dy = to.y - from.y;
 const auto steps = static_cast<unsigned>(std::ceil(std::max(std::abs(dx), std::abs(dy))));
 for (unsigned step = 0; step <= steps; ++step) {
  const float t = steps ? static_cast<float>(step) / static_cast<float>(steps) : 0;
  const float x = from.x + t * dx, y = from.y + t * dy;
  const int low = std::max(0, static_cast<int>(std::floor(y - radius)));
  const int high = std::min(static_cast<int>(height) - 1, static_cast<int>(std::ceil(y + radius)));
  for (int row = low; row <= high; ++row) {
   const float distance = static_cast<float>(row) + 0.5F - y;
   if (std::abs(distance) > radius) continue;
   const float reach = std::sqrt(static_cast<float>(radius) * radius - distance * distance);
   const int first = std::max(0, static_cast<int>(std::ceil(x - reach - 0.5F)));
   const int last = std::min(static_cast<int>(width) - 1, static_cast<int>(std::floor(x + reach - 0.5F)));
   if (first <= last) stroke.push_back({static_cast<std::uint16_t>(row), static_cast<std::uint16_t>(first), static_cast<std::uint16_t>(last)});
  }
  if (stroke.size() > c::kAnnotationMaskRunCapacity * 2) normalize(stroke);
 }
 normalize(stroke);
 if (stroke.empty()) return;
 if (!index_)
  index_ = std::make_shared<Index>();
 else if (!index_.unique())
  index_ = std::make_shared<Index>(*index_);
 for (std::size_t begin = 0U; begin < stroke.size();) {
  auto end = begin + 1U;
  while (end < stroke.size() && stroke[end].row == stroke[begin].row) ++end;
  const auto block_index = stroke[begin].row / kRowsPerBlock;
  if (index_->blocks.size() <= block_index) index_->blocks.resize(block_index + 1U);
  auto& block = index_->blocks[block_index];
  if (!block)
   block = std::make_shared<Block>();
  else if (!block.unique())
   block = std::make_shared<Block>(*block);
  auto& row = block->rows[stroke[begin].row % kRowsPerBlock];
  if (!row)
   row = std::make_shared<Runs>();
  else if (!row.unique())
   row = std::make_shared<Runs>(*row);
  const auto old_count = row->size();
  auto& result = scratch.result;
  result.clear();
  if (erase) {
   auto cut = begin;
   for (const auto run : *row) {
    unsigned first = run.first;
    while (cut < end && stroke[cut].last < first) ++cut;
    for (auto index = cut; index < end && stroke[index].first <= run.last; ++index) {
     if (stroke[index].first > first) result.push_back({run.row, static_cast<std::uint16_t>(first), static_cast<std::uint16_t>(stroke[index].first - 1U)});
     first = std::max(first, static_cast<unsigned>(stroke[index].last) + 1U);
     if (first > run.last) break;
    }
    if (first <= run.last) result.push_back({run.row, static_cast<std::uint16_t>(first), run.last});
   }
  } else {
   const auto append = [&](c::AnnotationMaskRun run) {
    if (!result.empty() && static_cast<unsigned>(result.back().last) + 1U >= run.first)
     result.back().last = std::max(result.back().last, run.last);
    else
     result.push_back(run);
   };
   auto existing = row->begin();
   auto added = begin;
   while (existing != row->end() || added < end) {
    if (added == end || (existing != row->end() && existing->first <= stroke[added].first))
     append(*existing++);
    else
     append(stroke[added++]);
   }
  }
  const auto count = index_->run_count - old_count + result.size();
  if (count > c::kAnnotationMaskRunCapacity) throw std::length_error("Mask run capacity exceeded");
  row->swap(result);
  index_->run_count = count;
  begin = end;
 }
}
void transform_mask(c::AnnotationObject& object, c::AnnotationBox from, c::AnnotationBox to, MaskScratch& scratch) {
 if (to.first.x >= to.second.x || to.first.y >= to.second.y) {
  object.mask.runs.clear();
  normalize_mask(object);
  return;
 }
 if (from.first.x >= from.second.x || from.first.y >= from.second.y) {
  normalize_mask(object);
  return;
 }
 const float sx = (to.second.x - to.first.x) / (from.second.x - from.first.x);
 const float sy = (to.second.y - to.first.y) / (from.second.y - from.first.y);
 const auto map_interval = [](float first, float last, float source, float target, float scale) {
  return std::pair{static_cast<int>(std::floor(target + (first - source) * scale)), static_cast<int>(std::ceil(target + (last + 1 - source) * scale)) - 1};
 };
 auto& transformed = scratch.result;
 transformed.clear();
 for (auto run : object.mask.runs) {
  const auto [first, last] = map_interval(run.first, run.last, from.first.x, to.first.x, sx);
  const auto [top, bottom] = map_interval(run.row, run.row, from.first.y, to.first.y, sy);
  for (int row = top; row <= bottom; ++row) transformed.push_back({static_cast<std::uint16_t>(row), static_cast<std::uint16_t>(first), static_cast<std::uint16_t>(last)});
  if (transformed.size() > c::kAnnotationMaskRunCapacity * 2) normalize(transformed);
 }
 object.mask.runs.swap(transformed);
 normalize_mask(object);
}
void fill_mask(c::AnnotationObject& object, c::AnnotationPoint point, std::uint16_t width, std::uint16_t height) {
 normalize(object.mask.runs);
 auto empty = complement(object.mask.runs, width, height);
 Components components(empty);
 std::size_t selected = empty.size();
 for (std::size_t index = 0; index < empty.size(); ++index)
  if (empty[index].row == static_cast<unsigned>(point.y) && point.x >= empty[index].first && point.x < static_cast<unsigned>(empty[index].last) + 1) selected = components.root(index);
 if (selected == empty.size()) return;
 for (std::size_t index = 0; index < empty.size(); ++index)
  if (components.root(index) == selected) object.mask.runs.push_back(empty[index]);
 normalize_mask(object);
}
void cleanup_mask(c::AnnotationObject& object, c::AnnotationMaskCleanup operation, std::uint16_t radius, std::uint16_t width, std::uint16_t height) {
 normalize(object.mask.runs);
 auto& runs = object.mask.runs;
 if (operation == c::AnnotationMaskCleanup::LargestComponent) {
  Components components(runs);
  std::vector<std::size_t> area(runs.size());
  for (std::size_t index = 0; index < runs.size(); ++index) area[components.root(index)] += runs[index].last - runs[index].first + 1;
  if (!runs.empty()) {
   const auto largest = static_cast<std::size_t>(std::ranges::max_element(area) - area.begin());
   Runs kept;
   for (std::size_t index = 0; index < runs.size(); ++index)
    if (components.root(index) == largest) kept.push_back(runs[index]);
   runs = std::move(kept);
  }
 } else if (operation == c::AnnotationMaskCleanup::FillHoles) {
  auto empty = complement(runs, width, height);
  Components components(empty);
  std::vector<bool> outside(empty.size());
  for (std::size_t index = 0; index < empty.size(); ++index) {
   auto run = empty[index];
   if (run.row == 0 || run.row + 1 == height || run.first == 0 || run.last + 1 == width) outside[components.root(index)] = true;
  }
  for (std::size_t index = 0; index < empty.size(); ++index)
   if (!outside[components.root(index)]) runs.push_back(empty[index]);
 } else {
  DiskReach disk(radius);
  const auto grow = [&] { runs = dilate(runs, radius, width, height, disk); };
  const auto shrink = [&] {
   runs = complement(dilate(complement(runs, width, height), radius, width, height, disk), width, height);
   Runs interior;
   for (auto run : runs) {
    if (run.row < radius || static_cast<unsigned>(run.row) + radius >= height || width <= 2U * radius) continue;
    run.first = std::max(run.first, radius);
    run.last = std::min(run.last, static_cast<std::uint16_t>(width - radius - 1));
    if (run.first <= run.last) interior.push_back(run);
   }
   runs = std::move(interior);
  };
  switch (operation) {
   case c::AnnotationMaskCleanup::Dilate: grow(); break;
   case c::AnnotationMaskCleanup::Erode: shrink(); break;
   case c::AnnotationMaskCleanup::Open:
    shrink();
    grow();
    break;
   case c::AnnotationMaskCleanup::Close:
    grow();
    shrink();
    break;
   default: break;
  }
 }
 object.mask.cleanup = operation;
 object.mask.cleanup_radius = radius;
 normalize_mask(object);
}
}  // namespace mmltk::controller::subsystems::annotation
