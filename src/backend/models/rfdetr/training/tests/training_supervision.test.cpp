#include <cuda_runtime.h>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <latch>
#include <memory>
#include <semaphore>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "archive_utils.h"
#include "catch2_compat.hpp"
#include "cuda_test_utils.hpp"
#include "src/backend/models/rfdetr/augmentation/tests/gpu_augment_test_support.h"
#include "src/backend/models/rfdetr/augmentation/tests/copy_paste_fixture.h"
#include "detail/checkpoint_private.h"
#include "detail/native_optimizer_private.h"
#include "detail/training_ops_private.h"
#include "detail/target_builder_private.h"
#include "model_access.h"
#include "model_state_fixture.h"
#include "model_state_access.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/models/rfdetr/training/train.h"
#include "test_fixture.h"
#include "torch_api.h"
#include "training_supervision.h"
#include "detection_ops.h"

import mmltk.backend.models.rfdetr.augmentation.augmentation_metadata;
import mmltk.backend.models.rfdetr.core.model;
import mmltk.backend.models.rfdetr.training.checkpoint;

#include "detail/gpu_augment_private.h"

namespace {

namespace rfdetr = mmltk::backend::models::rfdetr;
namespace torch_api = mmltk::backend::ml::torch_api;

[[nodiscard]] c10::cuda::CUDAStream training_test_stream() {
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    return c10::cuda::getStreamFromPool(false, 0);
}

#if defined(USE_C10D_NCCL)
class FailingCollectiveWork final : public c10d::Work {
   public:
    explicit FailingCollectiveWork(std::atomic<int>& waits) : c10d::Work(0, c10d::OpType::ALLREDUCE), waits_(&waits) {}

    bool wait(std::chrono::milliseconds = kNoTimeout) override {
        ++*waits_;
        throw std::runtime_error("deterministic collective wait failure");
    }

   private:
    std::atomic<int>* waits_;
};

class FailingCollectiveBackend final : public c10d::Backend {
   public:
    FailingCollectiveBackend() : c10d::Backend(0, 2) {}

    const std::string getBackendName() const override { return "failing-test-collective"; }

    c10::intrusive_ptr<c10d::Work> allreduce(std::vector<at::Tensor>&, const c10d::AllreduceOptions& = c10d::AllreduceOptions()) override {
        ++allreduces;
        return c10::make_intrusive<FailingCollectiveWork>(waits);
    }

    void abort() override { ++aborts; }

