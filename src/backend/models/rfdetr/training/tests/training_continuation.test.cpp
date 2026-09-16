#include <array>
#include <limits>
#include <string>
#include <vector>
#include <utility>
#include "catch2_compat.hpp"
#include "archive_utils.h"
#include "detail/checkpoint_private.h"
#include "detail/model_ema.h"
#include "detail/training_continuation.h"
#include "training_continuation_fixture.h"
namespace {
namespace r = mmltk::backend::models::rfdetr;
namespace api = mmltk::backend::ml::torch_api;
r::TrainRequest saved_request() {
    r::TrainRequest request;
    request.train_compiled_path = "train.bin";
    request.val_compiled_path = "val.bin";
    request.weights_path = "seed.pt";
    request.output_dir = "run";
    request.gpu_augmentation.perceptual_downscale = true;
    return request;
}
api::InputArchive continuation_fixture(const r::TrainRequest& request) {
    api::OutputArchive output;
    r::detail::write_training_continuation(output, request,
                                           {.epoch = 0,
                                            .best_regular_metric = -std::numeric_limits<double>::infinity(),
                                            .best_ema_metric = -std::numeric_limits<double>::infinity(),
                                            .grad_scaler_scale = 1024.0,
                                            .grad_scaler_growth_tracker = 17,
                                            .ema_completed_updates = request.use_ema ? 37 : 0,
                                            .training_attempt_id = "attempt",
                                            .training_original_descriptor = "original.json"});
    api::OutputArchive optimizer;
    r::write_int(optimizer, "fixture", 1);
    output.write("optimizer", optimizer);
    if (request.use_ema) {
        api::OutputArchive ema;
        r::write_int(ema, "entry_count", 0);
        output.write("ema_state", ema);
    }
    return r::testsupport::checkpoint_input(output);
}
void test_current_continuation_required_fields() {
    auto source = continuation_fixture(saved_request());
    const auto loaded = r::detail::read_training_continuation(source);
    REQUIRE(loaded.has_value());
    CHECK(loaded->configuration.gpu_augmentation.perceptual_downscale);
    CHECK_FALSE(loaded->configuration.gpu_augmentation.enabled);
    // Every written continuation fact is mandatory except the default supervision
    // blob, whose current format deliberately omits the default value.
    for (const auto& key : source.keys()) {
        api::OutputArchive incomplete;
        r::testsupport::copy_checkpoint_archive(source, incomplete, key);
        auto input = r::testsupport::checkpoint_input(incomplete);
        INFO(key);
        REQUIRE_THROWS(r::detail::read_training_continuation(input));
    }
    for (const auto& key : source.keys()) {
        api::OutputArchive malformed;
        r::testsupport::copy_checkpoint_archive(source, malformed, key);
        malformed.write(key, api::IValue(c10::List<int64_t>{}));
        auto input = r::testsupport::checkpoint_input(malformed);
        INFO(key);
        REQUIRE_THROWS(r::detail::read_training_continuation(input));
    }
    for (const auto* key : {"ema_state", "training_supervision_config_cbor"}) {
        api::OutputArchive malformed;
        r::testsupport::copy_checkpoint_archive(source, malformed);
        malformed.write(key, api::IValue(int64_t{1}));
        auto input = r::testsupport::checkpoint_input(malformed);
        REQUIRE_THROWS(r::detail::read_training_continuation(input));
    }
    api::OutputArchive weights;
    r::write_string(weights, "source_kind", "weights-only");
    auto input = r::testsupport::checkpoint_input(weights);
    REQUIRE_FALSE(r::detail::read_training_continuation(input).has_value());
}
void test_current_continuation_scalar_boundaries() {
    const std::array<std::pair<std::string, api::IValue>, 22> invalid{
        {{"epoch", int64_t{-1}},
         {"epoch", int64_t{std::numeric_limits<int>::max()}},
         {"grad_scaler_scale", 0.0},
         {"grad_scaler_scale", std::numeric_limits<double>::infinity()},
         {"grad_scaler_growth_tracker", int64_t{-1}},
         {"grad_scaler_growth_tracker", int64_t{std::numeric_limits<int>::max()} + 1},
         {"ema_completed_updates", int64_t{-1}},
         {"ema_completed_updates", std::numeric_limits<int64_t>::max()},
         {"ema_completed_updates", int64_t{1}},
         {"best_regular_metric", std::numeric_limits<double>::quiet_NaN()},
         {"best_ema_metric", std::numeric_limits<double>::quiet_NaN()},
         {"training_attempt_id", std::string{}},
         {"training_attempt_id", std::string(65, 'a')},
         {"training_original_descriptor", std::string(mmltk::frameworks::reflection::kMaximumPathBytes + 1, 'a')},
         {"warmup_epochs", 0.25},
         {"warmup_momentum", 0.25},
         {"lr_min_factor", 0.25},
         {"lr_drop", int64_t{7}},
         {"lr_scheduler", std::string("cosine")},
         {"gpu_augment_perceptual_downscale", false},
         {"optimizer_kind", std::string("invalid")},
         {"gpu_augment_geometry_probability", 0.125}}};
    for (const auto& [key, value] : invalid) {
        auto source = continuation_fixture(saved_request());
        api::OutputArchive output;
        r::testsupport::copy_checkpoint_archive(source, output, key);
        output.write(key, value);
        auto input = r::testsupport::checkpoint_input(output);
        INFO(key);
        REQUIRE_THROWS(r::detail::read_training_continuation(input));
    }
    for (const auto* key : {"training_configuration_cbor", "training_supervision_config_cbor"}) {
        auto source = continuation_fixture(saved_request());
        api::OutputArchive output;
        r::testsupport::copy_checkpoint_archive(source, output, key);
        output.write(key, api::zeros({1024 * 1024}, api::kUInt8));
        auto input = r::testsupport::checkpoint_input(output);
        REQUIRE_THROWS(r::detail::read_training_continuation(input));
    }
    auto supervised = saved_request();
    supervised.training_supervision.denoising.enabled = true;
    auto supervised_source = continuation_fixture(supervised);
    REQUIRE(r::detail::read_training_continuation(supervised_source).has_value());
    api::OutputArchive missing_supervision;
    r::testsupport::copy_checkpoint_archive(supervised_source, missing_supervision, "training_supervision_config_cbor");
    auto missing_input = r::testsupport::checkpoint_input(missing_supervision);
    REQUIRE_THROWS(r::detail::read_training_continuation(missing_input));
    for (auto optimizer : {r::TrainOptimizerKind::AdamW, r::TrainOptimizerKind::Muon}) {
        for (bool ema : {false, true}) {
            auto request = saved_request();
            request.optimizer = optimizer;
            request.use_ema = ema;
            auto source = continuation_fixture(request);
            const auto admitted = r::detail::read_training_continuation(source);
            REQUIRE(admitted.has_value());
            REQUIRE(admitted->configuration == request);
            REQUIRE(admitted->values.ema_completed_updates == (ema ? 37 : 0));
            auto active = request;
            active.output_dir = "another-run";
            active.epochs += 10;
            REQUIRE_NOTHROW(r::detail::require_active_training_continuation(*admitted, active));
            active.optimizer = optimizer == r::TrainOptimizerKind::AdamW ? r::TrainOptimizerKind::Muon : r::TrainOptimizerKind::AdamW;
            REQUIRE_THROWS(r::detail::require_active_training_continuation(*admitted, active));
            active = request;
            active.lr_drop += 1;
            REQUIRE_THROWS(r::detail::require_active_training_continuation(*admitted, active));
            active = request;
            active.training_supervision.denoising.enabled = true;
            REQUIRE_THROWS(r::detail::require_active_training_continuation(*admitted, active));
            active = request;
            active.use_ema = !ema;
            REQUIRE_THROWS(r::detail::require_active_training_continuation(*admitted, active));
        }
    }
}
void test_ordered_cpu_ema_admission() {
    const std::vector<std::string> names{"first", "second"};
    const std::vector<api::Tensor> parameters{api::ones({2, 3}), api::zeros({4})};
    const auto pointer = parameters.front().data_ptr();
    REQUIRE_NOTHROW(r::ModelEma::validate_cpu_shadow(parameters, parameters));
    for (int fault = 0; fault != 6; ++fault) {
        auto shadow = parameters;
        switch (fault) {
            case 0: shadow.pop_back(); break;
            case 1: shadow[0] = api::Tensor{}; break;
            case 2: shadow[0] = api::zeros({3, 2}); break;
            case 3: shadow[0] = shadow[0].to(torch::kFloat64); break;
            case 4: shadow[0] = api::full({2, 3}, std::numeric_limits<float>::infinity()); break;
            case 5: shadow[0] = shadow[0].to_sparse(); break;
        }
        REQUIRE_THROWS(r::ModelEma::from_cpu_shadow(parameters, shadow, 0.9, 100, 37));
        REQUIRE(parameters.front().data_ptr() == pointer);
        REQUIRE(api::equal(parameters.front(), api::ones({2, 3})));
    }
    for (const auto count : {int64_t{-1}, std::numeric_limits<int64_t>::max()}) {
        REQUIRE_THROWS(r::ModelEma::from_cpu_shadow(parameters, parameters, 0.9, 100, count));
        REQUIRE(parameters.front().data_ptr() == pointer);
        REQUIRE(api::equal(parameters.front(), api::ones({2, 3})));
    }
    for (int fault = 0; fault != 5; ++fault) {
        api::OutputArchive output;
        r::write_int(output, "entry_count", fault == 1 ? 1 : 2);
        for (std::size_t i = 0; i != names.size(); ++i) {
            api::OutputArchive entry;
            r::write_string(entry, "name", names[fault == 2 ? 1 - i : fault == 4 ? 0 : i]);
            if (fault != 3) entry.write("tensor", parameters[i]);
            output.write(r::archive_entry_name(i), entry);
        }
        auto input = r::testsupport::checkpoint_input(output);
        if (fault == 0) {
            const auto shadow = r::detail::read_ema_shadow_archive(input, names);
            REQUIRE_NOTHROW(r::ModelEma::validate_cpu_shadow(parameters, shadow));
        } else
            REQUIRE_THROWS(r::detail::read_ema_shadow_archive(input, names));
    }
}
}  // namespace
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][continuation]", test_current_continuation_required_fields);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][continuation]", test_current_continuation_scalar_boundaries);
MMLTK_REGISTER_TEST_CASE("[model][rfdetr][training][continuation][ema]", test_ordered_cpu_ema_admission);
