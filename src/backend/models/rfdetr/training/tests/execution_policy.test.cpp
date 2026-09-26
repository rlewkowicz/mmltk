#include "src/backend/ml/torch/tests/catch_support.h"
#include "detail/training_data_plan.h"
#include "src/backend/data/dataset_loader.h"
#include "detail/training_epoch_policy.h"
#include "detail/training_schedule.h"
#include "src/backend/models/rfdetr/contract/workflow_requests.h"
#include "src/backend/models/rfdetr/core/detail/semantic_sampling.h"
#include "src/backend/models/rfdetr/core/detail/training_supervision.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <catch2/catch_approx.hpp>
namespace {
namespace r = mmltk::backend::models::rfdetr;
r::TrainRecipeSettings sgd_recipe() {
 r::TrainRecipeSettings recipe;
 recipe.optimizer = r::TrainOptimizerKind::SGD;
 r::reset_train_recipe(recipe);
 return recipe;
}
TEST_CASE("Ultralytics scheduler retains literal warmup exit values across K overflow and Resume", "[rfdetr][training][schedule]") {
 auto recipe = sgd_recipe();
 recipe.warmup_epochs = .375;
 r::TrainingSchedule continuous(recipe, {.01, .01}, {r::TrainingGroupRole::Bias, r::TrainingGroupRole::Ordinary}, 4, 4, 2);
 continuous.begin_epoch(0);
 continuous.consume_microbatch();
 CHECK(continuous.state().warmup_microbatches == 2);
 CHECK(continuous.state().absolute_lrs[0] == Catch::Approx(.1));
 CHECK(continuous.state().absolute_lrs[1] == 0);
 CHECK(continuous.state().held_momentum == Catch::Approx(.8));
 continuous.consume_microbatch();
 continuous.prepare_attempt();
 CHECK(continuous.state().absolute_lrs[0] == Catch::Approx(.055));
 CHECK(continuous.state().absolute_lrs[1] == Catch::Approx(.005));
 CHECK(continuous.state().held_momentum == Catch::Approx(.85));
 continuous.finish_attempt(false);
 CHECK(continuous.state().successful_updates == 0);
 CHECK(continuous.state().consumed_attempts == 1);
 r::TrainingSchedule resumed(recipe, {.01, .01}, {r::TrainingGroupRole::Bias, r::TrainingGroupRole::Ordinary}, 8, 6, 2);
 resumed.restore(continuous.state());
 resumed.begin_epoch(0);  // No replay of the already consumed epoch event.
 CHECK(resumed.state() == continuous.state());
 for (auto* schedule : {&continuous, &resumed}) {
  schedule->consume_microbatch();
  schedule->consume_microbatch();
  schedule->prepare_attempt();
  schedule->finish_attempt(true);
  CHECK(schedule->state().absolute_lrs[0] == Catch::Approx(.055));
  CHECK(schedule->state().held_momentum == Catch::Approx(.85));
  schedule->begin_epoch(1);
  CHECK(schedule->state().absolute_lrs[0] == Catch::Approx(.007525));
  CHECK(schedule->state().absolute_lrs[1] == Catch::Approx(.007525));
  CHECK(schedule->state().held_momentum == Catch::Approx(.85));
 }
 CHECK(resumed.state() == continuous.state());
 resumed.begin_epoch(4);
 CHECK(resumed.state().absolute_lrs[0] == Catch::Approx(.0001));
 CHECK(resumed.state().original_epochs == 4);
 CHECK(resumed.state().nb_ref == 4);
 CHECK(resumed.state().steps_ref == 2);
 REQUIRE_THROWS(resumed.begin_epoch(3));
}
TEST_CASE("Ultralytics rounds half even and keeps nw zero and one semantics", "[rfdetr][training][schedule]") {
 auto recipe = sgd_recipe();
 for (const auto& [warmup, expected] : {std::pair{.625, 2U}, std::pair{.875, 4U}, std::pair{.125, 0U}, std::pair{.25, 1U}}) {
  recipe.warmup_epochs = warmup;
  r::TrainingSchedule schedule(recipe, {.01}, {r::TrainingGroupRole::Ordinary}, 4, 4, 1);
  CHECK(schedule.state().warmup_microbatches == expected);
  schedule.begin_epoch(0);
  for (int i = 0; i < 5; ++i) schedule.consume_microbatch();
  if (expected == 0) CHECK(schedule.state().held_momentum == Catch::Approx(.9));
  if (expected == 1) {
   CHECK(schedule.state().held_momentum == Catch::Approx(.8));
   CHECK(schedule.state().absolute_lrs[0] == 0);
  }
 }
 recipe.warmup_epochs = 3;
 r::TrainingSchedule one(recipe, {.01}, {r::TrainingGroupRole::Bias}, 1, 4, 1);
 one.begin_epoch(0);
 one.consume_microbatch();
 CHECK(one.state().warmup_microbatches == 0);
 CHECK(one.state().absolute_lrs[0] == .01);
 CHECK(one.state().held_momentum == .9);
 r::TrainingSchedule hundred(recipe, {.01}, {r::TrainingGroupRole::Ordinary}, 100, 4, 1);
 hundred.begin_epoch(99);
 CHECK(hundred.state().absolute_lrs[0] == Catch::Approx(.000199));
 auto bad = hundred.state();
 bad.held_momentum = std::numeric_limits<double>::quiet_NaN();
 REQUIRE_THROWS(hundred.restore(bad));
 bad = hundred.state();
 ++bad.warmup_microbatches;
 REQUIRE_THROWS(hundred.restore(bad));
 REQUIRE_THROWS(r::TrainingSchedule(recipe, {.01}, {}, 4, 4, 1));
}
TEST_CASE("Native schedule uses consumed attempts saved reference and independent momentum restoration", "[rfdetr][training][schedule]") {
 auto recipe = sgd_recipe();
 recipe.lr_scheduler = r::TrainLrSchedulerKind::Cosine;
 recipe.warmup_epochs = 1.5;
 r::TrainingSchedule schedule(recipe, {.01}, {r::TrainingGroupRole::Ordinary}, 4, 6, 2);
 schedule.begin_epoch(0);
 for (std::uint64_t attempt = 0; attempt < 6; ++attempt) {
  schedule.consume_microbatch();
  schedule.consume_microbatch();
  schedule.prepare_attempt();
  CHECK(schedule.state().absolute_lrs[0] == Catch::Approx(.01 * r::compute_lr_scale(recipe, attempt, 3, 12)));
  CHECK(schedule.state().held_momentum == Catch::Approx(r::compute_warmup_momentum(recipe, attempt, 3, .9)));
  schedule.finish_attempt(attempt != 2);
 }
 CHECK(r::compute_lr_scale(recipe, 3, 3, 12) == Catch::Approx(.75));
 CHECK(r::compute_warmup_momentum(recipe, 4, 3, .9) == Catch::Approx(.8 + .1 * 4 / 4.5));
 CHECK(r::compute_warmup_momentum(recipe, 5, 3, .9) == .9);
 CHECK(r::compute_lr_scale(recipe, 1000, 3, 12) == recipe.lr_min_factor);
 r::TrainingSchedule extended(recipe, {.01}, {r::TrainingGroupRole::Ordinary}, 8, 2, 2);
 extended.restore(schedule.state());
 extended.begin_epoch(1);
 extended.prepare_attempt();
 CHECK(extended.state().absolute_lrs[0] == Catch::Approx(.01 * r::compute_lr_scale(recipe, 6, 3, 24)));
 recipe.lr_scheduler = r::TrainLrSchedulerKind::Step;
 recipe.warmup_epochs = 0;
 recipe.lr_drop = 2;
 CHECK(r::compute_lr_scale(recipe, 5, 3, 24) == 1);
 CHECK(r::compute_lr_scale(recipe, 6, 3, 24) == .1);
 recipe.warmup_momentum = 0;
 CHECK(r::compute_warmup_momentum(recipe, 0, 3, .9) == .9);
}
TEST_CASE("Final epoch policies remain latched under extension and leave pending policies relative to new horizon", "[rfdetr][training][activation]") {
 r::TrainingEpochPolicy policy(2, 1);
 CHECK(policy.enter(1, 4) == r::TrainingEpochPolicyState{});
 auto saved = policy.enter(2, 4);
 CHECK(saved.encoder_unfrozen);
 CHECK_FALSE(saved.augmentation_disabled);
 r::TrainingEpochPolicy resumed(2, 1, saved);
 CHECK(resumed.enter(3, 8) == saved);
 CHECK(resumed.enter(7, 8).augmentation_disabled);
 CHECK(resumed.enter(8, 100).augmentation_disabled);
 r::TrainingEpochPolicy disabled(0, 0);
 CHECK(disabled.enter(100, 1) == r::TrainingEpochPolicyState{});
 r::TrainingEpochPolicy immediate(10, 10);
 CHECK(immediate.enter(0, 2) == (r::TrainingEpochPolicyState{true, true}));
 REQUIRE_THROWS(r::TrainingEpochPolicy(-1, 0));
}
TEST_CASE("Sparse training shards preserve empty images distinct support and stable identities", "[rfdetr][training][data_plan]") {
 r::TrainRequest request;
 request.batch_size = 1;
 request.grad_accum_steps = 1;
 request.lanes = 3;
 request.lane_configuration.mode = r::TrainLaneMode::Independent;
 r::resize_training_models(request.lane_configuration, 3, request.recipe, request.seed);
 request.data_policy.balancing = r::TrainBalancing::Stratified;
 const std::vector<r::TrainingImageClasses> images{{{0, 0}}, {{0, 1}}, {}, {{1}}, {{2}}, {{0}}, {}, {{0, 2}}};
 r::TrainingDataPlan plan(images, 4, request);
 std::set<std::uint32_t> members;
 std::vector<std::uint64_t> support(4);
 for (std::size_t model = 0; model < plan.shards().size(); ++model) {
  const auto& shard = plan.shards()[model];
  CHECK(shard.model_id == model + 1);
  CHECK(shard.images.size() >= 2);
  CHECK(shard.images.size() <= 3);
  for (auto image : shard.images) CHECK(members.insert(image).second);
  for (std::size_t cls = 0; cls < support.size(); ++cls) support[cls] += shard.unique_support[cls];
  CHECK(std::ranges::find(shard.missing_classes, 3) != shard.missing_classes.end());
 }
 CHECK(members.size() == images.size());
 CHECK(support == std::vector<std::uint64_t>{4, 2, 2, 0});
 auto reordered = request;
 std::ranges::reverse(reordered.lane_configuration.models);
 r::TrainingDataPlan same(images, 4, reordered);
 CHECK((same.shards() == plan.shards()));
 CHECK(same.hash() == plan.hash());
 request.lane_configuration.mode = r::TrainLaneMode::SharedGradients;
 r::TrainingDataPlan shared(images, 4, request);
 REQUIRE(shared.shards().size() == 1);
 CHECK(shared.shards()[0].images.size() == images.size());
 REQUIRE_THROWS(r::TrainingDataPlan({{{4}}}, 4, request));
}
TEST_CASE("Repeated global windows rank slicing and semantic keys survive physical packing", "[rfdetr][training][data_plan]") {
 r::TrainRequest request;
 request.batch_size = 3;
 request.grad_accum_steps = 2;
 request.data_policy.balancing = r::TrainBalancing::RareRepeatsStratified;
 request.data_policy.rare_threshold = 1;
 request.data_policy.maximum_repeat_factor = 64;
 request.data_policy.maximum_draw_multiplier = 1.5;
 std::vector<r::TrainingImageClasses> images(12);
 for (std::uint32_t image = 0; image < 12; ++image) images[image].classes.push_back(image);
 r::TrainingDataPlan plan(images, 13, request);
 const auto epoch = plan.epoch(0, 2);
 CHECK(epoch.schedule->image_indices.size() == 18);
 CHECK(epoch.microbatches == 6);
 CHECK(epoch.attempts == 3);
 CHECK(epoch.unused_tail == 0);
 CHECK(std::set<std::uint32_t>(epoch.schedule->image_indices.begin(), epoch.schedule->image_indices.end()).size() == 12);
 CHECK(epoch.repeated_exposure[12] == 0);
 CHECK(plan.epoch(0, 2).schedule->draw_keys == epoch.schedule->draw_keys);
 CHECK(plan.epoch(0, 3).schedule->draw_keys != epoch.schedule->draw_keys);
 for (const std::uint32_t world : {1, 2, 4}) {
  std::vector<std::shared_ptr<const mmltk::backend::data::DatasetIndexSchedule>> ranks;
  for (std::uint32_t rank = 0; rank < world; ++rank) {
   ranks.push_back(plan.rank_schedule(epoch, rank, world));
   const auto slice = plan.rank_slice(rank, world);
   CHECK(ranks.back()->image_indices.size() == epoch.microbatches * slice.count);
   if (slice.count)
    CHECK(ranks.back()->microbatch_keys == epoch.schedule->microbatch_keys);
   else
    CHECK(ranks.back()->microbatch_keys.empty());
  }
  for (std::size_t batch = 0; batch < epoch.microbatches; ++batch) {
   std::vector<std::uint64_t> keys;
   for (const auto& rank : ranks) {
    const auto count = rank->draw_keys.size() / epoch.microbatches;
    keys.insert(keys.end(), rank->draw_keys.begin() + batch * count, rank->draw_keys.begin() + (batch + 1) * count);
   }
   CHECK(keys == std::vector<std::uint64_t>(epoch.schedule->draw_keys.begin() + batch * 3, epoch.schedule->draw_keys.begin() + (batch + 1) * 3));
  }
 }
 REQUIRE_THROWS(plan.rank_schedule(epoch, 1, 1));
 request.data_policy.balancing = r::TrainBalancing::Off;
 request.batch_size = 5;
 r::TrainingDataPlan tail(images, 13, request);
 CHECK(tail.epoch(0, 0).unused_tail == 2);
 request.batch_size = 13;
 r::TrainingDataPlan empty(images, 13, request);
 REQUIRE_THROWS(empty.epoch(0, 0));
}
TEST_CASE("Maximum rare repeats preserve base draws under tight deterministic capacity", "[rfdetr][training][data_plan]") {
 r::TrainRequest request;
 request.batch_size = 1;
 request.data_policy.balancing = r::TrainBalancing::RareRepeatsStratified;
 request.data_policy.rare_threshold = 1;
 request.data_policy.maximum_repeat_factor = 64;
 std::vector<r::TrainingImageClasses> images(4096);
 images[0].classes = {0};
 for (const double multiplier : {1.0, 1.001, 1.5}) {
  request.data_policy.maximum_draw_multiplier = multiplier;
  r::TrainingDataPlan plan(images, 1, request);
  const auto admitted = plan.epoch(0, 0);
  const auto reused = admitted.schedule;
  CHECK(admitted.schedule->image_indices.size() == 4096 + std::min<std::size_t>(63, static_cast<std::size_t>(std::floor(multiplier * 4096)) - 4096));
  CHECK(std::set<std::uint32_t>(reused->image_indices.begin(), reused->image_indices.end()).size() == 4096);
  CHECK(plan.epoch(0, 0).schedule->image_indices == reused->image_indices);
 }
}
TEST_CASE("Empty membership and base-only repeat policy retain deterministic task-sized schedules", "[rfdetr][training][data_plan]") {
 r::TrainRequest request;
 request.batch_size = 1;
 request.data_policy.balancing = r::TrainBalancing::RareRepeatsStratified;
 request.data_policy.rare_threshold = 1;
 request.data_policy.maximum_repeat_factor = 64;
 request.data_policy.maximum_draw_multiplier = 16;
 std::vector<r::TrainingImageClasses> images(65536);
 r::TrainingDataPlan empty(images, 1, request);
 const auto baseline = empty.epoch(0, 0);
 CHECK(baseline.schedule->image_indices.size() == images.size());
 CHECK(empty.epoch(0, 0).schedule->image_indices == baseline.schedule->image_indices);
 request.data_policy.maximum_draw_multiplier = 1;
 images.front().classes = {0};
 r::TrainingDataPlan base_only(images, 1, request);
 const auto draws = base_only.epoch(0, 0);
 CHECK(draws.schedule->image_indices.size() == images.size());
 CHECK(std::set<std::uint32_t>(draws.schedule->image_indices.begin(), draws.schedule->image_indices.end()).size() == images.size());
 CHECK(base_only.epoch(0, 0).schedule->draw_keys == draws.schedule->draw_keys);
}
TEST_CASE("Logical donor history separates streams excludes self and retains invalid replacements", "[rfdetr][training][data_plan]") {
 r::TrainingDonorHistory history(2, 2);
 const std::array<std::uint64_t, 2> keys{41, 42};
 const std::array<std::uint32_t, 2> images{7, 9};
 const std::array<r::TrainingDonorDescriptor, 2> sources{{{7, 1, true}, {9, 2, true}}};
 CHECK_FALSE(history.plan(0, keys, images)[0].valid);
 history.replace(0, sources);
 const auto planned_span = history.plan(0, keys, images);
 const std::vector<r::TrainingDonorDescriptor> planned(planned_span.begin(), planned_span.end());
 CHECK(planned[0] == sources[1]);
 CHECK(planned[1] == sources[0]);
 CHECK_FALSE(history.plan(1, keys, images)[0].valid);
 history.replace(0, std::array<r::TrainingDonorDescriptor, 2>{});
 CHECK(std::ranges::equal(history.plan(0, keys, images), planned));
 REQUIRE_THROWS(r::TrainingDonorHistory(std::numeric_limits<std::size_t>::max(), 2));
}
TEST_CASE("Logical donor plans retain stream identities for reordered consumers", "[rfdetr][training][data_plan]") {
 r::TrainingDonorHistory serial(2, 3), reordered(2, 3);
 const std::array<r::TrainingDonorDescriptor, 3> first{{{7, 1, true}, {9, 2, true}, {11, 3, true}}};
 const std::array<r::TrainingDonorDescriptor, 3> second{{{13, 4, true}, {15, 5, true}, {17, 6, true}}};
 for (auto* history : {&serial, &reordered}) {
  history->replace(0, first);
  history->replace(1, second);
 }
 const std::array<std::uint64_t, 3> keys{0, 42, std::numeric_limits<std::uint64_t>::max()};
 const std::array<std::uint32_t, 3> images{7, 13, 99};
 std::array<std::vector<r::TrainingDonorDescriptor>, 2> delivered;
 for (std::size_t stream = 0; stream < delivered.size(); ++stream) {
  const auto planned = serial.plan(stream, keys, images);
  delivered[stream].assign(planned.begin(), planned.end());
 }
 for (const std::size_t stream : {1U, 0U}) {
  CHECK(std::ranges::equal(reordered.plan(stream, keys, images), delivered[stream]));
  // Workers consume their own descriptor copy; subsequent planning/admission
  // cannot change a previously submitted identity or another stream's slots.
  auto replacement = stream == 0 ? first : second;
  replacement[1] = {};
  reordered.replace(stream, replacement);
 }
 CHECK((reordered.state() == serial.state()));
}
TEST_CASE("Semantic samples retain valid rows under rank slices padding group and layer changes", "[rfdetr][training][sampling]") {
 const auto keys = torch::tensor({INT64_C(19), INT64_C(0x3abcdef012345678), INT64_C(-7)}, torch::kInt64);
 const auto samples = r::semantic_uniform(keys, 34, 43);
 CHECK(torch::equal(samples.index({1}), r::semantic_uniform(keys.slice(0, 1, 2), 34, 43).index({0})));
 auto padded = torch::cat({keys, torch::zeros({5}, torch::kInt64)});
 CHECK(torch::equal(samples, r::semantic_uniform(padded, 34, 43).slice(0, 0, 3)));
 CHECK(torch::equal(samples.slice(1, 0, 10), r::semantic_uniform(keys, 10, 43)));
 CHECK_FALSE(torch::equal(samples, r::semantic_uniform(keys, 34, 44)));
 CHECK_FALSE(torch::equal(samples, r::semantic_uniform(torch::bitwise_xor(keys, 0x1234), 34, 43)));
 CHECK(samples.ge(0).all().item<bool>());
 CHECK(samples.lt(1).all().item<bool>());
 CHECK(r::semantic_coordinates(100, 0, 1, torch::kCPU).numel() == 0);
 REQUIRE_THROWS(r::semantic_uniform(keys, -1, 43));
}
TEST_CASE("Denoising owner preserves valid semantic annotations across local padding rank and worker sequences", "[rfdetr][training][sampling]") {
 r::NativeRfDetrConfig config;
 config.num_classes = 3;
 config.hidden_dim = 8;
 config.num_queries = 4;
 config.training_supervision.denoising.enabled = true;
 config.training_supervision.denoising.groups = 2;
 r::TrainingSupervisionImpl supervision(config, 2);
 supervision.initialize(42);
 r::PreparedTargets packed;
 packed.all_labels = torch::tensor({0, 1, 0}, torch::kInt64);
 packed.all_boxes = torch::tensor({{.4F, .5F, .2F, .1F}, {.5F, .4F, .1F, .2F}, {.6F, .5F, .2F, .2F}});
 packed.offsets = {0, 1};
 packed.counts = {1, 2};
 packed.sampling_keys = torch::tensor({INT64_C(3001), INT64_C(4001), INT64_C(4002)}, torch::kInt64);
 packed.microbatch_key = 0;  // Zero is a valid explicit key, never an absence marker.
 const auto full = supervision.prepare_denoising(packed, {42, 2, 0, 6}, torch::kCPU, torch::kFloat32);
 REQUIRE(full);
 r::PreparedTargets local = packed;
 local.all_labels = packed.all_labels.slice(0, 0, 1);
 local.all_boxes = packed.all_boxes.slice(0, 0, 1);
 local.sampling_keys = packed.sampling_keys.slice(0, 0, 1);
 local.offsets = {0};
 local.counts = {1};
 const auto sliced = supervision.prepare_denoising(local, {42, 2, 7, 99}, torch::kCPU, torch::kFloat32);
 REQUIRE(sliced);
 for (int group = 0; group < 2; ++group) {
  CHECK(torch::equal(full->normalized_references.index({0, group * 2}), sliced->normalized_references.index({0, group})));
  CHECK(torch::equal(full->content.index({0, group * 2}), sliced->content.index({0, group})));
 }
 CHECK(full->mask_sampling_seed == sliced->mask_sampling_seed);
 auto resumed = supervision.prepare_denoising(packed, {42, 2, 3, 1234}, torch::kCPU, torch::kFloat32);
 REQUIRE(resumed);
 CHECK(torch::equal(resumed->normalized_references, full->normalized_references));
 CHECK(torch::equal(resumed->content, full->content));
}
}  // namespace