    std::atomic<int> allreduces = 0;
    std::atomic<int> waits = 0;
    std::atomic<int> aborts = 0;
};
#endif

rfdetr::NativeRfDetrConfig supervision_config() {
    rfdetr::NativeRfDetrConfig config;
    config.num_classes = 3;
    config.num_queries = 2;
    config.num_select = 2;
    config.dec_layers = 1;
    config.group_detr = 1;
    config.hidden_dim = 8;
    config.training_supervision.assignment = rfdetr::TrainAssignmentKind::MatchFree;
    return config;
}

void test_training_supervision_runtime_replication_is_one_shot() {
    const auto config = supervision_config();
    rfdetr::TrainingSupervisionImpl source(config);
    rfdetr::TrainingSupervisionImpl replica(config);
    source.initialize(71U);

    replica.install_replicated_initialized_runtime(config.training_supervision);
    MMLTK_ASSERT(source.initialized());
    MMLTK_ASSERT(replica.initialized());
    REQUIRE_THROWS(replica.install_replicated_initialized_runtime(config.training_supervision));

    auto incompatible_config = config;
    incompatible_config.training_supervision.match_free.rho = 0.75F;
    rfdetr::TrainingSupervisionImpl incompatible(incompatible_config);
    REQUIRE_THROWS(incompatible.install_replicated_initialized_runtime(config.training_supervision));
}

void test_checkpoint_supervision_config_and_deployment_pruning() {
    const auto path = std::filesystem::temp_directory_path() / "mmltk_rfdetr_training_supervision_archive.pt";
    auto config = supervision_config().training_supervision;
    config.denoising.enabled = true;
    config.denoising.groups = 7U;

    torch_api::OutputArchive output;
    rfdetr::detail::write_training_supervision_config(output, config);
    rfdetr::detail::write_state_archive(output, "state",
                                        {
                                            {"backbone.weight", torch_api::ones({1})},
                                            {"training_supervision.query_projection.weight", torch_api::ones({1})},
                                        });
    output.save_to(path.string());

    torch_api::InputArchive input;
    input.load_from(path.string());
    MMLTK_ASSERT(rfdetr::detail::read_training_supervision_config(input) == config);
    rfdetr::detail::require_resume_training_supervision_config(path, config);
    REQUIRE_THROWS(rfdetr::detail::require_resume_training_supervision_config(path, rfdetr::TrainingSupervisionConfig{}));
    torch_api::InputArchive state;
    input.read("state", state);
    MMLTK_ASSERT(rfdetr::require_int(state, "entry_count") == 1);

    torch_api::OutputArchive legacy_output;
    rfdetr::detail::write_training_supervision_config(legacy_output, {});
    rfdetr::detail::write_state_archive(legacy_output, "state", {});
    legacy_output.save_to(path.string());
    torch_api::InputArchive legacy_input;
    legacy_input.load_from(path.string());
    MMLTK_ASSERT(rfdetr::detail::read_training_supervision_config(legacy_input) == rfdetr::TrainingSupervisionConfig{});

    rfdetr::TrainingSupervisionConfig inactive_nondefault;
    inactive_nondefault.match_free.rho = 0.75F;
    torch_api::OutputArchive inactive_output;
    rfdetr::detail::write_training_supervision_config(inactive_output, inactive_nondefault);
    inactive_output.save_to(path.string());
    torch_api::InputArchive inactive_input;
    inactive_input.load_from(path.string());
    MMLTK_ASSERT(rfdetr::detail::read_training_supervision_config(inactive_input) == inactive_nondefault);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_feature_active_host_target_invariants() {
    const std::array<float, 4> valid{0.5F, 0.5F, 0.25F, 0.25F};
    rfdetr::validate_feature_active_target(0, valid, 2);
    REQUIRE_THROWS(rfdetr::validate_feature_active_target(2, valid, 2));

    auto invalid = valid;
    invalid[2] = 0.0F;
    REQUIRE_THROWS(rfdetr::validate_feature_active_target(0, invalid, 2));
    invalid = valid;
    invalid[0] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS(rfdetr::validate_feature_active_target(0, invalid, 2));
}

void test_ema_shadow_admission_is_transactional() {
    std::vector<torch_api::Tensor> parameters{torch_api::ones({2, 3}), torch_api::ones({4})};
    rfdetr::ModelEma ema(parameters, 0.99, 100.0);
    const auto original_first = ema.shadow_params().front().clone();
    const auto original_second = ema.shadow_params().back().clone();

    std::vector<torch_api::Tensor> malformed{torch_api::full({2, 3}, 7.0F), torch_api::zeros({5})};
    REQUIRE_THROWS(ema.stage_shadow_params(malformed));
    REQUIRE(torch_api::equal(ema.shadow_params().front(), original_first));
    REQUIRE(torch_api::equal(ema.shadow_params().back(), original_second));

    std::vector<torch_api::Tensor> valid{torch_api::full({2, 3}, 7.0F), torch_api::full({4}, 9.0F)};
    auto candidate = ema.stage_shadow_params(valid);
    ema.commit_shadow_params(std::move(candidate));
    REQUIRE(torch_api::equal(ema.shadow_params().front(), valid.front()));
    REQUIRE(torch_api::equal(ema.shadow_params().back(), valid.back()));
}

void test_native_optimizer_late_failure_preserves_live_state() {
    using AdamW = rfdetr::NativeAdamW;
    std::vector<AdamW::Group> groups{{rfdetr::NativeAdamWGroupConfig{0.01, 0.0, false}, {0, 1}}};
    std::vector<AdamW::NamedParameter> parameters{{"first.weight", torch_api::ones({2})}, {"second.weight", torch_api::ones({2})}};
    AdamW optimizer(std::move(groups), std::move(parameters), rfdetr::NativeOptimizerBackend::eager);
    const auto root = std::filesystem::temp_directory_path() / "mmltk_rfdetr_optimizer_atomicity";
    std::filesystem::create_directories(root);
    const auto write_archive = [&](const std::filesystem::path& path, const float first_value, const bool malformed_last) {
        torch_api::OutputArchive archive;
        rfdetr::write_string(archive, "format", "mmltk.rfdetr.native_adamw");
        rfdetr::write_int(archive, "format_version", 1);
        rfdetr::write_string(archive, "backend", "eager");
        rfdetr::write_double(archive, "beta1", 0.9);
        rfdetr::write_double(archive, "beta2", 0.999);
        rfdetr::write_double(archive, "eps", 1.0e-8);
        rfdetr::write_int(archive, "group_count", 1);
        rfdetr::write_int(archive, "param_count", 2);
        torch_api::OutputArchive group;
        rfdetr::write_double(group, "lr", 0.01);
        rfdetr::write_double(group, "weight_decay", 0.0);
        rfdetr::write_int(group, "amsgrad", 0);
        rfdetr::write_int(group, "param_index_count", 2);
        rfdetr::write_int(group, "param_index_000000", 0);
        rfdetr::write_int(group, "param_index_000001", 1);
        archive.write("group_000000", group);
        for (std::size_t index = 0; index < 2; ++index) {
            torch_api::OutputArchive parameter;
            rfdetr::write_string(parameter, "name", index == 0 ? "first.weight" : "second.weight");
            parameter.write("step", torch_api::tensor(2.0F));
            const auto shape = malformed_last && index == 1 ? std::vector<int64_t>{3} : std::vector<int64_t>{2};
            parameter.write("exp_avg", torch_api::full(shape, index == 0 ? first_value : 4.0F));
            parameter.write("exp_avg_sq", torch_api::ones(shape));
            rfdetr::write_int(parameter, "has_max_exp_avg_sq", 0);
            archive.write(rfdetr::archive_entry_name("param", index), parameter);
        }
        archive.save_to(path.string());
    };
    const auto valid_path = root / "valid.pt";
    write_archive(valid_path, 3.0F, false);
    torch_api::InputArchive valid;
    valid.load_from(valid_path.string());
    optimizer.load(valid);

    const auto malformed_path = root / "malformed.pt";
    write_archive(malformed_path, 7.0F, true);
    torch_api::InputArchive malformed;
    malformed.load_from(malformed_path.string());
    REQUIRE_THROWS(optimizer.load(malformed));

    const auto retained_path = root / "retained.pt";
    torch_api::OutputArchive retained;
    optimizer.save(retained);
    retained.save_to(retained_path.string());
    torch_api::InputArchive retained_input;
    retained_input.load_from(retained_path.string());
    torch_api::InputArchive first_parameter;
    retained_input.read("param_000000", first_parameter);
    const auto retained_average = rfdetr::require_tensor(first_parameter, "exp_avg");
    REQUIRE(torch_api::equal(retained_average, torch_api::full({2}, 3.0F)));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_resume_continuation_manifest_is_exact() {
    rfdetr::validate_resume_continuation_manifest({false, false, std::nullopt, std::nullopt});
    rfdetr::validate_resume_continuation_manifest({true, true, 1024.0, 17});
    REQUIRE_THROWS(rfdetr::validate_resume_continuation_manifest({true, false, 1024.0, 17}));
    REQUIRE_THROWS(rfdetr::validate_resume_continuation_manifest({false, true, 1024.0, 17}));
    REQUIRE_THROWS(rfdetr::validate_resume_continuation_manifest({false, false, 1024.0, std::nullopt}));
    REQUIRE_THROWS(rfdetr::validate_resume_continuation_manifest({false, false, std::nullopt, 17}));
    REQUIRE_THROWS(rfdetr::validate_resume_continuation_manifest({false, false, 0.0, 17}));
    REQUIRE_THROWS(rfdetr::validate_resume_continuation_manifest({false, false, std::numeric_limits<double>::infinity(), 17}));
    REQUIRE_THROWS(rfdetr::validate_resume_continuation_manifest({false, false, 1024.0, -1}));

    rfdetr::GradScaler scaler(true, 128.0F);
    const auto original_scale = scaler.current_scale();
    const auto original_growth = scaler.growth_tracker();
    REQUIRE_THROWS(rfdetr::validate_resume_continuation_manifest({true, false, 2048.0, 9}));
    REQUIRE(scaler.current_scale() == original_scale);
    REQUIRE(scaler.growth_tracker() == original_growth);
}

class TargetConsumerGate final {
   public:
    TargetConsumerGate() = default;
    ~TargetConsumerGate() { release(); }
    TargetConsumerGate(const TargetConsumerGate&) = delete;
    TargetConsumerGate& operator=(const TargetConsumerGate&) = delete;

    static void CUDART_CB wait(void* owner) { static_cast<TargetConsumerGate*>(owner)->gate_.acquire(); }

    void release() noexcept {
        if (!released_) {
            gate_.release();
            released_ = true;
        }
    }

   private:
    std::binary_semaphore gate_{0};
    bool released_ = false;
};

void test_target_scratch_reuse_waits_for_consumer_retirement() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
    constexpr int device_id = 0;
    rfdetr::TargetScratch scratch;
    scratch.ensure_batch(2, 8, 8, device_id);
    scratch.ensure_instance_capacity(3);
    scratch.ensure_copy_resources(device_id);

    const c10::DeviceIndex device_index = device_id;
    c10::cuda::CUDAGuard device_guard(device_index);
    const auto consumer = c10::cuda::getStreamFromPool(false, device_index);
    const auto releaser = c10::cuda::getStreamFromPool(false, device_index);
    REQUIRE(releaser.stream() != consumer.stream());
    const auto producer = c10::cuda::getStreamFromExternal(reinterpret_cast<cudaStream_t>(scratch.copy_stream_handle()), device_index);
    auto observed = torch_api::empty({2}, torch_api::TensorOptions().dtype(torch_api::kInt64).device(torch_api::kCUDA));
    auto gate_word = torch_api::zeros({1}, torch_api::TensorOptions().dtype(torch_api::kInt32).device(torch_api::kCUDA));
    REQUIRE(cudaDeviceSynchronize() == cudaSuccess);
    cudaEvent_t replacement_done = nullptr;
    REQUIRE(cudaEventCreateWithFlags(&replacement_done, cudaEventDisableTiming) == cudaSuccess);
    {
        c10::cuda::CUDAStreamGuard producer_guard(producer);
        scratch.offsets_gpu.narrow(0, 0, 2).copy_(
            torch_api::tensor({3, 7}, torch_api::TensorOptions().dtype(torch_api::kInt64).device(torch_api::kCUDA)));
        scratch.record_pending_copy_on_stream(scratch.copy_stream_handle());
    }

    rfdetr::PreparedTargets published;
    {
        c10::cuda::CUDAStreamGuard producer_guard(producer);
        const auto floating = torch_api::TensorOptions().dtype(torch_api::kFloat32).device(torch_api::kCUDA);
        const auto integer = torch_api::TensorOptions().dtype(torch_api::kInt64).device(torch_api::kCUDA);
        published.all_image_ids = torch_api::tensor({13, 17}, integer);
        published.orig_sizes = torch_api::full({2, 2}, 19, integer);
        published.nested_mask = torch_api::zeros({2, 8, 8}, torch_api::TensorOptions().dtype(torch_api::kBool).device(torch_api::kCUDA));
        published.all_boxes = torch_api::full({3, 4}, 2.0F, floating);
        published.all_labels = torch_api::full({3}, 3, integer);
        published.all_area = torch_api::full({3}, 5.0F, floating);
        published.all_iscrowd = torch_api::full({3}, 7, integer);
        published.target_offsets = scratch.offsets_gpu.narrow(0, 0, 2);
        published.target_counts = torch_api::tensor({1, 2}, integer);
        published.target_indices = torch_api::tensor({0, 1, 2}, integer);
        published.packed_masks = rfdetr::PackedTargetMasks{
            torch_api::full({3, 1}, 11, integer),    8, 8, torch_api::full({3, 6}, 13.0F, floating), torch_api::full({3}, 17, integer),
            torch_api::full({3, 6}, 19.0F, floating)};
        scratch.record_pending_copy_on_stream(scratch.copy_stream_handle());
    }
    {
        c10::cuda::CUDAStreamGuard consumer_guard(consumer);
        rfdetr::TargetConsumerLease lease(scratch, published, device_id);
        REQUIRE(cuStreamWaitValue32(reinterpret_cast<CUstream>(consumer.stream()), reinterpret_cast<CUdeviceptr>(gate_word.data_ptr()), 1U,
                                    CU_STREAM_WAIT_VALUE_EQ) == CUDA_SUCCESS);
        lease.handoff();
        lease.handoff();
        observed.copy_(scratch.offsets_gpu.narrow(0, 0, 2));
        lease.retire();
    }
    published = {};

    scratch.ensure_batch(2, 8, 8, device_id);
    scratch.ensure_copy_resources(device_id);
    {
        c10::cuda::CUDAStreamGuard producer_guard(producer);
        scratch.offsets_gpu.narrow(0, 0, 2).fill_(11);
        REQUIRE(cudaEventRecord(replacement_done, producer.stream()) == cudaSuccess);
    }
    REQUIRE(cudaEventQuery(replacement_done) == cudaErrorNotReady);
    REQUIRE(cuStreamWriteValue32(reinterpret_cast<CUstream>(releaser.stream()), reinterpret_cast<CUdeviceptr>(gate_word.data_ptr()), 1U,
                                 CU_STREAM_WRITE_VALUE_DEFAULT) == CUDA_SUCCESS);
    REQUIRE(cudaEventSynchronize(replacement_done) == cudaSuccess);
    REQUIRE(torch_api::equal(observed.cpu(), torch_api::tensor({3, 7}, torch_api::TensorOptions().dtype(torch_api::kInt64))));
    REQUIRE(cudaEventDestroy(replacement_done) == cudaSuccess);

    rfdetr::PreparedTargets exception_target;
    exception_target.all_boxes = torch_api::ones({1, 4}, torch_api::TensorOptions().dtype(torch_api::kFloat32).device(torch_api::kCUDA));
    try {
        rfdetr::TargetConsumerLease lease(scratch, exception_target, device_id);
        lease.handoff();
        exception_target.all_boxes.add_(1.0F);
        throw std::runtime_error("exercise target consumer exception retirement");
    } catch (const std::runtime_error&) {}
    REQUIRE(torch_api::equal(exception_target.all_boxes.cpu(), torch_api::full({1, 4}, 2.0F)));
}

void test_target_staging_ring_recycles_completed_slots_without_host_wait() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
    constexpr int device_id = 0;
    rfdetr::TargetScratch scratch(2);
    scratch.ensure_batch(1, 8, 8, device_id);
    scratch.ensure_copy_resources(device_id);
    const auto configured_copy_stream = scratch.copy_stream_handle();
    scratch.ensure_copy_resources(device_id);
    REQUIRE(scratch.copy_stream_handle() == configured_copy_stream);
    TargetConsumerGate gate;
    auto* copy_stream = reinterpret_cast<cudaStream_t>(scratch.copy_stream_handle());
    REQUIRE(cudaLaunchHostFunc(copy_stream, TargetConsumerGate::wait, &gate) == cudaSuccess);

    auto& first = scratch.acquire_staging_slot(1, 1, true, 8, 8);
    REQUIRE(first.batch_capacity == 1);
    REQUIRE(first.instance_capacity == 1);
    unsigned int registration_flags = 0U;
    REQUIRE(cuMemHostGetFlags(&registration_flags, first.boxes.data_ptr()) == CUDA_SUCCESS);
    REQUIRE(cuMemHostGetFlags(&registration_flags, first.labels.data_ptr()) == CUDA_SUCCESS);
    REQUIRE(cuMemHostGetFlags(&registration_flags, first.packed_masks.data_ptr()) == CUDA_SUCCESS);
    const auto* first_boxes = first.boxes.data_ptr();
    scratch.record_staging_copy_on_stream(scratch.copy_stream_handle());
    auto& second = scratch.acquire_staging_slot(2, 3, true, 16, 16);
    REQUIRE(second.batch_capacity == 2);
    REQUIRE(second.instance_capacity == 3);
    REQUIRE(second.mask_height == 16);
    scratch.record_staging_copy_on_stream(scratch.copy_stream_handle());
    REQUIRE_THROWS(scratch.acquire_staging_slot(1, 1, false, 8, 8));

    gate.release();
    REQUIRE(cudaStreamSynchronize(copy_stream) == cudaSuccess);
    auto& recycled = scratch.acquire_staging_slot(1, 1, false, 8, 8);
    REQUIRE(recycled.batch_capacity >= 1);
    REQUIRE(recycled.boxes.data_ptr() == first_boxes);
    scratch.record_staging_copy_on_stream(scratch.copy_stream_handle());
}

void test_target_scratch_retires_cross_device_events_on_their_owner() {
    if (mmltk::testsupport::checked_cuda_device_count() < 2) { SKIP("Two CUDA devices required; peer coverage remains unverified"); }
    c10::cuda::CUDAGuard ambient_device(static_cast<c10::DeviceIndex>(0));
    auto scratch = std::make_unique<rfdetr::TargetScratch>(1);
    scratch->ensure_batch(1, 8, 8, 0);
    scratch->ensure_instance_capacity(1);
    scratch->ensure_copy_resources(0);
    static_cast<void>(scratch->acquire_staging_slot(1, 1, false, 8, 8));
    scratch->record_staging_copy_on_stream(scratch->copy_stream_handle());
    scratch->record_pending_copy_on_stream(scratch->copy_stream_handle());

    scratch->ensure_batch(1, 8, 8, 1);
    scratch->ensure_instance_capacity(1);
    scratch->ensure_copy_resources(1);
    REQUIRE(scratch->copy_stream_handle() != 0U);
    static_cast<void>(scratch->acquire_staging_slot(1, 1, false, 8, 8));
    scratch->record_staging_copy_on_stream(scratch->copy_stream_handle());
    scratch->record_pending_copy_on_stream(scratch->copy_stream_handle());
    scratch->retire_consumer_on_stream(scratch->copy_stream_handle());

    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    scratch.reset();
    int ambient_after_destruction = -1;
    REQUIRE(cudaGetDevice(&ambient_after_destruction) == cudaSuccess);
    REQUIRE(ambient_after_destruction == 0);
}

void test_build_targets_recovers_after_staging_growth_and_failure() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
    constexpr int device_id = 0;
    using mmltk::backend::data::Batch;
    using mmltk::backend::data::LabelIndexEntry;
    using mmltk::backend::data::PackedInstance;
    using mmltk::backend::data::RLEPair;

