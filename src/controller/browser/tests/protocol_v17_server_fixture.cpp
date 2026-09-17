#include "src/controller/contracts/application_systems.h"
#include "src/controller/browser/application_schema.h"
#include "src/controller/browser/application_materializer.h"
#include "src/controller/browser/tests/annotation_wire_fixture.h"
#include "src/controller/contracts/default_state.h"
#include "src/controller/contracts/model_selection.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>
int main(const int argument_count, char* const* const arguments) {
    using namespace mmltk::controller;
    using namespace mmltk::controller::browser;
    if (argument_count != 2 || arguments[1] == nullptr || std::string_view(arguments[1]).empty()) return EXIT_FAILURE;
    std::size_t system_count = 0U;
    std::size_t intent_count = 0U;
    std::size_t interaction_count = 0U;
    std::size_t event_count = 0U;
    ApplicationSchema<ApplicationSystems>::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        ++system_count;
        constexpr auto annotation = application_schema_detail::annotation_value<Snapshot, contracts::reflection::Snapshot>();
        static_assert(annotation.byte_budget != 0U);
    });
    ApplicationSchema<ApplicationSystems>::VisitEndpoints([&]<class Endpoint>() {
        if constexpr (Endpoint::interaction)
            ++interaction_count;
        else
            ++intent_count;
    });
    ApplicationSchema<ApplicationSystems>::VisitEvents([&]<class Identity, class Event>(const contracts::reflection::Event annotation) {
        ++event_count;
        if (!mmltk::frameworks::reflection::enum_contains(annotation.delivery)) event_count = 0U;
    });
    if (system_count != 13U || intent_count == 0U || interaction_count == 0U || event_count == 0U) return EXIT_FAILURE;
    using SettingsEvent = ApplicationEventIdentity<ApplicationSystems, &ApplicationSystems::settings, SettingsChanged>;
    Bootstrap bootstrap{.schema_fingerprint = application_schema_fingerprint<ApplicationSystems>().words, .snapshots = {}};
    bool snapshots_valid = true;
    std::optional<wire::Value> named_annotation_reference;
    ApplicationSchema<ApplicationSystems>::VisitSnapshotDefaults([&]<class Cell, class Snapshot>(Snapshot snapshot) {
        if constexpr (requires { typename Cell::type::visual_source; }) {
            using Projection = typename Cell::type::visual_source;
            const auto session = presentation_source_session(Projection::kind);
            auto& frame = mmltk::frameworks::reflection::access<Snapshot, Projection::frame>(snapshot);
            frame = visual_frame({Projection::kind, 1U}, {32U, 24U}, 7U + session);
            frame.content = {1U, 2U, 20U, 16U};
            frame.clean_revision = 43U;
            mmltk::frameworks::reflection::access<Snapshot, Projection::revision>(snapshot) =
                Projection::kind == PresentationSourceKind::Predict ? 7U + session : std::numeric_limits<std::uint64_t>::max() - session;
            snapshots_valid = snapshots_valid && visual_clean_content_identity(frame).revision == 43U;
        }
        if constexpr (std::same_as<Snapshot, ValidationSnapshot>) {
            snapshot.operation.generation_frontier = 7U;
            snapshot.metrics.emplace();
            snapshot.metrics->bbox.available = true;
            snapshot.metrics->bbox.ap = 0.75;
            snapshot.metrics->bbox.average_recall = {0.25, 0.5, 0.75};
            snapshot.metrics->bbox.area_ap = {0.0, std::nullopt, 1.0};
            snapshot.metrics->bbox.confidence = {0.8, 0.6, 0.6857142857142857};
            snapshot.metrics->bbox.confidence_threshold = 0.58;
            snapshot.metrics->model_detection_budget = 500U;
            snapshot.detail_rows = 2U;
            snapshot.content_identity = 71U;
            snapshot.sample_identities[0] = {7U, 3U};
            snapshot.sample_available[0] = true;
        }
        if constexpr (std::same_as<Snapshot, AnnotationSnapshot>) {
            auto& scene = snapshot.ui.scene;
            scene.document = contracts::WorkspaceResource::From("annotation-codec-fixture", 1U);
            scene.frame_width = 32U;
            scene.frame_height = 24U;
            scene.frame_ready = true;
            scene.categories = {{.value = "fixture"}};
            scene.palette = {{.hue = 123.4567F, .saturation = 0.1234567F, .value = 0.7654321F}};
            const contracts::AnnotationPoint point{1.234567F, 2.345678F};
            auto object = test_support::make_annotation_wire_object(point, scene.palette.front());
            object.mask = {.runs = {{1U, 2U, 3U}}, .cleanup_radius = 17U, .present = true};
            scene.objects = {object};
            snapshot.ui.editor.selected_object = 0U;
            auto reference = mmltk::frameworks::serialization::reflected_value(snapshot);
            if (!reference) {
                snapshots_valid = false;
                return;
            }
            named_annotation_reference = std::move(*reference);
        }
        auto encoded = mmltk::frameworks::serialization::reflected_transport_value(snapshot);
        if (!encoded) {
            snapshots_valid = false;
            return;
        }
        bootstrap.snapshots.push_back({.system_id = Cell::stable_id, .value = std::move(*encoded)});
    });
    if (!snapshots_valid || !named_annotation_reference) return EXIT_FAILURE;
    std::uint64_t dialog_id = 0U;
    ApplicationSchema<ApplicationSystems>::VisitApplicationSettingsLeaves(
        [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsLeafFact& fact) {
            if (dialog_id == 0U && fact.file_dialog) dialog_id = fact.stable_id;
        });
    if (dialog_id == 0U) return EXIT_FAILURE;
    const services::FileDialogSelection cancelled{
        .target = services::FileDialogTarget{services::SettingsFieldTarget{dialog_id}},
        .result = services::FileDialogCancelled{},
    };
    const services::FileDialogSelection selected{
        .target = services::FileDialogTarget{services::SettingsFieldTarget{dialog_id}},
        .result =
            services::FileDialogSelected{
                .path = "/tmp/protocol-v17-fixture",
            },
    };
    auto cancelled_reply = mmltk::frameworks::serialization::reflected_transport_value(FileDialogSnapshot{
        .target = services::FileDialogTarget{services::SettingsFieldTarget{dialog_id}},
        .selection = cancelled,
    });
    auto selected_reply = mmltk::frameworks::serialization::reflected_transport_value(FileDialogSnapshot{
        .target = services::FileDialogTarget{services::SettingsFieldTarget{dialog_id}},
        .selection = selected,
    });
    auto event = mmltk::frameworks::serialization::reflected_transport_value(SettingsChanged{.snapshot = contracts::SettingsUiState{}});
    if (!cancelled_reply || !selected_reply || !event) return EXIT_FAILURE;
    bootstrap.input_epoch = 1U;
    std::vector<ServerRecord> records{
        std::move(bootstrap),
        IntentReply{
            .correlation = 17U,
            .result = std::move(*cancelled_reply),
            .error = {},
        },
        IntentReply{
            .correlation = 18U,
            .result = std::move(*selected_reply),
            .error = {},
        },
        SystemEvent{
            .system_id = SettingsEvent::system_id,
            .event_id = SettingsEvent::event_id,
            .delivery = contracts::reflection::EventDelivery::Critical,
            .value = std::move(*event),
        },
        InteractionRejected{.endpoint_id = application_stable_id("annotation", "Input"),
                            .error = {.category = contracts::ApplicationErrorCategory::Unavailable, .detail = "fixture input unavailable"}},
        InteractionRejected{.endpoint_id = application_stable_id("explore", "UpdateViewport"),
                            .error = {.category = contracts::ApplicationErrorCategory::Unavailable, .detail = "fixture unavailable"}},
        InteractionRejected{.endpoint_id = application_stable_id("annotation", "Input"),
                            .error = {.category = contracts::ApplicationErrorCategory::Failed, .detail = std::string(kMaxErrorDetailBytes, 'r')}},
        IntegrationControl{.receipt = {.kind = contracts::IntegrationControlKind::Advance, .sequence = 2U}},
    };
    // An independent named persistence projection is test data, carried as a
    // dynamic value. The client compares every decoded transport field against
    // it through the separate named codec, without a handwritten field mirror.
    records.emplace_back(IntentReply{.correlation = 19U, .result = std::move(*named_annotation_reference)});
    using ControlKind = contracts::IntegrationControlKind;
    contracts::visit_integration_commands([&]<auto Kind, auto Policy>(auto) {
        if constexpr (Policy.server && Kind != ControlKind::Advance) {
            records.emplace_back(
                IntegrationControl{.receipt = {.kind = Kind, .sequence = 2U, .read_generation = Policy.read_generation ? 7U : 0U, .compiled_index = 0U}});
        }
    });
    // Native settings/projection pairs exercise every relation row through the
    // independent named codec as well as the generated direct Rust projection.
    std::uint64_t model_correlation = 400U;
    bool model_projections_valid = true;
    contracts::ModelSelectionRelation::VisitRows([&]<class Row>(const auto& compatibility) {
        for (unsigned mode = 0U; mode != 4U; ++mode) {
            auto settings = contracts::default_gui_settings_state();
            Row::source(settings) = mode == 1U ? contracts::ModelSelectionSource::Canonical : contracts::ModelSelectionSource::Custom;
            Row::input(settings) = mode == 2U ? contracts::ModelArtifactInputKind::None : compatibility.input;
            Row::artifact(settings) = "/tmp/fixture-model";
            Row::class_layout(settings) = "/tmp/fixture-model.classes.json";
            if constexpr (std::tuple_size_v<decltype(Row::predicate)> != 0U)
                std::get<0>(Row::predicate)(settings) =
                    mode == 3U ? !*compatibility.required_export_build_tensorrt : *compatibility.required_export_build_tensorrt;
            const auto projection = contracts::model_settings_projection(settings, compatibility.workflow);
            if (!projection) {
                model_projections_valid = false;
                return;
            }
            auto encoded_settings = mmltk::frameworks::serialization::reflected_value(settings);
            auto encoded_projection = mmltk::frameworks::serialization::reflected_value(*projection);
            if (!encoded_settings || !encoded_projection) {
                model_projections_valid = false;
                return;
            }
            records.emplace_back(IntentReply{.correlation = model_correlation++, .result = std::move(*encoded_settings)});
            records.emplace_back(IntentReply{.correlation = model_correlation++, .result = std::move(*encoded_projection)});
        }
    });
    if (!model_projections_valid) return EXIT_FAILURE;
    // Full metric grids and paired sample metadata exercise independent named
    // and positional codecs, in addition to the compact bootstrap summary.
    namespace rfdetr = mmltk::backend::models::rfdetr;
    rfdetr::EvaluationDetailPage metric_page{7U, 2U, 0U, {rfdetr::EvaluationMetricDetail{}, rfdetr::EvaluationMetricDetail{}}};
    metric_page.rows[0].available = true;
    metric_page.rows[0].ground_truth_count = 17U;
    metric_page.rows[0].precision_curve[9][100] = 0.125;
    metric_page.rows[0].average_recall[2][9] = 0.375;
    metric_page.rows[1].category = 5U;
    metric_page.rows[1].category_name.emplace();
    for (std::size_t index = 0U; index < 128U; ++index) metric_page.rows[1].category_name->value += "é";
    metric_page.rows[1].kind = rfdetr::EvaluationMetricKind::Mask;
    ValidationImageMetadata sample_image;
    sample_image.frame = visual_frame({PresentationSourceKind::Validation, 1U}, {768U, 512U}, 77U);
    sample_image.content_identity = 71U;
    sample_image.overlays = {false, true, false, true};
    auto& sample = sample_image.samples[0];
    sample.identity = {7U, 3U};
    sample.available = true;
    sample.crop = {0U, 0U, 256U, 256U};
    sample.original_extent = {640U, 640U};
    sample.labels.push_back({{{1.25F, 2.5F}, {15.0F, 19.0F}}, {}, 5U, true, 0.0F, "last"});
    std::uint64_t validation_correlation = 600U;
    const auto append_validation = [&](const auto& value) {
        auto named = mmltk::frameworks::serialization::reflected_value(value);
        auto transport = mmltk::frameworks::serialization::reflected_transport_value(value);
        if (!named || !transport) return false;
        records.emplace_back(IntentReply{.correlation = validation_correlation++, .result = std::move(*named)});
        records.emplace_back(IntentReply{.correlation = validation_correlation++, .result = std::move(*transport)});
        return true;
    };
    if (!append_validation(metric_page) || !append_validation(sample_image)) return EXIT_FAILURE;
    if (!append_validation(rfdetr::kEvaluationAxes.iou) || !append_validation(rfdetr::kEvaluationAxes.recall) ||
        !append_validation(rfdetr::kEvaluationAxes.confidence))
        return EXIT_FAILURE;
    // Malformed pages cross the real native record encoder and Rust decoders.
    // Mutating wire values deliberately bypasses native output admission.
    const auto named_member = [](wire::Value& value, std::string_view name) -> wire::Value& {
        auto& fields = std::get<wire::Value::Object>(value.storage);
        return std::ranges::find_if(fields, [&](const auto& field) { return field.first == name; })->second;
    };
    for (const bool oversized_name : {false, true}) {
        auto named = *mmltk::frameworks::serialization::reflected_value(metric_page);
        auto positional = *mmltk::frameworks::serialization::reflected_transport_value(metric_page);
        auto& named_rows = std::get<wire::Value::Array>(named_member(named, "rows").storage);
        auto& positional_rows = std::get<wire::Value::Array>(std::get<wire::Value::Array>(positional.storage).back().storage);
        if (oversized_name) {
            named_member(named_member(named_rows[1], "category_name"), "value").storage = std::string(257U, 'x');
            // Field index is derived from the actual native named projection.
            const auto& named_fields = std::get<wire::Value::Object>(named_rows[1].storage);
            const auto index = static_cast<std::size_t>(std::ranges::find_if(named_fields, [](const auto& field) { return field.first == "category_name"; }) -
                                                        named_fields.begin());
            auto& name = std::get<wire::Value::Array>(positional_rows[1].storage)[index];
            std::get<wire::Value::Array>(name.storage).front().storage = std::string(257U, 'x');
        } else {
            named_rows.resize(rfdetr::kEvaluationDetailPageSize + 1U, named_rows.front());
            positional_rows.resize(rfdetr::kEvaluationDetailPageSize + 1U, positional_rows.front());
        }
        rfdetr::EvaluationDetailPage rejected;
        if (mmltk::frameworks::serialization::decode_into(rejected, named)) return EXIT_FAILURE;
        records.emplace_back(IntentReply{.correlation = validation_correlation++, .result = std::move(named)});
        records.emplace_back(IntentReply{.correlation = validation_correlation++, .result = std::move(positional)});
    }
    for (const std::uint32_t count : {0U, 5U}) {
        auto invalid_query = *mmltk::frameworks::serialization::reflected_value(rfdetr::EvaluationDetailQuery{});
        named_member(invalid_query, "count").storage = static_cast<std::uint64_t>(count);
        rfdetr::EvaluationDetailQuery rejected;
        if (mmltk::frameworks::serialization::decode_into(rejected, invalid_query)) return EXIT_FAILURE;
    }
    validation_correlation = 700U;
    rfdetr::TrainingRecord training_record;
    training_record.run_id = "run-native";
    training_record.attempt_id = "attempt-native";
    training_record.sequence = 17;
    training_record.dropped_before = 2;
    training_record.role = rfdetr::TrainingRecordRole::Epoch;
    training_record.evaluated_weights = rfdetr::EvaluatedWeights::Ema;
    training_record.progress.phase = rfdetr::TrainingPhase::EpochComplete;
    training_record.progress.scalars.total = 1.25;
    training_record.progress.scalars.learning_rate = 0.0001;
    training_record.progress.scalars.learning_rate_min = 0.00001;
    training_record.progress.scalars.learning_rate_max = 0.001;
    training_record.progress.scalars.correspondence_weighted = 0.25;
    training_record.progress.full_checkpoint_path = "/copied/full.pt";
    training_record.progress.checkpoint_path = "/run/epoch.pth";
    training_record.progress.val.emplace();
    auto& bbox = training_record.progress.val->bbox;
    bbox.available = true;
    bbox.ap = 0.625;
    bbox.ap50 = 0.875;
    bbox.ap75 = 0.375;
    bbox.average_recall = {0.125, std::nullopt, 0.75};
    bbox.detection_limits = {2, 20, 200};
    bbox.area_ap = {std::nullopt, 0.25, 0.5};
    bbox.area_ar = {0.3125, 0.5625, std::nullopt};
    bbox.confidence = {0.8125, 0.4375, 0.6875};
    bbox.confidence_threshold = 0.1875;
    auto& mask = training_record.progress.val->mask.emplace();
    mask.available = true;
    mask.ap = 0.0625;
    mask.ap50 = 0.9375;
    mask.ap75 = 0.15625;
    mask.average_recall = {0.21875, 0.28125, std::nullopt};
    mask.detection_limits = {3, 30, 300};
    mask.area_ap = {0.34375, std::nullopt, 0.40625};
    mask.area_ar = {std::nullopt, 0.46875, 0.53125};
    mask.confidence = {0.59375, 0.65625, 0.71875};
    mask.confidence_threshold = 0.78125;
    rfdetr::TrainingHistoryPage training_page;
    training_page.generation = 9;
    training_page.next_cursor = 123;
    training_page.records.push_back(training_record);
    if (!append_validation(training_record) || !append_validation(training_page)) return EXIT_FAILURE;
    for (const std::uint32_t count : {0U, static_cast<std::uint32_t>(rfdetr::kTrainingHistoryPageSize + 1U)}) {
        auto invalid_query = *mmltk::frameworks::serialization::reflected_value(rfdetr::TrainingHistoryQuery{});
        named_member(invalid_query, "count").storage = static_cast<std::uint64_t>(count);
        rfdetr::TrainingHistoryQuery rejected;
        if (mmltk::frameworks::serialization::decode_into(rejected, invalid_query)) return EXIT_FAILURE;
    }
    bool complete_record_surface = true;
    application_schema_detail::Variant<ServerRecord>::Visit([&]<class Alternative>() {
        complete_record_surface =
            complete_record_surface && std::ranges::any_of(records, [](const ServerRecord& record) { return std::holds_alternative<Alternative>(record); });
    });
    if (!complete_record_surface) return EXIT_FAILURE;
    std::ofstream output(arguments[1], std::ios::binary | std::ios::trunc);
    if (!output) return EXIT_FAILURE;
    for (const ServerRecord& record : records) {
        wire::ByteBuffer encoded;
        if (!encode_server_record(record, encoded) || encoded.size() > std::numeric_limits<std::uint32_t>::max()) return EXIT_FAILURE;
        const auto size = static_cast<std::uint32_t>(encoded.size());
        const std::array header{
            static_cast<unsigned char>(size >> 24U),
            static_cast<unsigned char>(size >> 16U),
            static_cast<unsigned char>(size >> 8U),
            static_cast<unsigned char>(size),
        };
        output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        output.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }
    return output ? EXIT_SUCCESS : EXIT_FAILURE;
}
