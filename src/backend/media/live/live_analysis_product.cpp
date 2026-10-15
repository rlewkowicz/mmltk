#include "live_analysis_product.h"
#include "src/frameworks/gpu/memory/pinned_host_buffer.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <meta>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
namespace mmltk::backend::media::live {
namespace runtime = mmltk::backend::ml::runtime;
namespace {
// Extents are bounded by the admitted frame, region and candidate capacities.
constexpr std::size_t kMaximumBytes = std::numeric_limits<std::size_t>::max() - 7U;
// Live's analyzer already admits at most this many candidates per region.
constexpr std::size_t kMaximumValues = runtime::kMaximumAnalysisRegions;
struct PinnedPages final {
 std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> pages;
 std::shared_ptr<bool> settled;
 ~PinnedPages() {
  if (pages && *settled) static_cast<void>(pages->ReleaseSettled());
  // Unproved reads use PinnedHostBuffer's checked retirement, preserving pages
  // on failed physical settlement. Normal completion never synchronizes a context.
 }
};
std::shared_ptr<void> allocate_pages(std::size_t bytes, LiveAnalysisProduct::Storage storage, const std::shared_ptr<bool>& settled) {
 if (storage == LiveAnalysisProduct::Storage::Host) return {::operator new(bytes), [](void* address) { ::operator delete(address); }};
 auto owner = std::make_shared<PinnedPages>();
 owner->settled = settled;
 owner->pages = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
 owner->pages->ensure_bytes(bytes);
 return {owner, owner->pages->data()};
}
cudaError_t copy_device(void* destination, const void* source, std::size_t bytes, std::uintptr_t stream) noexcept {
 return cudaMemcpyAsync(destination, source, bytes, cudaMemcpyDeviceToHost, reinterpret_cast<cudaStream_t>(stream));
}
std::size_t product(std::size_t left, std::size_t right) {
 if (left && right > kMaximumBytes / left) throw std::invalid_argument("Live analysis payload exceeds bounded storage");
 return left * right;
}
template<class T> struct IsSpan : std::false_type {};
template<class T, std::size_t N> struct IsSpan<std::span<T,N>> : std::true_type {};
// Structural projection applies only to the immutable host value view. Storage,
// copy submission, and publication remain explicit ordinary owner operations.
template<class Visitor> void visit_spans(LiveAnalysisRegion& region, Visitor&& visitor) {
 template for (constexpr auto member : std::define_static_array(std::meta::nonstatic_data_members_of(^^LiveAnalysisRegion, std::meta::access_context::current()))) {
  if constexpr (IsSpan<typename[:std::meta::type_of(member):]>::value) visitor(region.[:member:]);
 }
}
}
struct LiveAnalysisProduct::Impl final {
 struct Transfer {
  const void* source;
  std::size_t offset, bytes;
  void* view = nullptr;
  void (*materialize)(void*, void*, std::size_t) noexcept = nullptr;
 };
 struct Region { LiveAnalysisRegion view; std::size_t capacity, count_offset; };
 explicit Impl(Storage kind, Allocate allocator) : storage(kind), allocate(allocator) {
  plans.reserve(runtime::kMaximumAnalysisRegions * 8U);
  building.reserve(runtime::kMaximumAnalysisRegions);
  views.reserve(runtime::kMaximumAnalysisRegions);
 }
 Storage storage;
 Allocate allocate;
 std::shared_ptr<bool> settled = std::make_shared<bool>(true);
 std::shared_ptr<void> pages;
 std::size_t capacity = 0, used = 0;
 LiveFrameId frame{};
 bool ready = false, analyzed = false, prepared = false, submitted = false;
 std::vector<Transfer> plans;
 std::vector<Region> building;
 std::vector<LiveAnalysisRegion> views;
 std::size_t reserve(std::size_t bytes) {
  const auto offset = (used + 7U) & ~std::size_t{7U};
  if (offset > kMaximumBytes || bytes > kMaximumBytes - offset) throw std::invalid_argument("Live analysis payload exceeds bounded storage");
  used = offset + bytes;
  return offset;
 }
 std::shared_ptr<void> allocate_memory(std::size_t bytes) { return allocate ? allocate(bytes,storage) : allocate_pages(bytes,storage,settled); }
 void ensure() {
  if (used <= capacity) return;
  auto replacement = allocate_memory(used);
  if (!replacement) throw std::bad_alloc{};
  pages = std::move(replacement);
  capacity = used;
 }
 template<class T> void plane(const runtime::AnalysisDeviceBuffer& source, std::span<const T>& destination, std::span<const std::uint32_t> shape, runtime::AnalysisElementType type, std::size_t values) {
  std::size_t elements = 1;
  if (source.shape.rank != shape.size() || source.element_type != type) throw std::invalid_argument("Live analysis plane rank or type differs");
  for (std::size_t axis=0; axis<shape.size(); ++axis) {
   if (source.shape.extents[axis] != shape[axis]) throw std::invalid_argument("Live analysis plane shape differs");
   elements = product(elements, shape[axis]);
  }
  const auto element_bytes = type == runtime::AnalysisElementType::Uint8 ? 1U : 4U;
  const auto bytes = product(values,sizeof(T));
  if (!source.address || source.capacity_bytes < product(elements, element_bytes)) throw std::invalid_argument("Live analysis plane capacity differs");
  const auto offset = reserve(bytes);
  plans.push_back({reinterpret_cast<const void*>(source.address),offset,bytes,&destination,
   [](void* view, void* address, std::size_t size) noexcept {
    *static_cast<std::span<const T>*>(view) = {static_cast<const T*>(address),size/sizeof(T)};
   }});
 }
 void materialize() {
  for (const auto& plan : plans) if (plan.materialize)
   plan.materialize(plan.view, static_cast<std::byte*>(pages.get())+plan.offset, plan.bytes);
 }
};
LiveAnalysisProduct::LiveAnalysisProduct(Storage storage, Allocate allocation) : impl_(std::make_unique<Impl>(storage,allocation)) {}
LiveAnalysisProduct::~LiveAnalysisProduct() = default;
void LiveAnalysisProduct::Reset(LiveFrameId frame) noexcept {
 impl_->frame = frame;
 impl_->ready = frame.valid();
 impl_->analyzed = false;
 impl_->prepared = impl_->submitted = false;
 impl_->used = 0;
 impl_->plans.clear();
 impl_->building.clear();
 impl_->views.clear();
}
void LiveAnalysisProduct::PrepareDevice(LiveFrameId frame, std::uint32_t width, std::uint32_t height, std::span<const runtime::AnalysisAnnotationStorage> source) {
 if (!*impl_->settled) throw std::logic_error("Live analysis readback still has physical custody");
 Reset(frame);
 impl_->ready = false;
 if (!frame.valid() || !width || !height || source.size() > runtime::kMaximumAnalysisRegions) throw std::invalid_argument("Live analysis frame/region bounds differ");
 for (const auto& annotation : source) {
  const auto& region = annotation.source_region;
  if (!region.valid() || region.width > width || region.height > height || region.x > width-region.width || region.y > height-region.height ||
      annotation.value_capacity > kMaximumValues || !annotation.count.valid(annotation.value_capacity) || annotation.keypoint_capacity > mmltk::backend::data::catalog::kKeypointsPerClassCapacity)
   throw std::invalid_argument("Live analysis source region/count/capacity differs");
  const auto capacity = annotation.count.pending() ? annotation.value_capacity : annotation.count.value();
  impl_->building.push_back({{.source_region=region,.class_domain=annotation.class_domain,.class_catalog=annotation.class_catalog,.keypoint_capacity=annotation.keypoint_capacity,
   .masks_available=annotation.masks_available,.keypoints_available=annotation.keypoints_available},capacity,impl_->reserve(sizeof(std::int64_t))});
  auto& destination = impl_->building.back();
  if (annotation.count.pending()) {
   if (!annotation.count.device_view()) throw std::invalid_argument("Live pending count lacks device storage");
   impl_->plans.push_back({annotation.count.device_view(),destination.count_offset,sizeof(std::int64_t)});
  }
  if (annotation.class_domain != mmltk::backend::data::catalog::ClassReferenceDomain::Foreground &&
      annotation.class_domain != mmltk::backend::data::catalog::ClassReferenceDomain::RawOutputSlot)
   throw std::invalid_argument("Live analysis class reference domain differs");
  if (annotation.keypoints_available && (!annotation.keypoint_capacity || !annotation.class_catalog || !annotation.class_catalog->has_keypoints() ||
      annotation.class_domain != mmltk::backend::data::catalog::ClassReferenceDomain::Foreground))
   throw std::invalid_argument("Live joints require their named schema");
  if (annotation.value_capacity) {
   const auto full = static_cast<std::uint32_t>(annotation.value_capacity);
   const std::array values{full};
   impl_->plane(annotation.boxes_xyxy,destination.view.boxes_xyxy,std::array{full,4U},runtime::AnalysisElementType::Float32,capacity);
   impl_->plane(annotation.class_references,destination.view.class_references,values,runtime::AnalysisElementType::Int32,capacity);
   impl_->plane(annotation.confidences,destination.view.confidences,values,runtime::AnalysisElementType::Float32,capacity);
   impl_->plane(annotation.colors_rgb,destination.view.colors_rgb,std::array{full,3U},runtime::AnalysisElementType::Uint8,capacity);
   if (annotation.masks_available) impl_->plane(annotation.masks,destination.view.masks,std::array{full,region.height,region.width},runtime::AnalysisElementType::Uint8,product(capacity,product(region.height,region.width)));
   if (annotation.keypoints_available) {
    impl_->plane(annotation.keypoints,destination.view.keypoints,std::array{full,annotation.keypoint_capacity,static_cast<std::uint32_t>(runtime::kAnalysisKeypointChannels)},runtime::AnalysisElementType::Float32,product(capacity,annotation.keypoint_capacity));
    impl_->plane(annotation.keypoint_counts,destination.view.keypoint_counts,values,runtime::AnalysisElementType::Int32,capacity);
   }
  }
 }
 impl_->ensure();
 impl_->materialize();
 for (std::size_t index=0; index<source.size(); ++index) if (!source[index].count.pending()) {
  const auto count = static_cast<std::int64_t>(source[index].count.value());
  std::memcpy(static_cast<std::byte*>(impl_->pages.get())+impl_->building[index].count_offset,&count,sizeof(count));
 }
 impl_->analyzed = !source.empty();
 impl_->prepared = true;
}
cudaError_t LiveAnalysisProduct::EnqueueDeviceCopy(std::uintptr_t stream, DeviceCopy copy) noexcept {
 if (!stream || !impl_->prepared || impl_->submitted) return cudaErrorInvalidValue;
 impl_->prepared = false;
 if (!copy) copy = copy_device;
 for (const auto& transfer : impl_->plans) if (transfer.bytes) {
  *impl_->settled = false;
  const auto status = copy(static_cast<std::byte*>(impl_->pages.get())+transfer.offset,transfer.source,transfer.bytes,stream);
  if (status != cudaSuccess) return status;
 }
 impl_->submitted = true;
 return cudaSuccess;
}
void LiveAnalysisProduct::CompleteDeviceCopy() {
 if (!impl_->submitted) throw std::logic_error("Live analysis product has no submitted readback");
 impl_->submitted = false;
 SettleDeviceCopy();
 impl_->views.clear();
 for (auto& region : impl_->building) {
  std::int64_t count = 0;
  std::memcpy(&count,static_cast<const std::byte*>(impl_->pages.get())+region.count_offset,sizeof(count));
  if (count < 0 || static_cast<std::size_t>(count) > region.capacity) throw std::invalid_argument("Live completed analysis count exceeds capacity");
  auto& view = region.view;
  visit_spans(view,[&](auto& span) { span = span.first(region.capacity ? span.size()/region.capacity*static_cast<std::size_t>(count) : 0U); });
  view.count = static_cast<std::size_t>(count);
  for (std::size_t index=0; index<view.count; ++index) {
   const auto label = view.class_references[index];
   if (view.class_domain == mmltk::backend::data::catalog::ClassReferenceDomain::Foreground && (!view.class_catalog || label < 0 || static_cast<std::size_t>(label) >= view.class_catalog->size()))
    throw std::invalid_argument("Live completed class reference exceeds its catalog");
   if (view.keypoints_available && (view.keypoint_counts[index] < 0 || static_cast<std::uint32_t>(view.keypoint_counts[index]) > view.keypoint_capacity ||
       static_cast<std::size_t>(view.keypoint_counts[index]) != view.class_catalog->keypoints(label).names.size()))
    throw std::invalid_argument("Live completed joint count disagrees with its schema");
  }
  impl_->views.push_back(std::move(view));
 }
 impl_->building.clear();
 impl_->plans.clear();
 impl_->ready = true;
}
void LiveAnalysisProduct::SettleDeviceCopy() noexcept { *impl_->settled = true; }
void LiveAnalysisProduct::CopyFrom(const LiveAnalysisProduct& source) {
 if (!*impl_->settled) throw std::logic_error("Live analysis readback still has physical custody");
 if (this == &source) throw std::invalid_argument("Live semantic copy requires independent storage");
 if (!source.ready()) throw std::invalid_argument("Live semantic source has not completed");
 // Allocate/copy metadata before mutating the destination's published value.
 impl_->building.clear();
 for (const auto& view : source.impl_->views) impl_->building.push_back({view,0,0});
 const auto used = source.impl_->used;
 std::shared_ptr<void> replacement = impl_->pages;
 if (used > impl_->capacity) {
  replacement = impl_->allocate_memory(used);
  if (!replacement) throw std::bad_alloc{};
 }
 if (used) std::memcpy(replacement.get(),source.impl_->pages.get(),used);
 const auto old_base = reinterpret_cast<std::uintptr_t>(source.impl_->pages.get()), new_base = reinterpret_cast<std::uintptr_t>(replacement.get());
 for (auto& region : impl_->building) visit_spans(region.view,[&](auto& span) {
  using Element = typename std::remove_reference_t<decltype(span)>::element_type;
  if (span.data()) span = {reinterpret_cast<Element*>(new_base + (reinterpret_cast<std::uintptr_t>(span.data())-old_base)),span.size()};
 });
 impl_->views.clear();
 for (auto& region : impl_->building) impl_->views.push_back(std::move(region.view));
 impl_->building.clear();
 impl_->plans.clear();
 impl_->prepared = impl_->submitted = false;
 impl_->frame = source.frame();
 impl_->ready = true;
 impl_->pages = std::move(replacement);
 impl_->capacity = std::max(impl_->capacity,used);
 impl_->used = used;
 impl_->analyzed = source.analyzed();
}
LiveFrameId LiveAnalysisProduct::frame() const noexcept { return impl_->frame; }
bool LiveAnalysisProduct::ready() const noexcept { return impl_->ready; }
bool LiveAnalysisProduct::analyzed() const noexcept { return impl_->ready && impl_->analyzed; }
std::span<const LiveAnalysisRegion> LiveAnalysisProduct::regions() const noexcept { return impl_->ready ? std::span<const LiveAnalysisRegion>(impl_->views) : std::span<const LiveAnalysisRegion>{}; }
std::size_t LiveAnalysisProduct::capacity_bytes() const noexcept { return impl_->capacity; }
}  // namespace mmltk::backend::media::live