    const std::array indices{std::uint32_t{0}, std::uint32_t{1}};
    const std::array label_index{
        LabelIndexEntry{0, 1, 0},
        LabelIndexEntry{1, 2, 0},
    };
    std::array labels{
        PackedInstance{0, 0, 1, 1, 5, 5, 0, 1},
        PackedInstance{1, 0, 2, 2, 6, 6, static_cast<std::uint32_t>(sizeof(RLEPair)), 1},
        PackedInstance{0, 0, 3, 3, 7, 7, static_cast<std::uint32_t>(2 * sizeof(RLEPair)), 1},
    };
    const std::array rle_pairs{
        RLEPair{0, 16},
        RLEPair{8, 12},
        RLEPair{16, 8},
    };
    const auto batch = [&](const std::size_t images) {
        return Batch{
            images, nullptr, label_index.data(), labels.data(), rle_pairs.data(), indices.data(), 0, 0,
        };
    };
    auto supervision = rfdetr::TrainingSupervisionConfig{};
    supervision.assignment = rfdetr::TrainAssignmentKind::MatchFree;
    rfdetr::TargetScratch scratch(2);
    scratch.ensure_batch(1, 8, 8, device_id);
    scratch.ensure_copy_resources(device_id);

    auto first = rfdetr::build_targets(batch(1), 8, 8, true, true, device_id, scratch, "train", 8, supervision, 2);
    auto grown = rfdetr::build_targets(batch(2), 8, 8, true, true, device_id, scratch, "train", 8, supervision, 2);

    REQUIRE(first.packed_masks.has_value());
    REQUIRE(grown.packed_masks.has_value());
    REQUIRE(grown.all_boxes.size(0) == 3);
    REQUIRE(grown.packed_masks->bits.size(0) == 3);

    scratch.wait_for_pending_copy();
    labels.front().class_id = 2;
    REQUIRE_THROWS_WITH(rfdetr::build_targets(batch(1), 8, 8, true, true, device_id, scratch, "train", 8, supervision, 2),
                        "feature-active RF-DETR target label is outside the object-class catalog");
    labels.front().class_id = 0;
    auto recovered = rfdetr::build_targets(batch(1), 8, 8, true, true, device_id, scratch, "train", 8, supervision, 2);
    {
        c10::cuda::CUDAStreamGuard consumer_guard(c10::cuda::getStreamFromPool(false, device_id));
        rfdetr::TargetConsumerLease lease(scratch, recovered, device_id);
        lease.handoff();
        REQUIRE(recovered.all_boxes.size(0) == 1);
    }
}

void test_copy_paste_cache_publication_recovers_without_targets() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
    c10::cuda::CUDAStreamGuard stream_guard(training_test_stream());
    using mmltk::backend::data::PackedInstance;
    using mmltk::backend::data::RLEPair;
    const auto config = rfdetr::test_support::isolated_augmentation_config(1);
    rfdetr::GpuBatchAugmenter augmenter(config, 1, 8, 8, 0);
    const auto options = torch_api::TensorOptions().dtype(torch_api::kFloat32).device(torch_api::kCUDA);
    const auto donor_pixels = torch_api::full({1, 3, 8, 8}, .9F, options);
    const auto source_pixels = torch_api::full({1, 3, 8, 8}, .1F, options);
    constexpr PackedInstance instance{0, 0, 0, 0, 8, 8, 0, 1};
    constexpr RLEPair run{0, 64};
    const std::array<mmltk::backend::data::LabelIndexEntry, 2> entries{{{0, 1, 0}, {0, 0, 0}}};
    const std::array<std::uint32_t, 1> donor_index{0}, source_index{1};
    const mmltk::backend::data::Batch donor{.num_images = 1,
                                            .device_images = donor_pixels.data_ptr<float>(),
                                            .label_index = entries.data(),
                                            .labels = &instance,
                                            .rle_pairs = &run,
                                            .image_indices = donor_index.data(),
                                            .slot_index = 0,
                                            .lease_id = 0};
    auto source = donor;
    source.device_images = source_pixels.data_ptr<float>();
    source.image_indices = source_index.data();
    REQUIRE_THROWS(augmenter.prepare_batch_consumer());
    REQUIRE_THROWS(augmenter.finish_batch(donor));
    // Cache input is the selected original instance. No target tensor, target
    // scratch, or target consumer is needed to publish physical source support.
    const auto select_original = [&] {
        auto& plan = augmenter.batch_plan().images.front();
        plan.cache_source_ordinal = 0;
        plan.cache_source_label = 0;
        plan.cache_source_dataset_index = 0;
        plan.cache_source_area = 64;
        plan.cache_source_box = {0, 0, 1, 1};
    };
    for (std::uint64_t cycle = 0; cycle < 2; ++cycle) {
        (void)augmenter.run(donor, 127, 0, 0, cycle * 4);
        select_original();
        REQUIRE_THROWS(augmenter.finish_batch(donor));
        REQUIRE(augmenter.prepare_batch_consumer() != nullptr);
        REQUIRE(augmenter.finish_batch(donor) != nullptr);
        REQUIRE_THROWS(augmenter.finish_batch(donor));
        const auto pasted = augmenter.run(source, 127, 0, 0, cycle * 4 + 1);
        REQUIRE(augmenter.batch_plan().images[0].paste_donor_slot == 0);
        REQUIRE(augmenter.batch_plan().images[0].paste_masked);
        CHECK(pasted.gt(0).any().item<bool>());
        (void)augmenter.prepare_batch_consumer();
        (void)augmenter.finish_batch(source);
        (void)augmenter.run(donor, 127, 0, 0, cycle * 4 + 2);
        select_original();
        (void)augmenter.prepare_batch_consumer();
        auto unavailable_support = donor;
        unavailable_support.rle_pairs = nullptr;
        REQUIRE_THROWS_WITH(augmenter.finish_batch(unavailable_support), "donor cache source mask storage is missing");
        REQUIRE_NOTHROW(augmenter.reconfigure(config));
        const auto recovered = augmenter.run(source, 127, 0, 0, cycle * 4 + 3);
        CHECK(augmenter.batch_plan().images[0].paste_donor_slot == -1);
        CHECK_FALSE(recovered.gt(0).any().item<bool>());
        (void)augmenter.prepare_batch_consumer();
        (void)augmenter.finish_batch(source);
    }
}

