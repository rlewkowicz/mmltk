#include "detail/training_data_plan.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include <algorithm>
#include <bit>
#include "src/backend/models/rfdetr/augmentation/annotation_support.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augmentation_donor_index.h"
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
namespace mmltk::backend::models::rfdetr {
namespace {
const mmltk::backend::data::PackedInstance& donor_instance(const mmltk::backend::data::DatasetLoader& loader, const TrainingDonorDescriptor& descriptor) {
 if (descriptor.image_index >= loader.num_images()) throw std::invalid_argument("logical donor image is outside dataset");
 const auto& entry = loader.label_index()[descriptor.image_index];
 if (descriptor.annotation_index >= entry.num_instances) throw std::invalid_argument("logical donor annotation is outside image");
 const auto& instance = loader.label_data()[entry.label_begin + descriptor.annotation_index];
 if (instance.is_crowd()) throw std::invalid_argument("crowd annotation cannot enter logical donor history");
 return instance;
}
std::span<const mmltk::backend::data::RLEPair> original_donor_support(const mmltk::backend::data::DatasetLoader& loader, const mmltk::backend::data::PackedInstance& instance) {
 // DatasetLoader owns the immutable mapping and validates every annotation/RLE
 // span before exposing it. Borrow the original support without remapping it.
 if (!instance.mask_rle_pairs) return {};
 return {loader.rle_data() + instance.mask_rle_offset / sizeof(mmltk::backend::data::RLEPair), instance.mask_rle_pairs};
}
std::vector<TrainingImageClasses> image_classes(const mmltk::backend::data::DatasetLoader& loader) {
 std::vector<TrainingImageClasses> images(loader.num_images());
 for (std::size_t image = 0; image < images.size(); ++image) {
  const auto& index = loader.label_index()[image];
  for (std::size_t annotation = 0; annotation < index.num_instances; ++annotation) {
   const auto& instance = loader.label_data()[index.label_begin + annotation];
   if (!instance.is_crowd()) images[image].classes.push_back(instance.class_id);
  }
 }
 return images;
}
}  // namespace
TrainingDataPlan::TrainingDataPlan(const mmltk::backend::data::DatasetLoader& loader, const TrainRequest& request) : TrainingDataPlan(image_classes(loader), loader.num_classes(), request) {}
TrainingDataPlan::TrainingDataPlan(std::vector<TrainingImageClasses> images, std::uint32_t classes, const TrainRequest& request)
    : membership_(std::move(images)), policy_(request.data_policy), batch_(request.batch_size), contributions_(derive_execution_facts(request, 0).microbatches_per_attempt) {
 if (classes > mmltk::backend::data::MAX_CLASSES) throw std::invalid_argument("training class catalog exceeds compiled format");
 support_.resize(classes);
 if (membership_.empty() || membership_.size() > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("training image membership is empty or too large");
 for (auto& image : membership_) {
  std::ranges::sort(image.classes);
  image.classes.erase(std::unique(image.classes.begin(), image.classes.end()), image.classes.end());
  for (auto cls : image.classes) {
   if (cls >= classes) throw std::invalid_argument("training class is outside catalog");
   ++support_[cls];
  }
 }
 if (request.lane_configuration.mode == TrainLaneMode::SharedGradients)
  shards_.push_back({0, static_cast<std::uint64_t>(request.seed), {}, std::vector<std::uint64_t>(classes)});
 else
  for (const auto& model : request.lane_configuration.models) shards_.push_back({model.model_id, model.seed, {}, std::vector<std::uint64_t>(classes)});
 std::ranges::sort(shards_, {}, &TrainingShard::model_id);
 std::vector<std::uint32_t> order(membership_.size());
 std::iota(order.begin(), order.end(), 0);
 std::vector<std::uint64_t> rarity;
 if (policy_.balancing != TrainBalancing::Off) {
  rarity.resize(membership_.size(), std::numeric_limits<std::uint64_t>::max());
  for (std::size_t image = 0; image < membership_.size(); ++image)
   for (auto cls : membership_[image].classes) rarity[image] = std::min(rarity[image], support_[cls]);
 }
 std::mt19937_64 rng(static_cast<std::uint64_t>(request.seed));
 std::shuffle(order.begin(), order.end(), rng);
 if (policy_.balancing != TrainBalancing::Off) std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return rarity[a] < rarity[b]; });
 const auto capacity = [&](std::size_t shard) { return membership_.size() / shards_.size() + (shard < membership_.size() % shards_.size()); };
 for (auto image : order) {
  std::size_t chosen = shards_.size();
  double best = std::numeric_limits<double>::infinity();
  for (std::size_t shard = 0; shard < shards_.size(); ++shard) {
   if (shards_[shard].images.size() == capacity(shard)) continue;
   double score = static_cast<double>(shards_[shard].images.size()) / static_cast<double>(capacity(shard));
   if (policy_.balancing != TrainBalancing::Off)
    for (auto cls : membership_[image].classes) score += static_cast<double>(shards_[shard].unique_support[cls]) / static_cast<double>(support_[cls]);
   if (score < best) {
    chosen = shard;
    best = score;
   }
  }
  if (chosen == shards_.size()) throw std::logic_error("training shard capacity exhausted");
  auto& shard = shards_[chosen];
  shard.images.push_back(image);
  for (auto cls : membership_[image].classes) ++shard.unique_support[cls];
 }
 for (auto& shard : shards_)
  for (std::uint32_t cls = 0; cls < classes; ++cls)
   if (!shard.unique_support[cls]) shard.missing_classes.push_back(cls);
 hash_ = training_stochastic_key(request.seed, batch_, contributions_, static_cast<std::uint64_t>(policy_.balancing));
 hash_ = training_stochastic_key(
  hash_, std::bit_cast<std::uint64_t>(policy_.rare_threshold), std::bit_cast<std::uint64_t>(policy_.maximum_repeat_factor), std::bit_cast<std::uint64_t>(policy_.maximum_draw_multiplier));
 for (const auto& shard : shards_) {
  hash_ = training_stochastic_key(hash_, shard.model_id, shard.seed, shard.images.size());
  for (auto image : shard.images) {
   hash_ = training_stochastic_key(hash_, image, membership_[image].classes.size(), 0);
   for (auto cls : membership_[image].classes) hash_ = training_stochastic_key(hash_, cls, 0, 0);
  }
 }
}
TrainingEpochDraws TrainingDataPlan::epoch(std::size_t model, std::uint64_t epoch_number) const {
 const auto& shard = shards_.at(model);
 auto schedule = std::make_shared<mmltk::backend::data::DatasetIndexSchedule>();
 auto& draws = schedule->image_indices;
 draws = shard.images;
 const auto seed = training_stochastic_key(shard.seed, shard.model_id, epoch_number, 0);
 std::mt19937_64 rng(seed);
 if (policy_.balancing == TrainBalancing::RareRepeatsStratified) {
  const auto maximum_value = std::floor(policy_.maximum_draw_multiplier * static_cast<double>(shard.images.size()));
  if (!std::isfinite(maximum_value) || maximum_value < static_cast<double>(draws.size()) || maximum_value >= static_cast<double>(std::numeric_limits<std::size_t>::max()))
   throw std::overflow_error("training repeated draw capacity is invalid");
  const auto capacity = static_cast<std::size_t>(maximum_value) - draws.size();
  std::vector<std::uint32_t> extras;
  std::uint64_t proposed = 0;
  if (capacity)
   for (auto image : shard.images) {
    double repeat = 1;
    for (auto cls : membership_[image].classes) repeat = std::max(repeat, std::sqrt(policy_.rare_threshold * static_cast<double>(membership_.size()) / static_cast<double>(support_[cls])));
    repeat = std::min(repeat, policy_.maximum_repeat_factor);
    const auto whole = static_cast<std::size_t>(repeat);
    const auto key = training_stochastic_key(seed, shard.model_id, epoch_number, image, 1);
    const auto fractional = static_cast<double>(key >> 11) * 0x1.0p-53;
    const auto count = whole - 1 + (fractional < repeat - static_cast<double>(whole));
    if (proposed > std::numeric_limits<std::uint64_t>::max() - count) throw std::overflow_error("training repeated draws overflow");
    // Reservoir thinning retains a uniform seeded subset without materializing
    // rejected candidates. Base occurrences never enter the reservoir.
    for (std::size_t extra = 0; extra < count; ++extra) {
     ++proposed;
     if (extras.size() < capacity) {
      if (extras.size() == extras.capacity()) extras.reserve(extras.size() + std::min(capacity - extras.size(), std::max<std::size_t>(1, extras.size())));
      extras.push_back(image);
     } else {
      const auto selected = std::uniform_int_distribution<std::uint64_t>(0, proposed - 1)(rng);
      if (selected < capacity) extras[selected] = image;
     }
    }
   }
  std::shuffle(extras.begin(), extras.end(), rng);
  draws.insert(draws.end(), extras.begin(), extras.end());
 }
 std::shuffle(draws.begin(), draws.end(), rng);
 const auto window = checked_training_product(batch_, contributions_);
 const auto usable = draws.size() / window * window;
 if (!usable) throw std::invalid_argument("training model has zero complete global accumulation windows");
 TrainingEpochDraws result{schedule, std::vector<std::uint64_t>(support_.size()), draws.size() - usable, usable / batch_, usable / window};
 draws.resize(usable);
 schedule->draw_keys.reserve(usable);
 schedule->microbatch_keys.reserve(result.microbatches);
 for (std::size_t microbatch = 0; microbatch < result.microbatches; ++microbatch)
  schedule->microbatch_keys.push_back(training_stochastic_key(shard.seed, shard.model_id, epoch_number, microbatch, 0x4d42));
 for (std::size_t occurrence = 0; occurrence < draws.size(); ++occurrence) {
  for (auto cls : membership_[draws[occurrence]].classes) ++result.repeated_exposure[cls];
  schedule->draw_keys.push_back(training_stochastic_key(shard.seed, shard.model_id, epoch_number, occurrence, draws[occurrence]));
 }
 return result;
}
TrainingRankSlice TrainingDataPlan::rank_slice(std::uint32_t rank, std::uint32_t world) const { return training_rank_slice(batch_, rank, world); }
std::shared_ptr<const mmltk::backend::data::DatasetIndexSchedule> TrainingDataPlan::rank_schedule(const TrainingEpochDraws& global, std::uint32_t rank, std::uint32_t world) const {
 if (!world || rank >= world) throw std::invalid_argument("invalid training rank slice");
 auto local = std::make_shared<mmltk::backend::data::DatasetIndexSchedule>();
 const auto [begin, count] = rank_slice(rank, world);
 if (count) local->microbatch_keys = global.schedule->microbatch_keys;
 local->image_indices.reserve(global.microbatches * count);
 local->draw_keys.reserve(global.microbatches * count);
 for (std::size_t batch = 0; batch < global.microbatches; ++batch)
  for (std::size_t image = 0; image < count; ++image) {
   const auto index = batch * batch_ + begin + image;
   local->image_indices.push_back(global.schedule->image_indices[index]);
   local->draw_keys.push_back(global.schedule->draw_keys[index]);
  }
 return local;
}
TrainingDonorHistory::TrainingDonorHistory(std::size_t streams, std::size_t batch) : batch_(batch) {
 const auto capacity = checked_training_product(streams, batch);
 if (!streams || !batch || capacity > kMaximumTrainingDonorSlots) throw std::invalid_argument("logical donor history capacity is invalid");
 slots_.resize(capacity);
 planned_.resize(batch);
 replacements_.resize(batch);
 metadata_.resize(batch);
}
TrainingDonorSource resolve_training_donor(const mmltk::backend::data::DatasetLoader& loader, const TrainingDonorDescriptor& descriptor) {
 TrainingDonorSource result;
 if (!descriptor.valid) return result;
 const auto& instance = donor_instance(loader, descriptor);
 result.support = original_donor_support(loader, instance);
 const auto mapped = map_augmentation_instance(instance, loader.image_width(), loader.image_height(), nullptr, result.support);
 result.metadata = {instance.class_id, descriptor.image_index, mapped.source_area_pixels, mapped.source_box_xyxy, instance.has_mask(),
  (static_cast<std::uint64_t>(descriptor.image_index) << 32) | descriptor.annotation_index};
 if (instance.has_mask()) {
  result.metadata.area = 0;
  for (const auto& run : result.support) result.metadata.area += static_cast<float>(run.length);
 }
 return result;
}
std::span<const TrainingDonorDescriptor> TrainingDonorHistory::plan(std::size_t stream, std::span<const std::uint64_t> keys, std::span<const std::uint32_t> images) {
 if (stream >= slots_.size() / batch_ || keys.size() != batch_ || images.size() != batch_) throw std::invalid_argument("logical donor plan shape differs");
 for (std::size_t slot = 0; slot < batch_; ++slot) {
  const auto& descriptor = slots_[stream * batch_ + slot];
  metadata_[slot].label = descriptor.valid ? 0 : -1;
  metadata_[slot].dataset_index = descriptor.image_index;
 }
 index_.rebuild(metadata_);
 std::fill(planned_.begin(), planned_.end(), TrainingDonorDescriptor{});
 for (std::size_t image = 0; image < batch_; ++image) {
  const auto chosen = index_.select_for_image(keys[image], images[image]);
  if (chosen >= 0) planned_[image] = slots_[stream * batch_ + chosen];
 }
 return planned_;
}
std::span<const TrainingDonorDescriptor> TrainingDonorHistory::admit(
 const mmltk::backend::data::DatasetLoader& loader, std::size_t stream, std::span<const std::uint64_t> keys, std::span<const std::uint32_t> images, const GpuAugmentationConfig& config) {
 auto donors = plan(stream, keys, images);
 if (!config.enabled || config.copy_paste_probability <= 0) return donors;
 std::fill(replacements_.begin(), replacements_.end(), TrainingDonorDescriptor{});
 for (std::size_t image = 0; image < batch_; ++image) {
  if (images[image] >= loader.num_images()) throw std::invalid_argument("logical donor source image is outside dataset");
  const auto donor = resolve_training_donor(loader, donors[image]);
  auto plan = plan_augmentation_image(config, keys[image], images[image], &donor.metadata, image);
  plan.paste_support = donor.support.data();
  plan.paste_support_count = donor.support.size();
  const auto& entry = loader.label_index()[images[image]];
  std::int64_t candidates = 0;
  for (std::uint32_t ordinal = 0; ordinal < entry.num_instances; ++ordinal) {
   const auto& instance = loader.label_data()[entry.label_begin + ordinal];
   if (instance.is_crowd()) continue;
   const auto support = original_donor_support(loader, instance);
   if (!map_augmentation_instance(instance, loader.image_width(), loader.image_height(), &plan, support).visible) continue;
   if (augmentation_reservoir_select(plan.cache_choice, ++candidates, ordinal)) replacements_[image] = {images[image], ordinal, true};
  }
 }
 replace(stream, replacements_);
 return donors;
}
void TrainingDonorHistory::replace(std::size_t stream, std::span<const TrainingDonorDescriptor> replacements) {
 if (stream >= slots_.size() / batch_ || replacements.size() != batch_) throw std::invalid_argument("logical donor replacement shape differs");
 for (std::size_t index = 0; index < batch_; ++index)
  if (replacements[index].valid) slots_[stream * batch_ + index] = replacements[index];
}
void TrainingDonorHistory::restore(const mmltk::backend::data::DatasetLoader& loader, std::span<const TrainingDonorDescriptor> saved) {
 if (saved.size() != slots_.size()) throw std::invalid_argument("saved logical donor inventory differs");
 for (const auto& descriptor : saved) {
  if (!descriptor.valid) continue;
  (void)donor_instance(loader, descriptor);
 }
 std::copy(saved.begin(), saved.end(), slots_.begin());
}
}  // namespace mmltk::backend::models::rfdetr
