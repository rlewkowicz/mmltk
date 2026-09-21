#include "src/backend/models/rfdetr/augmentation/sampling.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include <algorithm>
#include <cuda.h>
#include <functional>
#include "src/backend/imaging/resample/image_resize_cuda.h"
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <meta>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include "mmltk/frameworks/reflection/materializer.h"
#include "detail/gpu_augment_cuda_launch.h"
#include "detail/gpu_augment_plan_math.h"
#include "detail/gpu_augmentation_donor_index.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::backend::models::rfdetr {
using mmltk::frameworks::gpu::ensure_cuda_ok;
namespace {
void require(const bool condition, const std::string_view message) {
 if (!condition) { throw std::runtime_error(std::string(message)); }
}
// This projection is limited to the executor's numeric configuration and plans.
// Borrowed support addresses are omitted; their ordinary count/shape facts remain.
template <class T>
void append_prepared_diagnostic(std::ostream& output, const T& value) {
 if constexpr (std::is_same_v<T, bool>) {
  output << (value ? "true" : "false");
 } else if constexpr (std::is_floating_point_v<T>) {
  if (std::isfinite(value))
   output << value;
  else
   output << '"' << value << '"';
 } else if constexpr (std::is_integral_v<T>) {
  output << +value;
 } else if constexpr (std::ranges::range<T>) {
  output << '[';
  bool first = true;
  for (const auto& element : value) {
   if (!std::exchange(first, false)) output << ',';
   append_prepared_diagnostic(output, element);
  }
  output << ']';
 } else {
  static constexpr auto members = mmltk::frameworks::reflection::materialize<T>([]<class Owner, class Reflection>() consteval {
   static_assert(Reflection::bases.empty());
   return Reflection::members;
  });
  output << '{';
  bool first = true;
  template for (constexpr auto member : members) {
   if constexpr (!std::is_pointer_v<typename[:std::meta::type_of(member):]>) {
    if (!std::exchange(first, false)) output << ',';
    output << '"' << std::meta::identifier_of(member) << "\":";
    append_prepared_diagnostic(output, value.[:member:]);
   }
  }
  output << '}';
 }
}
GpuAugmentationGroupLaunchConfig launch_group(const AugmentationGroupConfig& group) { return {group.probability, group.min_strength, group.max_strength}; }
GpuAugmentationLaunchConfig launch_config(const GpuAugmentationConfig& config) {
 return {config.enabled ? 1 : 0,     launch_group(config.geometry), launch_group(config.resize),   launch_group(config.color),
         launch_group(config.noise), launch_group(config.blur),     launch_group(config.occlusion)};
}
void prepare_image_plan(AugmentationImagePlan& plan, const GpuAugmentationLaunchConfig& config, const std::uint64_t key, float* parameters) {
 const augment_math::GeometryPlan geometry = augment_math::solve_image_plan(parameters, config, key);
 plan.erasure = augment_math::spatial_erasure(parameters, key);
 plan.resize_scale = geometry.resize_scale;
 plan.resize_offset_x = geometry.resize_offset_x;
 plan.resize_offset_y = geometry.resize_offset_y;
 std::copy_n(geometry.forward, plan.forward.size(), plan.forward.begin());
 std::copy_n(geometry.inverse, plan.inverse.size(), plan.inverse.begin());
 plan.area_scale = geometry.area_scale;
}
template <typename T>
class DeviceAllocation final {
public:
 DeviceAllocation() = default;
 explicit DeviceAllocation(const std::size_t count) { ensure(count); }
 ~DeviceAllocation() {
  if (data_ != nullptr) {
   int previous_device = -1;
   const bool restore = cudaGetDevice(&previous_device) == cudaSuccess && previous_device != device_id_ && cudaSetDevice(device_id_) == cudaSuccess;
   (void)cudaFree(data_);
   if (restore) { (void)cudaSetDevice(previous_device); }
  }
 }
 DeviceAllocation(const DeviceAllocation&) = delete;
 DeviceAllocation& operator=(const DeviceAllocation&) = delete;
 DeviceAllocation(DeviceAllocation&&) = delete;
 DeviceAllocation& operator=(DeviceAllocation&&) = delete;
 void ensure(const std::size_t count) {
  if (count <= count_) { return; }
  require(count <= std::numeric_limits<std::size_t>::max() / sizeof(T), "augmentation device allocation size overflows");
  int active_device = -1;
  ensure_cuda_ok(cudaGetDevice(&active_device), "cudaGetDevice for augmentation allocation");
  T* replacement = nullptr;
  ensure_cuda_ok(cudaMalloc(reinterpret_cast<void**>(&replacement), count * sizeof(T)), "cudaMalloc for augmentation workspace");
  if (data_ != nullptr) {
   const cudaError_t release_status = cudaFree(data_);
   if (release_status != cudaSuccess) {
    (void)cudaFree(replacement);
    ensure_cuda_ok(release_status, "cudaFree while growing augmentation workspace");
   }
  }
  data_ = replacement;
  count_ = count;
  device_id_ = active_device;
 }
 [[nodiscard]] T* data() noexcept { return data_; }
 [[nodiscard]] const T* data() const noexcept { return data_; }
 [[nodiscard]] std::size_t capacity() const noexcept { return count_; }
 [[nodiscard]] std::size_t bytes() const noexcept { return count_ * sizeof(T); }

private:
 T* data_ = nullptr;
 std::size_t count_ = 0U;
 int device_id_ = -1;
};
template <typename T>
class PinnedAllocation final {
public:
 PinnedAllocation() = default;
 explicit PinnedAllocation(const std::size_t count) : count_(count) {
  if (count_ != 0U) {
   require(count_ <= std::numeric_limits<std::size_t>::max() / sizeof(T), "augmentation pinned allocation size overflows");
   storage_ = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
   storage_->ensure_bytes(count_ * sizeof(T));
   data_ = static_cast<T*>(storage_->data());
  }
 }
 ~PinnedAllocation() = default;
 PinnedAllocation(const PinnedAllocation&) = delete;
 PinnedAllocation& operator=(const PinnedAllocation&) = delete;
 [[nodiscard]] T* data() noexcept { return data_; }
 [[nodiscard]] std::size_t bytes() const noexcept { return count_ * sizeof(T); }

private:
 std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> storage_;
 T* data_ = nullptr;
 std::size_t count_ = 0U;
};
constexpr std::size_t kStagingSlots = 2U;
}  // namespace
bool augmentation_paste_admitted(const GpuAugmentationConfig& config, const std::uint64_t key) noexcept {
 return config.enabled && augment_math::uniform01(key, 0x4000ULL) < config.copy_paste_probability;
}
void detail::CachedAugmentationDonorIndex::rebuild(const std::span<const GpuAugmentationDonor> donors) {
 donors_ = donors;
 valid_.clear();
 valid_.reserve(donors.size());
 first_valid_.assign(donors.size(), -1);
 next_different_.assign(donors.size(), -1);
 for (std::size_t slot = 0; slot < donors.size(); ++slot)
  if (donors[slot].label >= 0) valid_.push_back(slot);
 if (valid_.empty()) return;
 auto next = static_cast<std::int64_t>(valid_.front());
 for (std::size_t slot = donors.size(); slot-- != 0;) {
  if (donors[slot].label >= 0) next = static_cast<std::int64_t>(slot);
  first_valid_[slot] = next;
 }
 // Begin at a dataset boundary so no same-dataset run straddles the
 // traversal. Invalid cache entries were removed without changing order.
 std::size_t pivot = 0;
 while (pivot < valid_.size() && donors[valid_[pivot]].dataset_index == donors[valid_[(pivot + valid_.size() - 1) % valid_.size()]].dataset_index) ++pivot;
 if (pivot == valid_.size()) return;
 for (std::size_t begin = 0; begin < valid_.size();) {
  std::size_t end = begin + 1;
  const auto source = donors[valid_[(pivot + begin) % valid_.size()]].dataset_index;
  while (end < valid_.size() && donors[valid_[(pivot + end) % valid_.size()]].dataset_index == source) ++end;
  const auto following = static_cast<std::int64_t>(valid_[(pivot + end) % valid_.size()]);
  for (auto item = begin; item < end; ++item) next_different_[valid_[(pivot + item) % valid_.size()]] = following;
  begin = end;
 }
}
std::int64_t detail::CachedAugmentationDonorIndex::select(const std::size_t start, const std::uint32_t source) const noexcept {
 if (start >= first_valid_.size()) return -1;
 const auto first = first_valid_[start];
 if (first < 0) return -1;
 const auto slot = static_cast<std::size_t>(first);
 return donors_[slot].dataset_index == source ? next_different_[slot] : first;
}
struct GpuAugmentationExecutor::Impl final {
 struct PreparedStorage final {
  explicit PreparedStorage(mmltk::frameworks::gpu::DeviceContext owner, std::size_t capacity)
      : context(std::move(owner)), views(capacity * 2U), staged(kStagingSlots * capacity * 2U) {}
  mmltk::frameworks::gpu::DeviceContext context;
  DeviceAllocation<float> pixels;
  DeviceAllocation<GpuAugmentationPreparedView> views;
  PinnedAllocation<GpuAugmentationPreparedView> staged;
 };
 struct Reduction final {
  const float* source = nullptr;
  int width = 0, height = 0;
  std::size_t offset = 0;
  std::shared_ptr<const void> custody;
 };
 struct ReductionKey final {
  const float* source;
  int width, height;
  bool operator==(const ReductionKey&) const = default;
 };
 struct ReductionHash final {
  std::size_t operator()(ReductionKey value) const noexcept {
   return static_cast<std::size_t>(mmltk::common::math::deterministic_mix64(reinterpret_cast<std::uintptr_t>(value.source) ^
                                                                            (std::uint64_t(value.width) << 32U) ^ std::uint64_t(value.height)));
  }
 };
 Impl(const GpuAugmentationConfig& input_config, const std::size_t input_capacity, const int input_height, const int input_width,
      mmltk::frameworks::gpu::DeviceContext input_context, mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& authority)
     : context(std::move(input_context)),
       retirement(authority),
       config(input_config),
       capacity(input_capacity),
       height(input_height),
       width(input_width),
       device_id(context.device()),
       converted_input(),
       parameters(capacity * static_cast<std::size_t>(kGpuAugmentationParameterCount)),
       keys(capacity),
       input_slots(capacity),
       donor_slots(capacity),
       staged_slots(kStagingSlots * capacity * 2U),
       paste_parameters(capacity * static_cast<std::size_t>(kGpuCopyPasteParameterCount)),
       staged_keys(kStagingSlots * capacity),
       staged_parameters(kStagingSlots * capacity * static_cast<std::size_t>(kGpuAugmentationParameterCount)),
       staged_paste(kStagingSlots * capacity * static_cast<std::size_t>(kGpuCopyPasteParameterCount)) {
  ensure_cuda_ok(static_cast<cudaError_t>(cuCtxGetCurrent(&native_context)), "augmentation current context");
  plan.images.resize(capacity);
  training_keys.resize(capacity);
  std::size_t created = 0U;
  try {
   for (cudaEvent_t& event : staging_complete) {
    ensure_cuda_ok(cudaEventCreateWithFlags(&event, cudaEventDisableTiming), "cudaEventCreateWithFlags for augmentation staging");
    ++created;
   }
   ensure_cuda_ok(cudaEventCreateWithFlags(&execution_complete, cudaEventDisableTiming), "cudaEventCreateWithFlags for augmentation execution");
  } catch (...) {
   for (std::size_t slot = 0U; slot < created; ++slot) {
    (void)cudaEventDestroy(staging_complete[slot]);
    staging_complete[slot] = nullptr;
   }
   throw;
  }
  update_flags();
 }
 ~Impl() {
  int previous_device = -1;
  const bool restore = cudaGetDevice(&previous_device) == cudaSuccess && previous_device != device_id && cudaSetDevice(device_id) == cudaSuccess;
  if (execution_complete != nullptr) {
   if (execution_pending) { (void)cudaEventSynchronize(execution_complete); }
   (void)cudaEventDestroy(execution_complete);
  }
  for (std::size_t slot = 0U; slot < kStagingSlots; ++slot) {
   if (staging_complete[slot] == nullptr) { continue; }
   if (staging_pending[slot]) { (void)cudaEventSynchronize(staging_complete[slot]); }
   (void)cudaEventDestroy(staging_complete[slot]);
  }
  if (restore) { (void)cudaSetDevice(previous_device); }
 }
 void update_flags() noexcept {
  transforms_geometry = config.enabled && (config.geometry.probability > 0.0F || config.resize.probability > 0.0F);
  copy_paste = config.enabled && config.copy_paste_probability > 0.0F;
  remap = config.enabled && ((config.geometry.probability > 0.0F && config.geometry.max_strength > 0.0F) ||
                             (config.resize.probability > 0.0F && config.resize.max_strength > 0.0F) ||
                             (config.blur.probability > 0.0F && config.blur.max_strength > 0.0F) || copy_paste);
 }
 void prepare_plan(const GpuAugmentationBatchView& batch, const std::span<const std::uint64_t> image_keys, const std::span<const GpuAugmentationDonor> donors,
                   const GpuAugmentationDonorSelection donor_selection, float* paste, float* image_parameters) {
  plan.active_size = batch.image_indices.size();
  plan.transforms_geometry = transforms_geometry;
  plan.erases_spatial_support = false;
  plan.copy_paste_enabled = copy_paste;
  const GpuAugmentationLaunchConfig launch = launch_config(config);
  if (copy_paste && donor_selection == GpuAugmentationDonorSelection::Cached) donor_index.rebuild(donors);
  for (std::size_t image = 0U; image < plan.active_size; ++image) {
   AugmentationImagePlan& image_plan = plan.images[image];
   image_plan = AugmentationImagePlan{};
   const std::uint64_t key = image_keys[image];
   prepare_image_plan(image_plan, launch, key, image_parameters + image * static_cast<std::size_t>(kGpuAugmentationParameterCount));
   plan.erases_spatial_support |= image_plan.erasure.dropout_probability > 0.0F || image_plan.erasure.rectangular != 0U;
   image_plan.cache_choice = augment_math::uniform01(key, 0x5000ULL);
   image_plan.cache_source_dataset_index = batch.image_indices[image];
   if (!copy_paste) { continue; }
   float* current_paste = paste + image * static_cast<std::size_t>(kGpuCopyPasteParameterCount);
   std::fill_n(current_paste, kGpuCopyPasteParameterCount, 0.0F);
   current_paste[0] = -1.0F;
   if (!augmentation_paste_admitted(config, key)) { continue; }
   std::int64_t donor_slot = -1;
   if (donor_selection == GpuAugmentationDonorSelection::Aligned) {
    const GpuAugmentationDonor& donor = donors[image];
    donor_slot = donor.label >= 0 && donor.dataset_index != batch.image_indices[image] ? static_cast<std::int64_t>(image) : -1;
   } else {
    const std::size_t start =
     std::min<std::size_t>(static_cast<std::size_t>(augment_math::uniform01(key, 0x4001ULL) * static_cast<float>(capacity)), capacity - 1U);
    donor_slot = donor_index.select(start, batch.image_indices[image]);
   }
   if (donor_slot < 0) { continue; }
   const GpuAugmentationDonor& donor = donors[static_cast<std::size_t>(donor_slot)];
   const float scale = 0.5F + augment_math::uniform01(key, 0x4002ULL);
   const float destination_x = augment_math::uniform01(key, 0x4003ULL);
   const float destination_y = augment_math::uniform01(key, 0x4004ULL);
   const float source_x = (donor.box[0] + donor.box[2]) * 0.5F;
   const float source_y = (donor.box[1] + donor.box[3]) * 0.5F;
   const float translate_x = destination_x - scale * source_x;
   const float translate_y = destination_y - scale * source_y;
   const float inverse_scale = 1.0F / scale;
   const float output_x0 = augment_math::clamp01(std::fma(scale, donor.box[0], translate_x));
   const float output_y0 = augment_math::clamp01(std::fma(scale, donor.box[1], translate_y));
   const float output_x1 = augment_math::clamp01(std::fma(scale, donor.box[2], translate_x));
   const float output_y1 = augment_math::clamp01(std::fma(scale, donor.box[3], translate_y));
   if (output_x1 <= output_x0 || output_y1 <= output_y0) { continue; }
   image_plan.paste_donor_slot = donor_slot;
   image_plan.paste_masked = donor.has_mask;
   image_plan.paste_label = donor.label;
   image_plan.paste_source_area = donor.area;
   image_plan.paste_source_box = donor.box;
   image_plan.paste_output_box = {output_x0, output_y0, output_x1, output_y1};
   image_plan.paste_inverse = {inverse_scale, 0.0F, -translate_x * inverse_scale, 0.0F, inverse_scale, -translate_y * inverse_scale};
   current_paste[0] = static_cast<float>(donor_slot);
   current_paste[1] = donor.has_mask ? 1.0F : 2.0F;
   std::copy(image_plan.paste_inverse.begin(), image_plan.paste_inverse.end(), current_paste + 2);
  }
 }
 // Retained context outlives every physical allocation. Each staging slot
 // retains the exact source/donor/output aggregate through the final remap.
 mmltk::frameworks::gpu::DeviceContext context;
 mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& retirement;
 CUcontext native_context = nullptr;
 std::shared_ptr<PreparedStorage> prepared;
 std::unique_ptr<mmltk::backend::imaging::resample::GpuPerceptualDownscaler> downscaler;
 std::vector<Reduction> reductions;
 std::vector<ReductionKey> reduction_keys;
 std::vector<std::size_t> reduction_indices;
 std::vector<std::size_t> occupied_reductions;
 std::vector<std::size_t> view_reductions;
 std::array<std::array<std::shared_ptr<const void>, 3>, kStagingSlots> custody;
 bool prepare_reductions(const GpuAugmentationBatchView& batch, const GpuAugmentationDonorBatchView& donors, const float* input,
                         std::shared_ptr<const void> input_owner, cudaStream_t stream, std::size_t slot, GpuAugmentationExecutor& owner) {
  if (!config.perceptual_downscale || !remap) return false;
  const auto image_width = static_cast<float>(width);
  const auto image_height = static_cast<float>(height);
  const auto minifies = [&](float scale) {
   return scale < 1.0F && (std::ceil(image_width * scale) < image_width || std::ceil(image_height * scale) < image_height);
  };
  bool selected = false;
  for (std::size_t image = 0; image != plan.active_size; ++image) {
   const auto& planned = plan.images[image];
   selected |= minifies(std::sqrt(planned.area_scale));
   if (planned.paste_donor_slot >= 0) {
    const auto& inverse = planned.paste_inverse;
    selected |= minifies(1.0F / std::sqrt(std::abs(inverse[0] * inverse[4] - inverse[1] * inverse[3])));
   }
  }
  if (!selected) return false;
  const auto count = batch.image_indices.size();
  const auto image_values = std::size_t(width) * std::size_t(height) * 3U;
  require(image_values <= std::numeric_limits<std::size_t>::max() / (2U * sizeof(float)) &&
           count <= std::numeric_limits<std::size_t>::max() / (2U * image_values * sizeof(float)),
          "perceptual augmentation batch storage overflows");
  const auto image_bytes = image_values * sizeof(float);
  require(batch.input_custody && batch.output_custody, "perceptual augmentation requires exact batch custody");
  const auto input_bytes = batch.input_format == GpuAugmentationInputFormat::Rgba8 ? std::size_t(width) * std::size_t(height) * 4U : image_bytes;
  require(batch.input_capacity_bytes >= input_bytes * (batch.input_slots.empty() ? count : 1U), "augmentation input logical extent is too small");
  require(batch.output_capacity_bytes >= image_bytes * count, "augmentation output logical extent is too small");
  if (!prepared) {
   require(capacity <= std::numeric_limits<std::size_t>::max() / 4U, "augmentation reduction index overflows");
   auto candidate_storage = std::make_shared<PreparedStorage>(context, capacity);
   auto candidate_downscaler = std::make_unique<mmltk::backend::imaging::resample::GpuPerceptualDownscaler>(context, retirement);
   reductions.reserve(capacity * 2U);
   reduction_keys.resize(capacity * 4U);
   reduction_indices.resize(capacity * 4U);
   occupied_reductions.reserve(capacity * 2U);
   view_reductions.resize(capacity * 2U);
   // No candidate storage has been submitted. A failed allocation may
   // retain harmless CPU capacity, but readiness stays unpublished.
   // Both ownership transfers are nonthrowing; publish readiness last.
   downscaler = std::move(candidate_downscaler);
   prepared = std::move(candidate_storage);
  }
  reductions.clear();
  for (const auto occupied_slot : occupied_reductions) reduction_keys[occupied_slot].source = nullptr;
  occupied_reductions.clear();
  auto* views = prepared->staged.data() + slot * capacity * 2U;
  std::size_t total = 0U;
  const auto add = [&](std::size_t index, const float* pixels, float scale, const std::shared_ptr<const void>& source_owner) {
   auto& view = views[index];
   view = {pixels, width, height, width, std::int64_t(width) * height};
   view_reductions[index] = std::numeric_limits<std::size_t>::max();
   if (!(scale < 1.0F)) return;
   const int reduced_width = std::clamp(static_cast<int>(std::ceil(image_width * scale)), 1, width);
   const int reduced_height = std::clamp(static_cast<int>(std::ceil(image_height * scale)), 1, height);
   if (reduced_width == width && reduced_height == height) return;
   const ReductionKey key{pixels, reduced_width, reduced_height};
   auto bucket = ReductionHash{}(key) % reduction_keys.size();
   while (reduction_keys[bucket].source && !(reduction_keys[bucket] == key)) bucket = (bucket + 1U) % reduction_keys.size();
   if (!reduction_keys[bucket].source) {
    reduction_keys[bucket] = key;
    reduction_indices[bucket] = reductions.size();
    occupied_reductions.push_back(bucket);
    reductions.push_back({pixels, reduced_width, reduced_height, total, source_owner});
    total += std::size_t(reduced_width) * std::size_t(reduced_height) * 3U;
   }
   view_reductions[index] = reduction_indices[bucket];
  };
  for (std::size_t image = 0; image != count; ++image) {
   const auto& image_plan = plan.images[image];
   add(image * 2U, batch.input_slots.empty() ? input + image * image_values : batch.input_slots[image], std::sqrt(image_plan.area_scale), input_owner);
   views[image * 2U + 1U] = {};
   view_reductions[image * 2U + 1U] = std::numeric_limits<std::size_t>::max();
   if (image_plan.paste_donor_slot < 0) continue;
   require(donors.image_custody != nullptr, "perceptual augmentation requires exact donor custody");
   const auto donor = std::size_t(image_plan.paste_donor_slot);
   require(donors.image_capacity_bytes >= image_bytes * (donors.image_slots.empty() ? donor + 1U : 1U), "augmentation donor logical extent is too small");
   const auto& inverse = image_plan.paste_inverse;
   const float determinant = std::abs(inverse[0] * inverse[4] - inverse[1] * inverse[3]);
   add(image * 2U + 1U, donors.image_slots.empty() ? donors.images + donor * image_values : donors.image_slots[donor], 1.0F / std::sqrt(determinant),
       donors.image_custody);
  }
  // Growth is rare and bounded by two active-batch image families. Settle
  // once before replacing their common allocation, never once per image.
  if (total > prepared->pixels.capacity()) {
   if (execution_pending) owner.CheckSettlement(owner.event_wait_(execution_complete), "augmentation prepared storage growth");
   downscaler->finish();
   prepared->pixels.ensure(total);
  }
  const mmltk::backend::imaging::resample::RgbImageLayout source_layout{std::uint32_t(width),
                                                                        std::uint32_t(height),
                                                                        std::size_t(width) * sizeof(float),
                                                                        std::size_t(width) * std::size_t(height) * sizeof(float),
                                                                        image_bytes,
                                                                        mmltk::backend::imaging::resample::RgbPixelFormat::PlanarUnitSrgbF32};
  for (const auto& reduction : reductions) {
   const auto values = std::size_t(reduction.width) * std::size_t(reduction.height);
   const mmltk::backend::imaging::resample::RgbImageLayout destination_layout{
    std::uint32_t(reduction.width), std::uint32_t(reduction.height), std::size_t(reduction.width) * sizeof(float),
    values * sizeof(float),         values * 3U * sizeof(float),     mmltk::backend::imaging::resample::RgbPixelFormat::PlanarUnitSrgbF32};
   downscaler->downscale({reduction.source, source_layout}, {prepared->pixels.data() + reduction.offset, destination_layout}, stream, reduction.custody,
                         prepared);
  }
  for (std::size_t image = 0; image != count * 2U; ++image) {
   if (view_reductions[image] == std::numeric_limits<std::size_t>::max()) continue;
   const auto& reduction = reductions[view_reductions[image]];
   views[image] = {prepared->pixels.data() + reduction.offset, reduction.width, reduction.height, reduction.width,
                   std::int64_t(reduction.width) * reduction.height};
  }
  ensure_cuda_ok(cudaMemcpyAsync(prepared->views.data(), views, count * 2U * sizeof(*views), cudaMemcpyHostToDevice, stream),
                 "augmentation prepared view upload");
  return true;
 }
 GpuAugmentationConfig config;
 std::size_t capacity;
 int height;
 int width;
 int device_id;
 bool remap = false;
 bool transforms_geometry = false;
 bool copy_paste = false;
 AugmentationBatchPlan plan;
 std::vector<std::uint64_t> training_keys;
 detail::CachedAugmentationDonorIndex donor_index;
 DeviceAllocation<float> converted_input;
 DeviceAllocation<float> parameters;
 DeviceAllocation<std::uint64_t> keys;
 DeviceAllocation<const float*> input_slots, donor_slots;
 PinnedAllocation<const float*> staged_slots;
 DeviceAllocation<float> paste_parameters;
 PinnedAllocation<std::uint64_t> staged_keys;
 PinnedAllocation<float> staged_parameters;
 PinnedAllocation<float> staged_paste;
 std::array<cudaEvent_t, kStagingSlots> staging_complete{};
 std::array<bool, kStagingSlots> staging_pending{};
 cudaEvent_t execution_complete = nullptr;
 bool execution_pending = false;
};
GpuAugmentationExecutor::GpuAugmentationExecutor(const GpuAugmentationConfig& config, const std::size_t batch_capacity, const int height, const int width,
                                                 mmltk::frameworks::gpu::DeviceContext context,
                                                 mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& retirement)
    : impl_(nullptr) {
 require(gpu_augmentation_config_valid(config), "invalid GPU augmentation configuration");
 require(batch_capacity > 0U && height > 0 && width > 0, "invalid GPU augmentation batch shape");
 const auto unsigned_height = static_cast<std::size_t>(height);
 const auto unsigned_width = static_cast<std::size_t>(width);
 require(unsigned_height <= std::numeric_limits<std::size_t>::max() / unsigned_width, "GPU augmentation image size overflows");
 const std::size_t pixels = unsigned_height * unsigned_width;
 require(pixels <= std::numeric_limits<std::size_t>::max() / 3U && batch_capacity <= std::numeric_limits<std::size_t>::max() / (pixels * 3U) &&
          batch_capacity <= std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(kGpuAugmentationParameterCount) &&
          batch_capacity <= std::numeric_limits<std::size_t>::max() / (kStagingSlots * static_cast<std::size_t>(kGpuAugmentationParameterCount)) &&
          batch_capacity <= std::numeric_limits<std::size_t>::max() / (kStagingSlots * static_cast<std::size_t>(kGpuCopyPasteParameterCount)),
         "GPU augmentation workspace size overflows");
 int active_device = -1;
 ensure_cuda_ok(cudaGetDevice(&active_device), "cudaGetDevice for augmentation executor");
 require(active_device == context.device(), "augmentation executor requires its owning CUDA device to be current");
 CUcontext original = nullptr, retained = nullptr;
 ensure_cuda_ok(static_cast<cudaError_t>(cuCtxGetCurrent(&original)), "augmentation caller context");
 context.Bind();
 const auto captured = cuCtxGetCurrent(&retained);
 const auto restored = cuCtxSetCurrent(original);
 ensure_cuda_ok(static_cast<cudaError_t>(restored), "augmentation caller context restoration");
 ensure_cuda_ok(static_cast<cudaError_t>(captured), "augmentation retained context");
 require(original == retained, "augmentation executor requires its exact retained context to be current");
 retirement_ = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement);
 impl_ = std::make_shared<Impl>(config, batch_capacity, height, width, std::move(context), retirement);
}
void GpuAugmentationExecutor::RequireActive() const { require(impl_ && impl_->retirement.admission_open(), "augmentation executor has terminal custody"); }
void GpuAugmentationExecutor::Retire(const cudaError_t status) noexcept {
 if (impl_) std::move(retirement_).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(impl_)), status);
}
void GpuAugmentationExecutor::CheckSettlement(const cudaError_t status, const char* detail) {
 if (status == cudaSuccess) return;
 Retire(status);
 ensure_cuda_ok(status, detail);
}
GpuAugmentationExecutor::~GpuAugmentationExecutor() {
 if (!impl_) return;
 const auto retained_context = impl_->native_context;
 if (cuCtxPushCurrent(retained_context) != CUDA_SUCCESS) {
  Retire(cudaErrorContextIsDestroyed);
  return;
 }
 try {
  Finish();
  impl_.reset();
 } catch (...) {
  if (impl_) Retire(impl_->retirement.fact().first_failure != cudaSuccess ? impl_->retirement.fact().first_failure : cudaErrorUnknown);
 }
 CUcontext popped = nullptr;
 (void)cuCtxPopCurrent(&popped);
}
void GpuAugmentationExecutor::Finish() {
 RequireActive();
 if (impl_->execution_pending) CheckSettlement(event_wait_(impl_->execution_complete), "augmentation final settlement");
 try {
  if (impl_->downscaler) impl_->downscaler->finish();
 } catch (...) {
  Retire(impl_->retirement.fact().first_failure != cudaSuccess ? impl_->retirement.fact().first_failure : cudaErrorUnknown);
  throw;
 }
 impl_->execution_pending = false;
 impl_->staging_pending.fill(false);  // Final execution follows every staging read on the ordered streams.
 for (auto& slot : impl_->custody) slot = {};
 impl_->reductions.clear();
}
void GpuAugmentationExecutor::Reconfigure(const GpuAugmentationConfig& config) {
 RequireActive();
 if (impl_->config == config) return;
 require(gpu_augmentation_config_valid(config), "invalid GPU augmentation configuration");
 Finish();
 impl_->config = config;
 impl_->update_flags();
}
const AugmentationBatchPlan& GpuAugmentationExecutor::Run(const GpuAugmentationBatchView& batch, const std::span<const std::uint64_t> image_keys,
                                                          const std::span<const GpuAugmentationDonor> donors, const GpuAugmentationDonorBatchView& donor_batch,
                                                          cudaStream_t stream, const std::size_t staging_slot) {
 return RunImpl(batch, image_keys, donors, donor_batch, stream, staging_slot, true, 0U, 0, 0, 0U);
}
const AugmentationBatchPlan& GpuAugmentationExecutor::RunTraining(const GpuAugmentationBatchView& batch, const std::uint64_t seed, const int epoch,
                                                                  const int rank, const std::uint64_t sequence,
                                                                  const std::span<const GpuAugmentationDonor> donors,
                                                                  const GpuAugmentationDonorBatchView& donor_batch, cudaStream_t stream,
                                                                  const std::size_t staging_slot) {
 RequireActive();
 require(batch.image_indices.size() <= impl_->capacity, "GPU augmentation batch exceeds preallocated capacity");
 for (std::size_t image = 0U; image < batch.image_indices.size(); ++image) {
  impl_->training_keys[image] = training_augmentation_image_key(seed, epoch, rank, sequence, image);
 }
 return RunImpl(batch, std::span{impl_->training_keys}.first(batch.image_indices.size()), donors, donor_batch, stream, staging_slot, false, seed, epoch, rank,
                sequence);
}
const AugmentationBatchPlan& GpuAugmentationExecutor::RunImpl(const GpuAugmentationBatchView& batch, const std::span<const std::uint64_t> image_keys,
                                                              const std::span<const GpuAugmentationDonor> donors,
                                                              const GpuAugmentationDonorBatchView& donor_batch, cudaStream_t stream,
                                                              const std::size_t staging_slot, const bool explicit_keys, const std::uint64_t seed,
                                                              const int epoch, const int rank, const std::uint64_t sequence) {
 RequireActive();
 require(batch.height == impl_->height && batch.width == impl_->width, "GPU augmentation batch dimensions do not match the executor");
 require(batch.image_indices.size() <= impl_->capacity, "GPU augmentation batch exceeds preallocated capacity");
 require(image_keys.size() == batch.image_indices.size(), "GPU augmentation key count does not match the batch");
 require(staging_slot < kStagingSlots, "GPU augmentation staging slot is out of range");
 require(
  batch.input_slots.empty() || (batch.input_format == GpuAugmentationInputFormat::PlanarFloat32 && batch.input_slots.size() == batch.image_indices.size()),
  "augmentation input slots have invalid shape");
 require(donor_batch.image_slots.empty() || (donor_batch.image_slots.size() == donors.size() && donor_batch.image_slots.size() <= impl_->capacity),
         "augmentation donor slots have invalid shape");
 for (const auto* input : batch.input_slots) require(input != nullptr, "augmentation input slot is null");
 for (const auto* input : donor_batch.image_slots) require(input != nullptr, "augmentation donor slot is null");
 require(batch.input_format == GpuAugmentationInputFormat::PlanarFloat32 || batch.input_format == GpuAugmentationInputFormat::Rgba8,
         "GPU augmentation input format is invalid");
 require(batch.output_domain == GpuAugmentationOutputDomain::ModelNormalized || batch.output_domain == GpuAugmentationOutputDomain::UnitRgb,
         "GPU augmentation output domain is invalid");
 require(donor_batch.selection == GpuAugmentationDonorSelection::Aligned || donor_batch.selection == GpuAugmentationDonorSelection::Cached,
         "GPU augmentation donor selection is invalid");
 if (batch.image_indices.empty()) {
  impl_->prepare_plan(batch, image_keys, donors, donor_batch.selection, impl_->staged_paste.data(), impl_->staged_parameters.data());
  return impl_->plan;
 }
 // A null handle is CUDA's valid default stream, including Torch's current default stream.
 require((batch.input != nullptr || batch.input_slots.size() == batch.image_indices.size()) && batch.output != nullptr,
         "GPU augmentation requires input and output storage");
 if (impl_->copy_paste) {
  const std::size_t expected_donors = donor_batch.selection == GpuAugmentationDonorSelection::Aligned ? batch.image_indices.size() : impl_->capacity;
  require(donors.size() == expected_donors, "GPU augmentation donor metadata does not match its selection policy");
 }
 if (impl_->staging_pending[staging_slot]) {
  CheckSettlement(event_wait_(impl_->staging_complete[staging_slot]), "cudaEventSynchronize for augmentation staging reuse");
  impl_->staging_pending[staging_slot] = false;
  impl_->custody[staging_slot] = {};
 }
 std::uint64_t* staged_keys = impl_->staged_keys.data() + staging_slot * impl_->capacity;
 if (explicit_keys) { std::memcpy(staged_keys, image_keys.data(), image_keys.size_bytes()); }
 float* staged_paste = impl_->staged_paste.data() + staging_slot * impl_->capacity * static_cast<std::size_t>(kGpuCopyPasteParameterCount);
 float* staged_parameters = impl_->staged_parameters.data() + staging_slot * impl_->capacity * static_cast<std::size_t>(kGpuAugmentationParameterCount);
 impl_->prepare_plan(batch, image_keys, donors, donor_batch.selection, staged_paste, staged_parameters);
 bool has_paste = false;
 bool needs_donor_masks = false;
 bool needs_donor_boxes = false;
 if (impl_->copy_paste) {
  for (std::size_t image = 0U; image < batch.image_indices.size(); ++image) {
   const std::int64_t donor_slot = impl_->plan.images[image].paste_donor_slot;
   if (donor_slot < 0) { continue; }
   has_paste = true;
   if (donors[static_cast<std::size_t>(donor_slot)].has_mask) {
    needs_donor_masks = true;
   } else {
    needs_donor_boxes = true;
   }
  }
  require(!has_paste || donor_batch.images != nullptr || !donor_batch.image_slots.empty(), "copy-paste augmentation requires donor images");
  require(!needs_donor_boxes || donor_batch.boxes != nullptr, "box copy-paste augmentation requires donor boxes");
  require(!needs_donor_masks || (donor_batch.masks != nullptr && donor_batch.mask_words > 0), "mask copy-paste augmentation requires donor masks");
 }
 const std::size_t converted_count = batch.image_indices.size() * 3U * static_cast<std::size_t>(impl_->height) * static_cast<std::size_t>(impl_->width);
 if (batch.input_format == GpuAugmentationInputFormat::Rgba8 && converted_count > impl_->converted_input.capacity()) {
  if (impl_->execution_pending) {
   CheckSettlement(event_wait_(impl_->execution_complete), "cudaEventSynchronize before growing augmentation conversion workspace");
   impl_->execution_pending = false;
  }
  impl_->converted_input.ensure(converted_count);
 }
 // From the first stream operation onward, every failure path must settle the stream before
 // executor-owned staging or device storage can be reused or destroyed. The final execution
 // event becomes the normal lifetime fence only after cudaEventRecord succeeds.
 impl_->custody[staging_slot] = {batch.input_custody, batch.output_custody, donor_batch.image_custody};
 try {
  if (impl_->execution_pending) {
   ensure_cuda_ok(cudaStreamWaitEvent(stream, impl_->execution_complete, 0), "cudaStreamWaitEvent for augmentation workspace reuse");
  }
  if (explicit_keys) {
   ensure_cuda_ok(cudaMemcpyAsync(impl_->keys.data(), staged_keys, image_keys.size_bytes(), cudaMemcpyHostToDevice, stream),
                  "cudaMemcpyAsync for augmentation keys");
  }
  if (impl_->copy_paste) {
   ensure_cuda_ok(
    cudaMemcpyAsync(impl_->paste_parameters.data(), staged_paste,
                    batch.image_indices.size() * static_cast<std::size_t>(kGpuCopyPasteParameterCount) * sizeof(float), cudaMemcpyHostToDevice, stream),
    "cudaMemcpyAsync for augmentation copy-paste parameters");
  }
  ensure_cuda_ok(
   cudaMemcpyAsync(impl_->parameters.data(), staged_parameters,
                   batch.image_indices.size() * static_cast<std::size_t>(kGpuAugmentationParameterCount) * sizeof(float), cudaMemcpyHostToDevice, stream),
   "cudaMemcpyAsync for augmentation parameters");
  auto* slots = impl_->staged_slots.data() + staging_slot * impl_->capacity * 2U;
  const auto upload_slots = [&](std::span<const float* const> source, auto& destination, std::size_t offset) {
   if (source.empty()) return;
   std::copy(source.begin(), source.end(), slots + offset);
   ensure_cuda_ok(cudaMemcpyAsync(destination.data(), slots + offset, source.size_bytes(), cudaMemcpyHostToDevice, stream), "augmentation input slot upload");
  };
  upload_slots(batch.input_slots, impl_->input_slots, 0U);
  upload_slots(donor_batch.image_slots, impl_->donor_slots, impl_->capacity);
  const auto* input_slots = batch.input_slots.empty() ? nullptr : impl_->input_slots.data();
  const auto* donor_slots = donor_batch.image_slots.empty() ? nullptr : impl_->donor_slots.data();
  const GpuAugmentationLaunchConfig launch = launch_config(impl_->config);
  const float* augmentation_input = static_cast<const float*>(batch.input);
  if (batch.input_format == GpuAugmentationInputFormat::Rgba8) {
   launch_gpu_rgba8_to_planar_float(static_cast<const std::uint8_t*>(batch.input), impl_->converted_input.data(),
                                    static_cast<std::int64_t>(batch.image_indices.size()), impl_->height, impl_->width, stream);
   augmentation_input = impl_->converted_input.data();
  }
  const bool has_prepared = impl_->prepare_reductions(
   batch, donor_batch, augmentation_input, batch.input_format == GpuAugmentationInputFormat::Rgba8 ? std::shared_ptr<const void>(impl_) : batch.input_custody,
   stream, staging_slot, *this);
  const auto* prepared = has_prepared ? impl_->prepared->views.data() : nullptr;
  const auto batch_size = static_cast<std::int64_t>(batch.image_indices.size());
  auto* const paste_parameters = impl_->copy_paste ? impl_->paste_parameters.data() : nullptr;
  const auto* const donor_images = has_paste ? donor_batch.images : nullptr;
  const auto* const donor_masks = needs_donor_masks ? donor_batch.masks : nullptr;
  const auto* const donor_boxes = needs_donor_boxes ? donor_batch.boxes : nullptr;
  const auto donor_mask_words = needs_donor_masks ? donor_batch.mask_words : 0;
  if (explicit_keys) {
   launch_gpu_augmentation_images_explicit(augmentation_input, batch.output, impl_->parameters.data(), paste_parameters, donor_images, donor_masks, donor_boxes,
                                           donor_mask_words, impl_->keys.data(), batch_size, impl_->height, impl_->width, launch, impl_->remap,
                                           batch.output_domain, stream, input_slots, donor_slots, prepared);
  } else {
   launch_gpu_augmentation_images(augmentation_input, batch.output, impl_->parameters.data(), paste_parameters, donor_images, donor_masks, donor_boxes,
                                  donor_mask_words, batch_size, impl_->height, impl_->width, launch, seed, epoch, rank, sequence, impl_->remap,
                                  batch.output_domain, stream, input_slots, donor_slots, prepared);
  }
  ensure_cuda_ok(cudaEventRecord(impl_->staging_complete[staging_slot], stream), "cudaEventRecord for augmentation staging consumption");
  impl_->staging_pending[staging_slot] = true;
  ensure_cuda_ok(cudaEventRecord(impl_->execution_complete, stream), "cudaEventRecord for augmentation execution");
  impl_->execution_pending = true;
 } catch (...) {
  const auto failure = std::current_exception();
  if (!impl_) std::rethrow_exception(failure);
  if (!impl_->retirement.admission_open()) {
   Retire(impl_->retirement.fact().first_failure);
   std::rethrow_exception(failure);
  }
  const auto settled = stream_wait_(stream);
  if (settled != cudaSuccess) {
   Retire(settled);
  } else {
   impl_->custody[staging_slot] = {};
   impl_->reductions.clear();
  }
  std::rethrow_exception(failure);
 }
 return impl_->plan;
}
const AugmentationBatchPlan& GpuAugmentationExecutor::plan() const {
 RequireActive();
 return impl_->plan;
}
std::string GpuAugmentationExecutor::prepared_image_diagnostic(const std::size_t image, const std::size_t staging_slot) const {
 RequireActive();
 require(image < impl_->plan.active_size && staging_slot < kStagingSlots, "augmentation prepared diagnostic image or staging slot is unavailable");
 const auto offset = staging_slot * impl_->capacity + image;
 constexpr auto parameter_count = static_cast<std::size_t>(kGpuAugmentationParameterCount);
 constexpr auto paste_parameter_count = static_cast<std::size_t>(kGpuCopyPasteParameterCount);
 std::ostringstream output;
 output.imbue(std::locale::classic());
 output << std::setprecision(std::numeric_limits<float>::max_digits10);
 output << "{\"image_key\":" << impl_->plan.images[image].erasure.key << ",\"image_slot\":" << image << ",\"staging_slot\":" << staging_slot
        << ",\"source_width\":" << impl_->width << ",\"source_height\":" << impl_->height << ",\"config\":";
 append_prepared_diagnostic(output, impl_->config);
 output << ",\"plan\":";
 append_prepared_diagnostic(output, impl_->plan.images[image]);
 output << ",\"parameters\":";
 append_prepared_diagnostic(output, std::span{impl_->staged_parameters.data() + offset * parameter_count, parameter_count});
 output << ",\"parameter_indices\":{";
 bool first = true;
 for (const auto entry : mmltk::frameworks::reflection::kReflectedEnumEntries<augment_math::ParameterIndex>) {
  if (!std::exchange(first, false)) output << ',';
  output << '"' << entry.name << "\":" << static_cast<unsigned int>(entry.value);
 }
 output << "},\"paste_parameters\":";
 if (impl_->copy_paste)
  append_prepared_diagnostic(output, std::span{impl_->staged_paste.data() + offset * paste_parameter_count, paste_parameter_count});
 else
  output << "null";
 output << '}';
 return std::move(output).str();
}
bool GpuAugmentationExecutor::enabled() const {
 RequireActive();
 return impl_->config.enabled;
}
bool GpuAugmentationExecutor::transforms_geometry() const {
 RequireActive();
 return impl_->transforms_geometry;
}
bool GpuAugmentationExecutor::copy_paste_enabled() const {
 RequireActive();
 return impl_->copy_paste;
}
bool GpuAugmentationExecutor::remaps_pixels() const {
 RequireActive();
 return impl_->remap;
}
std::size_t GpuAugmentationExecutor::batch_capacity() const {
 RequireActive();
 return impl_->capacity;
}
std::size_t GpuAugmentationExecutor::workspace_capacity_bytes() const {
 RequireActive();
 return device_capacity_bytes() + pinned_capacity_bytes();
}
std::size_t GpuAugmentationExecutor::device_capacity_bytes() const {
 RequireActive();
 return impl_->converted_input.bytes() + impl_->parameters.bytes() + impl_->keys.bytes() + impl_->input_slots.bytes() + impl_->donor_slots.bytes() +
        impl_->paste_parameters.bytes() + (impl_->prepared ? impl_->prepared->pixels.bytes() + impl_->prepared->views.bytes() : 0U);
}
std::size_t GpuAugmentationExecutor::pinned_capacity_bytes() const {
 RequireActive();
 return impl_->staged_slots.bytes() + impl_->staged_keys.bytes() + impl_->staged_paste.bytes() + impl_->staged_parameters.bytes() +
        (impl_->prepared ? impl_->prepared->staged.bytes() : 0U);
}
void normalize_gpu_batch(const float* input, void* output, const std::int64_t active_batch_size, const std::int64_t output_batch_size, const int height,
                         const int width, const GpuPreprocessOutputType output_type, cudaStream_t stream) {
 launch_gpu_batch_normalization(input, output, active_batch_size, output_batch_size, height, width, output_type, stream);
}
}  // namespace mmltk::backend::models::rfdetr