void test_training_adapter_matches_raw_augmentation_executor() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
    constexpr int height = 8;
    constexpr int width = 8;
    constexpr std::uint64_t seed = 127U;
    constexpr int epoch = 3;
    constexpr int rank = 1;
    constexpr std::uint64_t sequence = 19U;
    constexpr std::uint32_t source_index = 5U;
    constexpr std::uint32_t donor_index = 11U;
    constexpr std::array<std::uint32_t, 1U> source_indices{source_index};
    constexpr std::array<std::uint32_t, 1U> donor_indices{donor_index};
    const int device_id = 0;
    REQUIRE(cudaSetDevice(device_id) == cudaSuccess);
    const c10::cuda::CUDAStream test_stream = c10::cuda::getStreamFromPool(false, device_id);
    c10::cuda::CUDAStreamGuard test_stream_guard(test_stream);
    const auto float_device = torch_api::TensorOptions().dtype(torch_api::kFloat32).device(torch_api::kCUDA);
    const auto int64_device = torch_api::TensorOptions().dtype(torch_api::kInt64).device(torch_api::kCUDA);
    auto source_pixels = torch_api::full({1, 3, height, width}, 0.1F, float_device);
    auto donor_pixels = torch_api::full({1, 3, height, width}, 0.9F, float_device);

    std::array<mmltk::backend::data::LabelIndexEntry, donor_index + 1U> label_index{};
    label_index[donor_index] = {0U, 1U, 0U};
    constexpr std::array donor_labels{
        mmltk::backend::data::PackedInstance{2U, 0U, 1, 1, 7, 7, 0U, 1U},
    };
    constexpr std::array donor_rle{mmltk::backend::data::RLEPair{0U, height * width}};
    const mmltk::backend::data::Batch donor_batch{
        .num_images = donor_indices.size(),
        .device_images = donor_pixels.data_ptr<float>(),
        .label_index = label_index.data(),
        .labels = donor_labels.data(),
        .rle_pairs = donor_rle.data(),
        .image_indices = donor_indices.data(),
        .slot_index = 0U,
        .lease_id = 0U,
    };
    const mmltk::backend::data::Batch source_batch{
        .num_images = source_indices.size(),
        .device_images = source_pixels.data_ptr<float>(),
        .label_index = label_index.data(),
        .labels = donor_labels.data(),
        .rle_pairs = donor_rle.data(),
        .image_indices = source_indices.data(),
        .slot_index = 0U,
        .lease_id = 0U,
    };
    auto config = rfdetr::test_support::isolated_augmentation_config(1.0F);

    for (const bool include_masks : {false, true}) {
        rfdetr::GpuBatchAugmenter adapter(config, 1, height, width, device_id);
        (void)adapter.run(donor_batch, seed, epoch, rank, sequence - 1U);
        rfdetr::TargetScratch scratch(1);
        auto supervision = rfdetr::TrainingSupervisionConfig{};
        supervision.assignment = rfdetr::TrainAssignmentKind::MatchFree;
        auto targets = rfdetr::build_targets(donor_batch, width, height, include_masks, include_masks, device_id, scratch, "train", 8,
                                             supervision, 3, &adapter.batch_plan());
        REQUIRE(adapter.prepare_batch_consumer() != nullptr);
        REQUIRE(adapter.finish_batch(donor_batch) != nullptr);

        {
            rfdetr::TargetConsumerLease lease(scratch, targets, device_id);
            lease.handoff();
            const auto identity_boxes = targets.all_boxes.cpu();
            const auto box = identity_boxes.accessor<float, 2>();
            CHECK(box[0][0] == 0.5F);
            CHECK(box[0][1] == 0.5F);
            CHECK(box[0][2] == 1.0F);
            CHECK(box[0][3] == 1.0F);
            CHECK(targets.all_area.cpu().item<float>() == static_cast<float>(height * width));
            std::vector<rfdetr::AugmentationPreviewAnnotation> identity_preview;
            rfdetr::build_augmentation_preview_annotations(donor_labels, nullptr, nullptr, width, height, identity_preview, donor_rle);
            REQUIRE(identity_preview.size() == 1);
            CHECK(identity_preview[0].box_xyxy == std::array<float, 4>{0, 0, 1, 1});
            CHECK(identity_preview[0].visible_area_pixels == static_cast<float>(height * width));
        }
        const torch_api::Tensor adapted = adapter.run(source_batch, seed, epoch, rank, sequence);
        const rfdetr::AugmentationBatchPlan adapted_plan = adapter.batch_plan();
        REQUIRE(adapted_plan.images.front().paste_donor_slot == 0);

        rfdetr::GpuAugmentationExecutor raw(config, 1U, height, width, device_id);
        auto raw_output = torch_api::empty_like(source_pixels);
        std::array<std::uint64_t, source_indices.size()> keys{};
        for (std::size_t image = 0U; image < keys.size(); ++image) {
            keys[image] = rfdetr::training_augmentation_image_key(seed, epoch, rank, sequence, image);
        }
        const auto stream = c10::cuda::getCurrentCUDAStream(device_id).stream();
        const rfdetr::GpuAugmentationBatchView raw_batch{
            .input = source_pixels.data_ptr<float>(),
            .output = raw_output.data_ptr<float>(),
            .image_indices = source_indices,
            .height = height,
            .width = width,
        };
        const std::array raw_donors{
            rfdetr::GpuAugmentationDonor{
                .label = 2,
                .dataset_index = donor_index,
                .area = static_cast<float>(height * width),
                .box = {0.125F, 0.125F, 0.875F, 0.875F},
                .has_mask = true,
            },
        };
        auto donor_mask = torch_api::full({1, 1}, -1, int64_device);
        auto donor_box = torch_api::tensor({0.125F, 0.125F, 0.875F, 0.875F}, float_device).view({1, 4});
        const rfdetr::GpuAugmentationDonorBatchView raw_donor_batch{
            .images = donor_pixels.data_ptr<float>(),
            .masks = donor_mask.data_ptr<std::int64_t>(),
            .boxes = donor_box.data_ptr<float>(),
            .mask_words = 1,
            .selection = rfdetr::GpuAugmentationDonorSelection::Cached,
        };
        (void)raw.Run(raw_batch, keys, raw_donors, raw_donor_batch, stream);
        REQUIRE(torch_api::equal(adapted, raw_output));
        auto raw_image_plan = raw.plan().images.front();
        raw_image_plan.paste_support = adapted_plan.images.front().paste_support;
        raw_image_plan.paste_support_count = adapted_plan.images.front().paste_support_count;
        CAPTURE(include_masks, adapted_plan.images.front().paste_source_area, raw_image_plan.paste_source_area);
        CHECK(adapted_plan.images.front() == raw_image_plan);
        CHECK(raw.plan().images.front().paste_source_box == raw_donors.front().box);
        CHECK(raw.plan().images.front().paste_label == raw_donors.front().label);
        REQUIRE(adapted_plan.images.front().paste_support_count == donor_rle.size());
        CHECK(adapted_plan.images.front().paste_masked);
        std::vector<rfdetr::AugmentationPreviewAnnotation> preview;
        rfdetr::build_augmentation_preview_annotations({}, &donor_labels.front(), &adapted_plan.images.front(), width, height, preview);
        auto pasted_targets = rfdetr::build_targets(source_batch, width, height, include_masks, include_masks, device_id, scratch, "train",
                                                    8, supervision, 3, &adapter.batch_plan());
        REQUIRE(adapter.prepare_batch_consumer() != nullptr);
        REQUIRE(adapter.finish_batch(source_batch) != nullptr);
        rfdetr::TargetConsumerLease pasted_lease(scratch, pasted_targets, device_id);
        pasted_lease.handoff();
        REQUIRE(pasted_targets.all_boxes.size(0) == 1);
        REQUIRE(preview.size() == 1);
        if (!include_masks) {
            // Positive red values identify the bright donor in the normalized image.
            const auto image_cpu = adapted.cpu();
            const auto pixels = image_cpu.accessor<float, 4>();
            int xmin = width, ymin = height, xmax = -1, ymax = -1, count = 0;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    if (pixels[0][0][y][x] <= 0) continue;
                    ++count;
                    xmin = std::min(xmin, x);
                    ymin = std::min(ymin, y);
                    xmax = std::max(xmax, x);
                    ymax = std::max(ymax, y);
                }
            REQUIRE(count > 0);
            const std::array<float, 4> footprint{static_cast<float>(xmin) / width, static_cast<float>(ymin) / height,
                                                 static_cast<float>(xmax + 1) / width, static_cast<float>(ymax + 1) / height};
            CHECK(preview[0].box_xyxy == footprint);
            CHECK(preview[0].visible_area_pixels == static_cast<float>(count));
            const auto target_boxes = pasted_targets.all_boxes.cpu();
            const auto box = target_boxes.accessor<float, 2>();
            CHECK(box[0][0] == (footprint[0] + footprint[2]) / 2);
            CHECK(box[0][1] == (footprint[1] + footprint[3]) / 2);
            CHECK(box[0][2] == footprint[2] - footprint[0]);
            CHECK(box[0][3] == footprint[3] - footprint[1]);
            CHECK(pasted_targets.all_area.cpu().item<float>() == static_cast<float>(count));
        }
    }
}

void check_copy_paste_targets(const rfdetr::PreparedTargets& targets, const std::span<const std::uint64_t> support_by_class,
                              const std::vector<rfdetr::AugmentationPreviewAnnotation>& preview, const torch_api::Tensor& points) {
    const auto boxes = targets.all_boxes.cpu(), areas = targets.all_area.cpu(), ids = targets.all_labels.cpu();
    const auto box = boxes.accessor<float, 2>();
    const auto area = areas.accessor<float, 1>();
    const auto id = ids.accessor<std::int64_t, 1>();
    const auto expected_count = std::ranges::count_if(support_by_class, [](auto bits) { return bits != 0; });
    REQUIRE(boxes.size(0) == expected_count);
    REQUIRE(preview.size() == static_cast<std::size_t>(expected_count));
    torch_api::Tensor sampled;
    if (targets.packed_masks && expected_count != 0)
        sampled =
            rfdetr::sample_target_masks(*targets.packed_masks, torch_api::arange(expected_count, points.options().dtype(torch_api::kInt64)),
                                        points, "ring support")
                .cpu();
    for (std::int64_t i = 0; i < expected_count; ++i) {
        REQUIRE(id[i] >= 0);
        REQUIRE(static_cast<std::size_t>(id[i]) < support_by_class.size());
        const auto support = support_by_class[static_cast<std::size_t>(id[i])];
        int x0 = 8, y0 = 8, x1 = 0, y1 = 0, count = 0;
        for (int p = 0; p < 64; ++p) {
            const bool present = (support & (1ULL << p)) != 0;
            if (sampled.defined()) CHECK((sampled.accessor<float, 2>()[i][p] > .5F) == present);
            if (!present) continue;
            ++count;
            x0 = std::min(x0, p % 8);
            y0 = std::min(y0, p / 8);
            x1 = std::max(x1, p % 8 + 1);
            y1 = std::max(y1, p / 8 + 1);
        }
        REQUIRE(count != 0);
        CHECK(area[i] == static_cast<float>(count));
        CHECK(box[i][0] == static_cast<float>(x0 + x1) / 16.F);
        CHECK(box[i][1] == static_cast<float>(y0 + y1) / 16.F);
        CHECK(box[i][2] == static_cast<float>(x1 - x0) / 8.F);
        CHECK(box[i][3] == static_cast<float>(y1 - y0) / 8.F);
        const auto& annotation = preview[static_cast<std::size_t>(i)];
        CHECK(annotation.class_id == id[i]);
        CHECK(annotation.box_xyxy == std::array<float, 4>{static_cast<float>(x0) / 8.F, static_cast<float>(y0) / 8.F,
                                                          static_cast<float>(x1) / 8.F, static_cast<float>(y1) / 8.F});
        CHECK(annotation.visible_area_pixels == static_cast<float>(count));
    }
}

void test_copy_paste_ring_support_and_cache_cycles() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
    c10::cuda::CUDAStreamGuard stream_guard(training_test_stream());
    using namespace rfdetr::test_support;
    using mmltk::backend::data::PackedInstance;
    using mmltk::backend::data::RLEPair;
    const auto options = torch_api::TensorOptions().dtype(torch_api::kFloat32).device(torch_api::kCUDA);
    auto source_pixels = torch_api::full({1, 3, 8, 8}, .1F, options);
    auto donor_pixels = torch_api::full({1, 3, 8, 8}, .9F, options);
    std::vector<RLEPair> runs(dot_runs.begin(), dot_runs.end());
    runs.insert(runs.end(), ring_runs.begin(), ring_runs.end());
    std::array<PackedInstance, dot_runs.size() + 1> labels{};
    for (std::size_t i = 0; i < dot_runs.size(); ++i) {
        const auto x = static_cast<int>(dot_runs[i].start % 8);
        const auto y = static_cast<int>(dot_runs[i].start / 8);
        labels[i] = {static_cast<std::uint8_t>(i),
                     0,
                     static_cast<std::int16_t>(x),
                     static_cast<std::int16_t>(y),
                     static_cast<std::int16_t>(x + dot_runs[i].length),
                     static_cast<std::int16_t>(y + 1),
                     static_cast<std::uint32_t>(i * sizeof(RLEPair)),
                     1};
    }
    labels.back() = {7, 0, 1, 1, 7, 7, dot_runs.size() * sizeof(RLEPair), static_cast<std::uint16_t>(ring_runs.size())};
    const std::array<mmltk::backend::data::LabelIndexEntry, 3> entries{{{0, 7, 0}, {7, 1, 0}, {0, 0, 0}}};
    const std::array<std::uint32_t, 1> source_index{0}, donor_index{1}, empty_index{2};
    auto batch = mmltk::backend::data::Batch{.num_images = 1,
                                             .device_images = donor_pixels.data_ptr<float>(),
                                             .label_index = entries.data(),
                                             .labels = labels.data(),
                                             .rle_pairs = runs.data(),
                                             .image_indices = donor_index.data(),
                                             .slot_index = 0,
                                             .lease_id = 0};
    std::vector<float> centers;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            centers.push_back((static_cast<float>(x) + .5F) / 8);
            centers.push_back((static_cast<float>(y) + .5F) / 8);
        }
    const auto points = torch_api::tensor(centers, options).view({1, 64, 2});
    std::array<torch_api::Tensor, 3> detection_images;
    for (const bool include_masks : {false, true}) {
        CAPTURE(include_masks);
        auto config = isolated_augmentation_config(1);
        config.geometry = {1.F, .4F, .4F};
        rfdetr::GpuBatchAugmenter augmenter(config, 1, 8, 8, 0);
        rfdetr::TargetScratch scratch(1);
        const auto consume_cached_batch = [&] {
            auto targets = rfdetr::build_targets(batch, 8, 8, include_masks, include_masks, 0, scratch, "train", 16,
                                                 rfdetr::TrainingSupervisionConfig{}, 8, &augmenter.batch_plan());
            (void)augmenter.prepare_batch_consumer();
            (void)augmenter.finish_batch(batch);
            rfdetr::TargetConsumerLease lease(scratch, targets, 0);
            lease.handoff();
        };
        // Warm the cache from original donor pixels and RLE, with no prior donor.
        batch.device_images = donor_pixels.data_ptr<float>();
        batch.image_indices = donor_index.data();
        (void)augmenter.run(batch, 127, 0, 0, 0);
        CHECK(augmenter.batch_plan().images[0].paste_donor_slot == -1);
        consume_cached_batch();
        REQUIRE(augmenter.batch_plan().transforms_geometry);
        config = isolated_augmentation_config(1);
        augmenter.reconfigure(config);
        // Empty sources preserve the ring across cycles; the last image adds all explicit dots.
        for (std::uint64_t cycle = 1; cycle <= 3; ++cycle) {
            batch.device_images = source_pixels.data_ptr<float>();
            batch.image_indices = cycle == 3 ? source_index.data() : empty_index.data();
            const auto pixels = augmenter.run(batch, 127, 0, 0, cycle);
            const auto plan = augmenter.batch_plan().images[0];
            REQUIRE(plan.paste_donor_slot == 0);
            REQUIRE(plan.paste_masked);
            REQUIRE(plan.paste_support_count == ring_runs.size());
            std::uint64_t footprint = 0;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    if (ring_paste_contains(plan.paste_inverse, true, x, y)) footprint |= 1ULL << (y * 8 + x);
                }
            const auto observed = pixels.cpu();
            if (include_masks)
                REQUIRE(torch_api::equal(observed, detection_images[cycle - 1]));
            else
                detection_images[cycle - 1] = observed;
            const auto rgb = observed.accessor<float, 4>();
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const bool pasted = (footprint & (1ULL << (y * 8 + x))) != 0;
                    const std::array means{.485F, .456F, .406F}, deviations{.229F, .224F, .225F};
                    for (int c = 0; c < 3; ++c)
                        CHECK(std::abs(rgb[0][c][y][x] - ((pasted ? .9F : .1F) - means[c]) / deviations[c]) < 1.e-5F);
                }
            auto targets = rfdetr::build_targets(batch, 8, 8, include_masks, include_masks, 0, scratch, "train", 16,
                                                 rfdetr::TrainingSupervisionConfig{}, 8, &augmenter.batch_plan());
            std::array<std::uint64_t, 8> expected{};
            expected[7] = footprint;
            const auto sources = cycle == 3 ? std::span{labels.data(), dot_runs.size()} : std::span<PackedInstance>{};
            if (cycle == 3)
                for (std::size_t i = 0; i < dot_runs.size(); ++i)
                    expected[i] = (((1ULL << dot_runs[i].length) - 1) << dot_runs[i].start) & ~footprint;
            std::vector<rfdetr::AugmentationPreviewAnnotation> preview;
            rfdetr::build_augmentation_preview_annotations(sources, &labels.back(), &plan, 8, 8, preview, runs);
            // Targets are complete before cache publication as well as after it.
            {
                rfdetr::TargetConsumerLease lease(scratch, targets, 0);
                lease.handoff();
                check_copy_paste_targets(targets, expected, preview, points);
            }
            (void)augmenter.prepare_batch_consumer();
            (void)augmenter.finish_batch(batch);
            rfdetr::TargetConsumerLease lease(scratch, targets, 0);
            lease.handoff();
            CHECK(targets.packed_masks.has_value() == include_masks);
            check_copy_paste_targets(targets, expected, preview, points);
        }
        // A disabled interval preserves the donor cache for a subsequent enable.
        augmenter.reconfigure(isolated_augmentation_config(0));
        batch.image_indices = empty_index.data();
        (void)augmenter.run(batch, 127, 0, 0, 4);
        CHECK(augmenter.batch_plan().images[0].paste_donor_slot == -1);
        consume_cached_batch();
        augmenter.reconfigure(config);
        (void)augmenter.run(batch, 127, 0, 0, 5);
        CHECK(augmenter.batch_plan().images[0].paste_masked);
        consume_cached_batch();
        if (!include_masks) {
            // Replacing a masked donor with a genuinely box-only record clears
            // mask availability and keeps the original rectangular paste mode.
            labels.back().mask_rle_pairs = 0;
            batch.image_indices = donor_index.data();
            batch.device_images = donor_pixels.data_ptr<float>();
            (void)augmenter.run(batch, 127, 0, 0, 6);
            auto replacement = rfdetr::build_targets(batch, 8, 8, false, false, 0, scratch, "train", 16,
                                                     rfdetr::TrainingSupervisionConfig{}, 8, &augmenter.batch_plan());
            (void)augmenter.prepare_batch_consumer();
            (void)augmenter.finish_batch(batch);
            {
                rfdetr::TargetConsumerLease lease(scratch, replacement, 0);
                lease.handoff();
            }
            batch.image_indices = empty_index.data();
            batch.device_images = source_pixels.data_ptr<float>();
            const auto rectangle = augmenter.run(batch, 127, 0, 0, 7);
            const auto box_plan = augmenter.batch_plan().images[0];
            REQUIRE(box_plan.paste_donor_slot == 0);
            CHECK_FALSE(box_plan.paste_masked);
            CHECK(box_plan.paste_support_count == 0);
            std::array<std::uint64_t, 8> expected{};
            const auto pixels_cpu = rectangle.cpu();
            const auto rgb = pixels_cpu.accessor<float, 4>();
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const bool present = ring_paste_contains(box_plan.paste_inverse, false, x, y);
                    if (present) expected[7] |= 1ULL << (y * 8 + x);
                    CHECK((rgb[0][0][y][x] > 0) == present);
                }
            auto targets = rfdetr::build_targets(batch, 8, 8, false, false, 0, scratch, "train", 16, rfdetr::TrainingSupervisionConfig{}, 8,
                                                 &augmenter.batch_plan());
            std::vector<rfdetr::AugmentationPreviewAnnotation> preview;
            rfdetr::build_augmentation_preview_annotations({}, &labels.back(), &box_plan, 8, 8, preview);
            (void)augmenter.prepare_batch_consumer();
            (void)augmenter.finish_batch(batch);
            rfdetr::TargetConsumerLease lease(scratch, targets, 0);
            lease.handoff();
            CHECK_FALSE(targets.packed_masks.has_value());
            check_copy_paste_targets(targets, expected, preview, points);
            labels.back().mask_rle_pairs = static_cast<std::uint16_t>(ring_runs.size());
        }
        // Exact identity placement isolates all prescribed partial-visibility boundaries.
        rfdetr::AugmentationBatchPlan plan;
        plan.active_size = 1;
        plan.copy_paste_enabled = true;
        plan.images.resize(1);
        auto& image = plan.images[0];
        image.paste_donor_slot = 0;
        image.paste_label = 7;
        image.paste_masked = true;
        image.paste_source_box = {.125F, .125F, .875F, .875F};
        image.paste_output_box = image.paste_source_box;
        image.paste_support = ring_runs.data();
        image.paste_support_count = ring_runs.size();
        batch.image_indices = source_index.data();
        std::vector<rfdetr::AugmentationPreviewAnnotation> preview;
        rfdetr::build_augmentation_preview_annotations(std::span{labels.data(), dot_runs.size()}, &labels.back(), &image, 8, 8, preview,
                                                       runs);
        auto targets = rfdetr::build_targets(batch, 8, 8, include_masks, include_masks, 0, scratch, "train", 16,
                                             rfdetr::TrainingSupervisionConfig{}, 8, &plan);
        rfdetr::TargetConsumerLease lease(scratch, targets, 0);
        lease.handoff();
        std::array<std::uint64_t, 8> expected{};
        std::ranges::copy(identity_survivors, expected.begin());
        for (int p = 0; p < 64; ++p)
            if (fixture_contains(ring_runs, p % 8, p / 8)) expected[7] |= 1ULL << p;
        check_copy_paste_targets(targets, expected, preview, points);
        CHECK(targets.packed_masks.has_value() == include_masks);
        lease.retire();

        // The first source lies in the ring's hole and survives its pasted donor:
        // augmentation grows one admitted target into two with one ordinary query.
        const std::array<mmltk::backend::data::LabelIndexEntry, 1> one_source{{{0, 1, 0}}};
        batch.label_index = one_source.data();
        auto grown = rfdetr::build_targets(batch, 8, 8, include_masks, include_masks, 0, scratch, "train", 1,
                                           rfdetr::TrainingSupervisionConfig{}, 8, &plan);
        rfdetr::TargetConsumerLease grown_lease(scratch, grown, 0);
        grown_lease.handoff();
        REQUIRE(grown.counts == std::vector<int64_t>{2});
        REQUIRE(torch_api::equal(grown.all_labels.cpu(), torch_api::tensor({0, 7}, torch_api::TensorOptions().dtype(torch_api::kInt64))));
        batch.label_index = entries.data();
    }
}

void test_native_augmentation_preview_target_support_parity() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
    c10::cuda::CUDAStreamGuard stream_guard(training_test_stream());
    using mmltk::backend::data::PackedInstance;
    using mmltk::backend::data::RLEPair;
    const std::array runs{RLEPair{9, 3}, RLEPair{17, 3}, RLEPair{25, 3}, RLEPair{10, 2}, RLEPair{18, 2}, RLEPair{26, 2}};
    const PackedInstance source{2, 0, 0, 0, 5, 5, 0, 3};
    const PackedInstance donor{4, 0, 0, 0, 5, 5, 3 * sizeof(RLEPair), 3};
    const std::array<mmltk::backend::data::LabelIndexEntry, 1> entries{{{0, 1, 0}}};
    const std::array<std::uint32_t, 1> indices{0};
    const mmltk::backend::data::Batch batch{.num_images = 1,
                                            .device_images = nullptr,
                                            .label_index = entries.data(),
                                            .labels = &source,
                                            .rle_pairs = runs.data(),
                                            .image_indices = indices.data(),
                                            .slot_index = 0,
                                            .lease_id = 0};
    for (const bool include_masks : {false, true})
        for (const bool erase_all : {false, true}) {
            rfdetr::AugmentationBatchPlan plan;
            plan.active_size = 1;
            plan.copy_paste_enabled = true;
            plan.transforms_geometry = true;
            plan.erases_spatial_support = erase_all;
            plan.images.resize(1);
            auto& image = plan.images.front();
            image.paste_donor_slot = 0;
            image.paste_label = donor.class_id;
            image.paste_source_box = {0, 0, 0.625F, 0.625F};
            image.paste_output_box = image.paste_source_box;
            image.paste_support = runs.data() + 3;
            image.paste_support_count = 3;
            image.paste_masked = include_masks;
            image.erasure.dropout_probability = erase_all ? 1 : 0;
            std::vector<rfdetr::AugmentationPreviewAnnotation> preview;
            rfdetr::build_augmentation_preview_annotations(std::span{&source, 1U}, &donor, &image, 8, 8, preview, runs);
            rfdetr::TargetScratch scratch(1);
            auto targets = rfdetr::build_targets(batch, 8, 8, include_masks, include_masks, 0, scratch, "train", 8,
                                                 rfdetr::TrainingSupervisionConfig{}, 5, &plan);
            rfdetr::TargetConsumerLease lease(scratch, targets, 0);
            lease.handoff();
            REQUIRE(targets.all_boxes.size(0) == static_cast<std::int64_t>(preview.size()));
            const auto boxes = targets.all_boxes.cpu();
            const auto box = boxes.accessor<float, 2>();
            const auto areas = targets.all_area.cpu();
            const auto area = areas.accessor<float, 1>();
            const auto labels = targets.all_labels.cpu();
            const auto label = labels.accessor<std::int64_t, 1>();
            for (std::size_t i = 0; i < preview.size(); ++i) {
                const auto& expected = preview[i].box_xyxy;
                CHECK(box[i][0] == (expected[0] + expected[2]) / 2);
                CHECK(box[i][1] == (expected[1] + expected[3]) / 2);
                CHECK(box[i][2] == expected[2] - expected[0]);
                CHECK(box[i][3] == expected[3] - expected[1]);
                CHECK(area[i] == preview[i].visible_area_pixels);
                CHECK(label[i] == preview[i].class_id);
            }
            // CLEANUP-IGNORE: CPD crosses an independent preview oracle assertion into the next CUDA fixture; its stream guard must remain
            // test-scoped.
            CHECK(preview.empty() == erase_all);
        }
}

void test_tiny_mask_training_outer_edges() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
    c10::cuda::CUDAStreamGuard stream_guard(training_test_stream());
    using mmltk::backend::data::PackedInstance;
    using mmltk::backend::data::RLEPair;
    const std::array<mmltk::backend::data::LabelIndexEntry, 1> entries{{{0, 1, 0}}};
    const std::array<std::uint32_t, 1> indices{0};
    for (const auto run : {RLEPair{0, 1}, RLEPair{7, 1}, RLEPair{24, 1}, RLEPair{31, 1}, RLEPair{11, 1}, RLEPair{11, 2}, RLEPair{10, 4}}) {
        const PackedInstance source{0, 0, 0, 0, 8, 4, 0, 1};
        const mmltk::backend::data::Batch batch{.num_images = 1,
                                                .device_images = nullptr,
                                                .label_index = entries.data(),
                                                .labels = &source,
                                                .rle_pairs = &run,
                                                .image_indices = indices.data(),
                                                .slot_index = 0,
                                                .lease_id = 0};
        for (int geometry = 0; geometry < 4; ++geometry)
            for (const bool masks : {false, true}) {
                CAPTURE(run.start, run.length, geometry, masks);
                rfdetr::AugmentationBatchPlan plan;
                plan.active_size = 1;
                plan.transforms_geometry = geometry != 0;
                plan.erases_spatial_support = geometry == 3;
                plan.images.resize(1);
                plan.images.front() = rfdetr::test_support::small_object_plan(geometry);
                const int x = int(run.start % 8);
                const int y = int(run.start / 8);
                const auto edges = rfdetr::test_support::small_object_edges({x, y, x + int(run.length), y + 1}, geometry);
                const bool present = edges[0] < edges[2] && edges[1] < edges[3];
                const std::array<float, 4> expected{float(edges[0]) / 8, float(edges[1]) / 4, float(edges[2]) / 8, float(edges[3]) / 4};
                rfdetr::TargetScratch scratch(1);
                auto targets =
                    rfdetr::build_targets(batch, 4, 8, masks, masks, 0, scratch, "train", 8, rfdetr::TrainingSupervisionConfig{}, 1, &plan);
                rfdetr::TargetConsumerLease lease(scratch, targets, 0);
                lease.handoff();
                REQUIRE(targets.all_boxes.size(0) == (present ? 1 : 0));
                if (!present) continue;
                const auto boxes = targets.all_boxes.cpu();
                const auto box = boxes.accessor<float, 2>();
                for (int axis = 0; axis < 2; ++axis) {
                    CHECK(box[0][axis] - box[0][axis + 2] / 2 == expected[axis]);
                    CHECK(box[0][axis] + box[0][axis + 2] / 2 == expected[axis + 2]);
                }
            }
    }
}

void test_training_mask_targets_follow_spatial_image_erasure() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
    constexpr std::size_t count = 64U;
    constexpr int extent = 8;
    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    const c10::cuda::CUDAStream test_stream = c10::cuda::getStreamFromPool(false, 0);
    c10::cuda::CUDAStreamGuard test_stream_guard(test_stream);
    const auto floats = torch_api::TensorOptions().dtype(torch_api::kFloat32).device(torch_api::kCUDA);
    auto pixels = torch_api::full({static_cast<int64_t>(count), 3, extent, extent}, 0.9F, floats);
    std::array<std::uint32_t, count> indices{};
    std::array<mmltk::backend::data::LabelIndexEntry, count> label_index{};
    for (std::size_t image = 0U; image < count; ++image) {
        indices[image] = static_cast<std::uint32_t>(image);
        label_index[image] = {0U, 1U, 0U};
    }
    constexpr std::array labels{mmltk::backend::data::PackedInstance{0U, 0U, 0, 0, extent, extent, 0U, 1U}};
    constexpr std::array rle{mmltk::backend::data::RLEPair{0U, extent * extent}};
    const mmltk::backend::data::Batch batch{.num_images = count,
                                            .device_images = pixels.data_ptr<float>(),
                                            .label_index = label_index.data(),
                                            .labels = labels.data(),
                                            .rle_pairs = rle.data(),
                                            .image_indices = indices.data(),
                                            .slot_index = 0U,
                                            .lease_id = 0U};
    rfdetr::GpuBatchAugmenter augmenter(rfdetr::test_support::spatial_occlusion_config(), count, extent, extent, 0);
    const auto image = augmenter.run(batch, 53U, 0, 0, 0U);
    rfdetr::TargetScratch scratch(count);
    auto supervision = rfdetr::TrainingSupervisionConfig{};
    supervision.assignment = rfdetr::TrainAssignmentKind::MatchFree;
    auto targets =
        rfdetr::build_targets(batch, extent, extent, true, true, 0, scratch, "train", 8, supervision, 1, &augmenter.batch_plan());
    REQUIRE(augmenter.prepare_batch_consumer() != nullptr);
    REQUIRE(augmenter.finish_batch(batch) != nullptr);
    rfdetr::TargetConsumerLease lease(scratch, targets, 0);
    lease.handoff();
    REQUIRE(targets.packed_masks.has_value());
    REQUIRE(targets.packed_masks->erasure.defined());
    std::vector<float> centers;
    for (int y = 0; y < extent; ++y) {
        for (int x = 0; x < extent; ++x) {
            centers.push_back((static_cast<float>(x) + 0.5F) / extent);
            centers.push_back((static_cast<float>(y) + 0.5F) / extent);
        }
    }
    const auto points = torch_api::tensor(centers, floats).view({1, extent * extent, 2});
    const auto sampled = rfdetr::sample_target_masks(
        *targets.packed_masks, torch_api::arange(static_cast<int64_t>(count), floats.dtype(torch_api::kInt64)), points, "training erasure");
    const auto visible = image.ne(0.0F).any(1).reshape({static_cast<int64_t>(count), extent * extent});
    REQUIRE(torch_api::equal(sampled.to(torch_api::kBool), visible));
    CHECK(visible.any().item<bool>());
    CHECK(visible.logical_not().any().item<bool>());
}

void test_parallel_wave_drains_failures_and_cancellation() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
    const rfdetr::DistributedContext distributed;
    const auto device = rfdetr::cuda_device(0);

    std::atomic<int> prepublication_drained = 0;
    {
        rfdetr::ParallelTrainingWave<int> wave(2, true, 0, distributed);
        const auto normalizer = wave.normalizer();
        wave.add(std::async(std::launch::async, [normalizer, &prepublication_drained] {
            try {
                normalizer->publish(0, 1);
                ++prepublication_drained;
                return 1;
            } catch (...) {
                ++prepublication_drained;
                throw;
            }
        }));
        wave.add(std::async(std::launch::async, [normalizer, &prepublication_drained]() -> int {
            try {
                throw std::runtime_error("lane failed before publication");
            } catch (...) {
                normalizer->fail(std::current_exception());
                ++prepublication_drained;
                throw;
            }
        }));
        REQUIRE_THROWS(wave.settle(device, [](int&) {}));
    }
    REQUIRE(prepublication_drained.load() == 2);

#if defined(USE_C10D_NCCL)
    std::atomic<int> collective_failure_drained = 0;
    std::latch both_published(2);
    auto failing_backend = c10::make_intrusive<FailingCollectiveBackend>();
    rfdetr::DistributedContext failing_distributed;
    failing_distributed.enabled = true;
    failing_distributed.world_size = 2;
    failing_distributed.process_group = failing_backend;
    {
        rfdetr::ParallelTrainingWave<int> wave(2, true, 0, failing_distributed);
        const auto normalizer = wave.normalizer();
        for (std::size_t lane = 0; lane < 2; ++lane) {
            wave.add(std::async(std::launch::async, [normalizer, lane, &both_published, &collective_failure_drained]() -> int {
                try {
                    c10::cuda::CUDAGuard guard(static_cast<c10::DeviceIndex>(0));
                    const auto stream = c10::cuda::getCurrentCUDAStream(static_cast<c10::DeviceIndex>(0));
                    normalizer->publish(lane, static_cast<std::int64_t>(lane));
                    both_published.count_down();
                    both_published.wait();
                    static_cast<void>(normalizer->consume(lane, stream.stream()));
                    return 0;
                } catch (...) {
                    ++collective_failure_drained;
                    throw;
                }
            }));
        }
        REQUIRE_THROWS(wave.settle(device, [](int&) {}));
    }
    REQUIRE(collective_failure_drained.load() == 2);
    REQUIRE(failing_backend->allreduces.load() == 1);
    REQUIRE(failing_backend->waits.load() == 1);
    REQUIRE(failing_backend->aborts.load() == 1);
#endif

    std::atomic<int> cancellation_drained = 0;
    {
        rfdetr::ParallelTrainingWave<int> wave(2, true, 0, distributed);
        const auto normalizer = wave.normalizer();
        for (std::size_t lane = 0; lane < 2; ++lane) {
            wave.add(std::async(std::launch::async, [normalizer, lane, &cancellation_drained]() -> int {
                try {
                    normalizer->publish(lane, 0);
                    static_cast<void>(normalizer->consume(lane, nullptr));
                    return 0;
                } catch (...) {
                    ++cancellation_drained;
                    throw;
                }
            }));
        }
    }
    REQUIRE(cancellation_drained.load() == 2);

    std::atomic<int> inactive_drained = 0;
    {
        rfdetr::ParallelTrainingWave<int> wave(2, false, 0, distributed);
        wave.add(std::async(std::launch::async, [&inactive_drained] {
            ++inactive_drained;
            return 1;
        }));
        wave.add(std::async(std::launch::async, [&inactive_drained]() -> int {
            ++inactive_drained;
            throw std::runtime_error("inactive lane failure");
        }));
        REQUIRE_THROWS(wave.settle(device, [](int&) {}));
    }
    REQUIRE(inactive_drained.load() == 2);
}

void test_all_supervision_routes_execute_fixture_backed_training() {
    if (mmltk::testsupport::checked_cuda_device_count() == 0) { SKIP("CUDA device unavailable; GPU coverage remains unverified"); }
    const auto root = std::filesystem::temp_directory_path() / "mmltk_rfdetr_supervision_routes";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    mmltk::backend::data::testsupport::FixtureSpec fixture;
    fixture.root_dir = root.string();
    fixture.width = 64;
    fixture.height = 64;
    fixture.num_images = 12;
    mmltk::backend::data::testsupport::create_synthetic_dataset(fixture);
    {
        const auto annotated =
            std::filesystem::path(mmltk::backend::data::testsupport::dataset_dir(fixture)) / fixture.split / "000011.jsonl";
        std::ifstream input(annotated);
        std::string annotation;
        REQUIRE(static_cast<bool>(std::getline(input, annotation)));
        input.close();
        std::ofstream output(annotated, std::ios::app);
        REQUIRE(output.good());
        for (int additional = 0; additional < 3; ++additional) output << annotation << '\n';
        // Distinct source identities keep both lane caches populated and permit
        // deterministic copy-paste between over-capacity images. Images 9, 10,
        // and 12 remain empty so all routes still exercise mixed batches.
        for (int image_index = 1; image_index <= 8; ++image_index) {
            const auto destination = annotated.parent_path() / ("00000" + std::to_string(image_index) + ".jsonl");
            std::ofstream annotations(destination, std::ios::trunc);
            REQUIRE(annotations.good());
            for (int instance = 0; instance < 4; ++instance) annotations << annotation << '\n';
        }
    }
    {
        const auto second_annotated =
            std::filesystem::path(mmltk::backend::data::testsupport::dataset_dir(fixture)) / fixture.split / "000012.jsonl";
        std::ofstream clear_annotations(second_annotated, std::ios::trunc);
        REQUIRE(clear_annotations.good());
    }
    mmltk::backend::data::CompilerConfig compiler;
    compiler.source_dir = mmltk::backend::data::testsupport::dataset_dir(fixture);
    compiler.output_dir = mmltk::backend::data::testsupport::compiled_dir(fixture);
    compiler.split = fixture.split;
    compiler.target_width = 64;
    compiler.target_height = 64;
    const auto compile_plan = mmltk::backend::data::DatasetCompiler::prepare(compiler, {fixture.split});
    mmltk::backend::data::DatasetCompiler::compile(compile_plan, 0U);

    auto config = rfdetr::native_config_from_preset(rfdetr::model_presets().front());
    config.resolution = 64;
    config.num_classes = 2;
    config.num_queries = 2;
    config.num_select = 2;
    config.segmentation = false;
    rfdetr::NativeRfDetrModel seed_model(config);
    rfdetr::DecodedNativeModelState checkpoint;
    checkpoint.metadata.preset_name = config.preset_name;
    checkpoint.metadata.source_kind = "training-route-test";
    checkpoint.metadata.source_path = (root / "seed.pt").string();
    checkpoint.metadata.num_classes = config.num_classes;
    checkpoint.metadata.num_queries = config.num_queries;
    checkpoint.metadata.num_select = config.num_select;
    const auto& seed_module = rfdetr::detail::native_model_owner(seed_model).module();
    rfdetr::detail::model_state_owner(checkpoint).entries = rfdetr::testsupport::clone_normalized_model_state(seed_module);
    const auto weights = root / "seed.pt";
    rfdetr::save_native_checkpoint(weights, checkpoint);

    const std::array routes{
        rfdetr::TrainingSupervisionConfig{},
        [] {
            rfdetr::TrainingSupervisionConfig value;
            value.assignment = rfdetr::TrainAssignmentKind::MatchFree;
            return value;
        }(),
        [] {
            rfdetr::TrainingSupervisionConfig value;
            value.denoising.enabled = true;
            return value;
        }(),
        [] {
            rfdetr::TrainingSupervisionConfig value;
            value.assignment = rfdetr::TrainAssignmentKind::MatchFree;
            value.denoising.enabled = true;
            return value;
        }(),
    };
    for (std::size_t route_index = 0; route_index < routes.size(); ++route_index) {
        for (const int lanes : {1, 2}) {
            rfdetr::TrainRequest request;
            request.h2d_dataloader = true;
            request.train_compiled_path = mmltk::backend::data::testsupport::compiled_bin_path(fixture);
            request.val_compiled_path = request.train_compiled_path;
            request.weights_path = weights;
            request.output_dir = root / ("route-" + std::to_string(route_index) + "-lanes-" + std::to_string(lanes));
            request.preset_name = config.preset_name;
            request.batch_size = lanes == 1 ? 5 : 2;
            request.val_batch_size = 12;
            request.num_queries = static_cast<std::size_t>(config.num_queries);
            request.epochs = 1;
            request.workers = 4;
            request.lanes = lanes;
            request.compilation_mode = rfdetr::CompilationMode::kNone;
            request.amp = false;
            request.progress_bar = false;
            request.validation_loss = true;
            request.use_ema = route_index == routes.size() - 1 && lanes == 2;
            if (route_index == routes.size() - 1 && lanes == 2) {
                request.gpu_augmentation.enabled = true;
                request.gpu_augmentation.copy_paste_probability = 1.0F;
            }
            if (route_index == 0 && lanes == 2) {
                request.gpu_augmentation = rfdetr::test_support::isolated_augmentation_config(1.0F);
            }
            request.training_supervision = routes[route_index];
            const auto result = rfdetr::run_training(request);
            REQUIRE(result.history.size() == 1);
            REQUIRE(std::isfinite(result.history.front().train_loss));
            REQUIRE(result.history.front().val_loss.has_value());
            REQUIRE(result.best_checkpoint_path.has_value());

            auto deployment_config = result.artifacts.config;
            deployment_config.training_supervision = {};
            rfdetr::NativeRfDetrModel deployment_model(deployment_config);
            const auto deployment_summary = rfdetr::load_model_weights(deployment_model, *result.best_checkpoint_path, false);
            REQUIRE(deployment_summary.unexpected_names.empty());
            REQUIRE(deployment_summary.incompatible_names.empty());
            const auto deployment_state = rfdetr::decode_model_state(*result.best_checkpoint_path);
            for (const auto& entry : rfdetr::detail::model_state_owner(deployment_state).entries) {
                REQUIRE_FALSE(entry.name.starts_with("training_supervision."));
            }
            if (request.use_ema) {
                auto resume_request = request;
                resume_request.weights_path.clear();
                resume_request.resume_path = result.checkpoint_path;
                resume_request.output_dir = root / "active-resume";
                resume_request.epochs = 2;
                const auto resumed = rfdetr::run_training(resume_request);
                REQUIRE(resumed.history.size() == 1);
                REQUIRE(resumed.last_epoch == 1);
                REQUIRE(std::isfinite(resumed.history.front().train_loss));
                REQUIRE(resumed.artifacts.config.num_queries == config.num_queries);
                resume_request.num_queries = static_cast<std::size_t>(config.num_queries + 1);
                REQUIRE_THROWS(rfdetr::run_training(resume_request));
            }
        }
    }

#if defined(USE_C10D_NCCL)
    if (mmltk::testsupport::checked_cuda_device_count() >= 2) {
        for (std::size_t route_index = 0; route_index < routes.size(); ++route_index) {
            const auto& distributed_route = routes[route_index];
            const auto distributed_output = root / ("distributed-route-" + std::to_string(route_index));
            const auto store_path = distributed_output / "rendezvous";
            std::filesystem::create_directories(distributed_output);
            std::array<std::future<rfdetr::TrainRunResult>, 2> workers;
            for (int rank = 0; rank < 2; ++rank) {
                workers[static_cast<std::size_t>(rank)] = std::async(std::launch::async, [&, rank] {
                    rfdetr::TrainRequest request;
                    request.h2d_dataloader = true;
                    request.train_compiled_path = mmltk::backend::data::testsupport::compiled_bin_path(fixture);
                    request.val_compiled_path = request.train_compiled_path;
                    request.weights_path = weights;
                    request.output_dir = distributed_output;
                    request.preset_name = config.preset_name;
                    request.batch_size = 1;
                    request.val_batch_size = 12;
                    request.num_queries = static_cast<std::size_t>(config.num_queries);
                    request.epochs = 1;
                    request.workers = 2;
                    request.lanes = 2;
                    request.compilation_mode = rfdetr::CompilationMode::kNone;
                    request.amp = false;
                    request.progress_bar = false;
                    request.validation_loss = true;
                    request.training_supervision = distributed_route;
                    if (route_index == 0) { request.gpu_augmentation = rfdetr::test_support::isolated_augmentation_config(1.0F); }
                    request.distributed_worker = true;
                    request.distributed_rank = rank;
                    request.distributed_world_size = 2;
                    request.distributed_store_path = store_path;
                    request.device_id = rank;
                    return rfdetr::run_training(request);
                });
            }
            for (auto& worker : workers) {
                const auto result = worker.get();
                REQUIRE(result.history.size() == 1);
                REQUIRE(std::isfinite(result.history.front().train_loss));
            }
        }
    }
#endif

    std::filesystem::remove_all(root, ignored);
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]",
                         test_training_supervision_runtime_replication_is_one_shot);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]",
                         test_checkpoint_supervision_config_and_deployment_pruning);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]", test_feature_active_host_target_invariants);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]", test_ema_shadow_admission_is_transactional);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]",
                         test_native_optimizer_late_failure_preserves_live_state);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]", test_resume_continuation_manifest_is_exact);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]",
                         test_target_scratch_reuse_waits_for_consumer_retirement);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]",
                         test_target_staging_ring_recycles_completed_slots_without_host_wait);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]",
                         test_target_scratch_retires_cross_device_events_on_their_owner);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]",
                         test_build_targets_recovers_after_staging_growth_and_failure);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]",
                         test_training_adapter_matches_raw_augmentation_executor);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]",
                         test_training_mask_targets_follow_spatial_image_erasure);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]",
                         test_parallel_wave_drains_failures_and_cancellation);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][supervision][training_supervision]",
                         test_all_supervision_routes_execute_fixture_backed_training);

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][augmentation][support]", test_native_augmentation_preview_target_support_parity);

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][augmentation][support]", test_tiny_mask_training_outer_edges);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision][augmentation][copy_paste]", test_copy_paste_ring_support_and_cache_cycles);

MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training_supervision][augmentation][copy_paste]",
                         test_copy_paste_cache_publication_recovers_without_targets);
