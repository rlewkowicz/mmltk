#include "src/controller/browser/client_record.h"
#include "src/controller/presentation/detail/workspace_surface_import_abi.h"
#include "src/controller/browser/application_materializer.h"
#include "src/controller/browser/application_schema.h"
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/contracts/model_selection.h"
#include "src/controller/services/file_dialog_catalog.h"
#include "src/controller/services/file_dialog_system.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/subsystems/explore/explore_system.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mmltk::controller::browser {
namespace {

struct ClientFixture final {
    std::string kind;
    wire::ByteBuffer bytes;
};

class FixtureFileDialogSystem final {
   public:
    using event_type = std::variant<mmltk::controller::FileDialogFailed>;
    [[= mmltk::controller::contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] mmltk::controller::FileDialogSnapshot Open(
        mmltk::controller::services::FileDialogOpen request) {
        const auto dialog = mmltk::controller::services::file_dialog_catalog().resolve(request);
        if (!dialog) throw mmltk::controller::contracts::InvalidIntentError("unknown fixture file dialog");
        opened = request;
        return {
            .generation = 1U,
            .active = true,
            .target = request.target,
        };
    }
    [[= mmltk::controller::contracts::reflection::Snapshot{8192U}]] [[nodiscard]] mmltk::controller::FileDialogSnapshot snapshot() const {
        return {};
    }
    mmltk::controller::services::FileDialogOpen opened{};
};

class FixtureSettingsSystem final {
   public:
    using event_type = std::variant<mmltk::controller::SettingsChanged>;
    [[= mmltk::controller::contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] mmltk::controller::contracts::SettingsUiState
        Update(mmltk::controller::contracts::SettingsUpdateRequest request) {
        state = mmltk::controller::contracts::default_gui_settings_state();
        const auto applied = mmltk::controller::contracts::apply_gui_settings_values(
            state, std::span<const mmltk::controller::contracts::SettingsValueUpdate>{request.updates});
        if (!applied) throw mmltk::controller::contracts::InvalidIntentError("invalid fixture settings update");
        latest = std::move(request);
        return snapshot();
    }
    [[= mmltk::controller::contracts::reflection::Snapshot{mmltk::controller::contracts::kSettingsUiStateByteBudget}]]
        [[nodiscard]] mmltk::controller::contracts::SettingsUiState snapshot() const {
        return {.revision = 1U, .settings_state = state};
    }
    mmltk::controller::contracts::GuiSettingsState state{};
    mmltk::controller::contracts::SettingsUpdateRequest latest{};
};

class FixtureExploreSystem final {
   public:
    using event_type = std::variant<mmltk::controller::ExploreChanged>;
    [[= mmltk::controller::contracts::reflection::direct::InteractionEndpoint{}]] void UpdateViewport(
        mmltk::controller::ExploreViewportUpdate request) {
        latest = request.viewport;
    }
    [[= mmltk::controller::contracts::reflection::Snapshot{64U *
                                                           1024U}]] [[nodiscard]] mmltk::controller::ExploreSnapshot snapshot() const {
        return {.viewport = latest};
    }
    mmltk::controller::ExploreViewport latest{};
};

class FixtureAnnotationSystem final {
   public:
    using event_type = std::variant<mmltk::controller::AnnotationChanged>;
    [[= mmltk::controller::contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] mmltk::controller::AnnotationSnapshot Edit(
        mmltk::controller::AnnotationEditRequest request) {
        if (const auto* category = std::get_if<mmltk::controller::AnnotationCategoryEdit>(&request.edit.value);
            category != nullptr && !category->category.valid())
            throw mmltk::controller::contracts::InvalidIntentError("invalid fixture annotation category");
        alternatives.push_back(request.edit.value.index());
        return {};
    }
    [[= mmltk::controller::contracts::reflection::Snapshot{256U *
                                                           1024U}]] [[nodiscard]] mmltk::controller::AnnotationSnapshot snapshot() const {
        return {};
    }
    [[= mmltk::controller::contracts::reflection::direct::InteractionEndpoint{}]] void Input(mmltk::controller::WorkspaceMouse batch) {
        input = std::move(batch);
    }
    mmltk::controller::WorkspaceMouse input;
    std::vector<std::size_t> alternatives;
};

struct FixtureApplicationSystems final {
    FixtureSettingsSystem* settings = nullptr;
    FixtureFileDialogSystem* file_dialog = nullptr;
    FixtureExploreSystem* explore = nullptr;
    FixtureAnnotationSystem* annotation = nullptr;
};

[[nodiscard]] std::vector<ClientFixture> protocol_client_fixtures() {
    std::ifstream input(MMLTK_PROTOCOL_V17_CLIENT_FIXTURE_PATH);
    REQUIRE(input.good());
    const auto nibble = [](const char value) -> unsigned char {
        if (value >= '0' && value <= '9') return static_cast<unsigned char>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<unsigned char>(value - 'a' + 10);
        throw std::invalid_argument("invalid protocol fixture hex");
    };
    std::vector<ClientFixture> fixtures;
    for (std::string kind, hex; input >> kind >> hex;) {
        REQUIRE(hex.size() % 2U == 0U);
        wire::ByteBuffer bytes;
        bytes.reserve(hex.size() / 2U);
        for (std::size_t index = 0U; index < hex.size(); index += 2U)
            bytes.push_back(static_cast<std::byte>((nibble(hex[index]) << 4U) | nibble(hex[index + 1U])));
        fixtures.push_back({.kind = std::move(kind), .bytes = std::move(bytes)});
    }
    return fixtures;
}

[[nodiscard]] const ClientFixture& fixture_named(const std::vector<ClientFixture>& fixtures, const std::string_view name) {
    const auto found = std::ranges::find(fixtures, name, &ClientFixture::kind);
    REQUIRE(found != fixtures.end());
    return *found;
}

[[nodiscard]] Intent decode_intent_fixture(const ClientFixture& fixture) {
    const auto record = decode_client_record(wire::ByteSegments{.first = fixture.bytes, .second = {}});
    REQUIRE(record);
    REQUIRE(std::holds_alternative<Intent>(*record));
    Intent intent = std::get<Intent>(std::move(*record));
    CHECK(intent.protocol_version == kBrowserProtocolVersion);
    return intent;
}

template <class Exception>
[[nodiscard]] ApplicationErrorRecord map_exception(Exception exception) {
    try {
        throw std::move(exception);
    } catch (...) { return map_current_exception(); }
}

TEST_CASE("browser client records are one complete canonical CBOR item", "[controller][browser][protocol]") {
    const ClientRecord source = Intent{
        .correlation = 7U,
        .endpoint_id = 11U,
        .fields = {{.field_id = 13U, .value = wire::Value(std::uint64_t{17U})}},
    };
    wire::ByteBuffer encoded;
    REQUIRE(encode_client_record(source, encoded));
    const auto decoded = decode_client_record(wire::ByteSegments{.first = encoded, .second = {}});
    REQUIRE(decoded);
    CHECK(*decoded == source);

    encoded.push_back(std::byte{0xf6});
    CHECK_FALSE(decode_client_record(wire::ByteSegments{.first = encoded, .second = {}}));
}

TEST_CASE("integration control retains typed direction and sequence validation", "[controller][browser][protocol]") {
    using Kind = mmltk::controller::contracts::IntegrationControlKind;
    wire::ByteBuffer encoded;
    mmltk::controller::contracts::visit_integration_commands([&]<auto kind, auto policy>(auto) {
        const IntegrationControl source{.receipt = {.kind = kind,
                                                    .sequence = 3U,
                                                    .progress = policy.server ? 0U : 5U,
                                                    .failureline = kind == Kind::Failed ? 123U : 0U,
                                                    .read_generation = policy.read_generation ? 7U : 0U,
                                                    .compiled_index = 0U}};
        if constexpr (policy.server) {
            REQUIRE(encode_server_record(ServerRecord{source}, encoded));
            const auto decoded = decode_server_record({.first = encoded, .second = {}});
            REQUIRE(decoded);
            CHECK(std::get<IntegrationControl>(*decoded) == source);
            CHECK_FALSE(decode_client_record({.first = encoded, .second = {}}));
            CHECK_FALSE(encode_client_record(ClientRecord{source}, encoded));
        } else {
            REQUIRE(encode_client_record(ClientRecord{source}, encoded));
            const auto decoded = decode_client_record({.first = encoded, .second = {}});
            REQUIRE(decoded);
            CHECK(std::get<IntegrationControl>(*decoded) == source);
            CHECK_FALSE(decode_server_record({.first = encoded, .second = {}}));
            CHECK_FALSE(encode_server_record(ServerRecord{source}, encoded));
        }
        auto invalid = source.receipt;
        invalid.read_generation = policy.read_generation ? 0U : 7U;
        CHECK_FALSE(mmltk::controller::contracts::integration_receipt_valid(invalid));
        invalid = source.receipt;
        invalid.compiled_index = 9U;
        CHECK(mmltk::controller::contracts::integration_receipt_valid(invalid) == policy.compiled_index);
    });
    CHECK_FALSE(encode_client_record(ClientRecord{IntegrationControl{.receipt = {.kind = Kind::Settled}}}, encoded));
    CHECK_FALSE(encode_server_record(ServerRecord{IntegrationControl{.receipt = {.kind = Kind::Advance}}}, encoded));
}

TEST_CASE("integration failure text has a bounded failed-only canonical payload", "[controller][browser][protocol]") {
    using namespace mmltk::controller::contracts;
    for (const auto size : {0U, 1U, static_cast<unsigned>(kIntegrationFailureMaxBytes)}) {
        IntegrationControl source{.receipt = {.kind = IntegrationControlKind::Failed,
                                              .sequence = 3U,
                                              .progress = 7U,
                                              .failureline = 123U,
                                              .failure = std::string(size, 'x')}};
        wire::ByteBuffer encoded;
        REQUIRE(encode_client_record(ClientRecord{source}, encoded));
        const auto decoded = decode_client_record({.first = encoded, .second = {}});
        REQUIRE(decoded);
        CHECK(std::get<IntegrationControl>(*decoded) == source);
        CHECK_FALSE(decode_server_record({.first = encoded, .second = {}}));
        source.receipt.failure.assign(kIntegrationFailureMaxBytes + 1U, 'x');
        CHECK_FALSE(encode_client_record(ClientRecord{source}, encoded));
    }
    visit_integration_commands([&]<auto kind, auto policy>(auto) {
        if constexpr (kind != IntegrationControlKind::Failed) {
            const IntegrationControl source{.receipt = {.kind = kind,
                                                        .sequence = 3U,
                                                        .read_generation = policy.read_generation ? 1U : 0U,
                                                        .failure = "Protocol: invalid frame"}};
            CHECK_FALSE(integration_receipt_valid(source.receipt));
        }
    });
}

TEST_CASE("Rust Protocol-17 client fixtures are accepted by native codec", "[controller][browser][protocol][interop]") {
    STATIC_REQUIRE(kBrowserProtocolVersion == 17U);
    const auto fixtures = protocol_client_fixtures();
    constexpr auto annotation_alternatives = std::variant_size_v<decltype(AnnotationEdit::value)>;
    REQUIRE(std::ranges::count_if(fixtures, [](const auto& fixture) { return !fixture.kind.starts_with("IntegrationControl:"); }) ==
            6U + annotation_alternatives);
    for (const auto* name : {"IntegrationControl:capacity", "IntegrationControl:visible-arm", "IntegrationControl:visible-release"}) {
        const auto decoded = decode_client_record({.first = fixture_named(fixtures, name).bytes, .second = {}});
        REQUIRE(decoded);
        const auto* receipt = std::get_if<IntegrationControl>(&*decoded);
        REQUIRE(receipt);
        CHECK(receipt->receipt.compiled_index == 0U);
        CHECK(receipt->receipt.read_generation == (std::string_view{name}.ends_with("release") ? 7U : 0U));
    }
    const auto& control_fixture = fixture_named(fixtures, "IntegrationControl");
    const auto control = decode_client_record(wire::ByteSegments{.first = control_fixture.bytes, .second = {}});
    REQUIRE(control);
    const auto* receipt = std::get_if<IntegrationControl>(&*control);
    REQUIRE(receipt);
    CHECK(receipt->receipt.kind == mmltk::controller::contracts::IntegrationControlKind::Settled);
    CHECK(receipt->receipt.sequence == 1U);
    const auto& dialog_fixture = fixture_named(fixtures, "Intent:file_dialog.Open");
    const auto& model_dialog_fixture = fixture_named(fixtures, "Intent:file_dialog.Open.model_artifact");
    const auto& settings_fixture = fixture_named(fixtures, "Intent:settings.Update");
    const auto& interaction_fixture = fixture_named(fixtures, "Interaction:explore.UpdateViewport");
    const auto& batch_fixture = fixture_named(fixtures, "Interaction:annotation.Input");

    const Intent intent = decode_intent_fixture(dialog_fixture);
    CHECK(intent.correlation == 17U);
    CHECK(intent.endpoint_id == application_stable_id("file_dialog", "Open"));
    REQUIRE(intent.fields.size() == 1U);
    FixtureFileDialogSystem file_dialog;
    FixtureSettingsSystem settings;
    FixtureExploreSystem explore;
    FixtureAnnotationSystem annotation;
    FixtureApplicationSystems systems{
        .settings = &settings,
        .file_dialog = &file_dialog,
        .explore = &explore,
        .annotation = &annotation,
    };
    const auto reply = dispatch_intent(systems, intent);
    CHECK(reply.result.has_value());
    CHECK_FALSE(reply.error.has_value());
    REQUIRE(mmltk::controller::services::file_dialog_stable_id(file_dialog.opened.target) != 0U);
    CHECK(std::holds_alternative<mmltk::controller::services::SettingsFieldTarget>(file_dialog.opened.target.value));
    REQUIRE(mmltk::controller::services::file_dialog_catalog().resolve(file_dialog.opened));

    const Intent model_intent = decode_intent_fixture(model_dialog_fixture);
    CHECK(model_intent.correlation == 23U);
    CHECK(model_intent.endpoint_id == application_stable_id("file_dialog", "Open"));
    REQUIRE(model_intent.fields.size() == 1U);
    const auto model_reply = dispatch_intent(systems, model_intent);
    REQUIRE(model_reply.result.has_value());
    CHECK_FALSE(model_reply.error.has_value());
    REQUIRE(std::holds_alternative<mmltk::controller::services::ModelArtifactTarget>(file_dialog.opened.target.value));
    const auto& model_target = std::get<mmltk::controller::services::ModelArtifactTarget>(file_dialog.opened.target.value);
    const auto& expected_model = mmltk::controller::contracts::kModelSelectionCompatibility.front();
    CHECK(model_target.stable_id == application_settings_field_stable_id(expected_model.artifact_field_path));
    CHECK(model_target.workflow == expected_model.workflow);
    CHECK(model_target.input == expected_model.input);
    REQUIRE(mmltk::controller::services::file_dialog_catalog().resolve(file_dialog.opened));

    const Intent settings_intent = decode_intent_fixture(settings_fixture);
    CHECK(settings_intent.correlation == 19U);
    CHECK(settings_intent.endpoint_id == application_stable_id("settings", "Update"));
    const auto settings_reply = dispatch_intent(systems, settings_intent);
    REQUIRE(settings_reply.result.has_value());
    CHECK_FALSE(settings_reply.error.has_value());
    REQUIRE(settings.latest.updates.size() == 1U);
    // CLEANUP-IGNORE: Settings and file-dialog fixtures prove separate generated endpoints and native dispatch paths.
    CHECK(mmltk::controller::contracts::gui_settings_valid(settings.state));

    const auto interaction_record = decode_client_record(wire::ByteSegments{.first = interaction_fixture.bytes, .second = {}});
    REQUIRE(interaction_record);
    REQUIRE(std::holds_alternative<Interaction>(*interaction_record));
    const Interaction& interaction = std::get<Interaction>(*interaction_record);
    CHECK(interaction.endpoint_id == application_stable_id("explore", "UpdateViewport"));
    ExploreViewportUpdate decoded_interaction{};
    REQUIRE(mmltk::frameworks::serialization::decode_compact_into(
        decoded_interaction, {.first = interaction.value},
        {.max_bytes = kMaxIntentValueBytes, .max_items = kMaxIntentValueItems, .max_depth = kMaxIntentValueDepth}));
    CHECK(decoded_interaction.viewport.first_row == 0U);
    CHECK(decoded_interaction.viewport.row_count == 1U);
    CHECK(decoded_interaction.viewport.columns == 1U);
    CHECK(decoded_interaction.viewport.valid());
    const auto batch_view = decode_interaction_view(batch_fixture.bytes);
    REQUIRE(batch_view);
    CHECK(dispatch_interaction(systems, *batch_view).disposition == InteractionDispatchDisposition::Accepted);
    REQUIRE(annotation.input.point);
    CHECK(annotation.input.point->x == 1.25F);
    CHECK(annotation.input.point->y == 2.5F);
    CHECK(annotation.input.document_epoch == 1U);
    CHECK(annotation.input.kind == WorkspaceMouseKind::Motion);
    CHECK(annotation.input.wheel_unit == WorkspaceWheelUnit::Pixels);
    const auto mouse_owned = decode_client_record({.first = batch_fixture.bytes});
    REQUIRE(mouse_owned);
    auto malformed_mouse = std::get<Interaction>(*mouse_owned);
    malformed_mouse.value.push_back(std::byte{0xf6});
    CHECK(dispatch_interaction(systems, malformed_mouse).disposition == InteractionDispatchDisposition::ProtocolInvalid);
    // The numeric envelope accepts every segmented boundary, rejects obsolete
    // named records, and never tolerates truncation, unknown opcodes or trailing bytes.
    wire::ByteBuffer projected;
    REQUIRE(encode_client_record(interaction, projected));
    for (std::size_t split = 0U; split <= projected.size(); ++split) {
        const auto bytes = std::span<const std::byte>(projected);
        REQUIRE(decode_client_record({.first = bytes.first(split), .second = bytes.subspan(split)}));
    }
    for (std::size_t size = 0U; size < projected.size(); ++size)
        CHECK_FALSE(decode_interaction_view(std::span<const std::byte>(projected).first(size)));
    auto trailing = projected;
    trailing.push_back(std::byte{0xf6});
    CHECK_FALSE(decode_interaction_view(trailing));
    CHECK_FALSE(decode_client_record({.first = trailing}));
    auto unknown = projected;
    unknown[1] = std::byte{0x17};
    CHECK_FALSE(decode_interaction_view(unknown));
    CHECK_FALSE(decode_client_record({.first = unknown}));
    REQUIRE(mmltk::frameworks::serialization::encode(
        ClientRecord{interaction}, projected,
        {.max_bytes = kMaxRecordWireBytes, .max_items = kMaxIntentValueItems, .max_depth = kMaxIntentValueDepth}));
    CHECK_FALSE(decode_interaction_view(projected));
    CHECK_FALSE(decode_client_record({.first = projected}));
    const auto accepted_interaction = dispatch_interaction(systems, interaction);
    CHECK(accepted_interaction.disposition == InteractionDispatchDisposition::Accepted);
    CHECK_FALSE(accepted_interaction.error.has_value());
    CHECK(explore.latest.extent.width == 64U);
    CHECK(explore.latest.extent.height == 64U);

    const auto malformed_interaction =
        dispatch_interaction(systems, Interaction{.endpoint_id = interaction.endpoint_id, .value = {std::byte{0xf6}}});
    CHECK(malformed_interaction.disposition == InteractionDispatchDisposition::ProtocolInvalid);

    systems.explore = nullptr;
    const auto unavailable_interaction = dispatch_interaction(systems, interaction);
    CHECK(unavailable_interaction.disposition == InteractionDispatchDisposition::ApplicationRejected);
    REQUIRE(unavailable_interaction.error.has_value());
    CHECK(unavailable_interaction.error->category == mmltk::controller::contracts::ApplicationErrorCategory::Unavailable);
    systems.explore = &explore;

    for (std::size_t alternative = 0U; alternative != annotation_alternatives; ++alternative) {
        const std::string name = "Intent:annotation.Edit." + std::to_string(alternative);
        const auto& fixture = fixture_named(fixtures, name);
        const auto record = decode_client_record(wire::ByteSegments{.first = fixture.bytes, .second = {}});
        REQUIRE(record);
        REQUIRE(std::holds_alternative<Intent>(*record));
        const auto& edit = std::get<Intent>(*record);
        CHECK(edit.correlation == 100U + alternative);
        CHECK(edit.endpoint_id == application_stable_id("annotation", "Edit"));
        const auto edit_reply = dispatch_intent(systems, edit);
        REQUIRE(edit_reply.result.has_value());
        CHECK_FALSE(edit_reply.error.has_value());
    }
    REQUIRE(annotation.alternatives.size() == annotation_alternatives);
    for (std::size_t alternative = 0U; alternative != annotation.alternatives.size(); ++alternative)
        // CLEANUP-IGNORE: Exhaustive annotation alternatives and renderer observations are independent protocol
        // evidence.
        CHECK(annotation.alternatives[alternative] == alternative);
}

TEST_CASE("browser server records preserve reply and event error vocabulary", "[controller][browser][protocol]") {
    const ServerRecord failed = IntentReply{
        .correlation = 23U,
        .result = {},
        .error =
            ApplicationErrorRecord{
                .category = mmltk::controller::contracts::ApplicationErrorCategory::Busy,
                .detail = "operation already active",
            },
    };
    wire::ByteBuffer encoded;
    REQUIRE(encode_server_record(failed, encoded));
    const auto decoded = decode_server_record(wire::ByteSegments{.first = encoded, .second = {}});
    REQUIRE(decoded);
    CHECK(*decoded == failed);

    for (const auto delivery : {mmltk::controller::contracts::reflection::EventDelivery::Transient,
                                mmltk::controller::contracts::reflection::EventDelivery::Critical,
                                mmltk::controller::contracts::reflection::EventDelivery::LatestState}) {
        const ServerRecord event = SystemEvent{
            .system_id = 29U,
            .event_id = 31U,
            .delivery = delivery,
            .state_revision = 3U,
            .value = wire::Value(wire::Value::Object{{"progress", wire::Value(std::uint64_t{3U})}}),
        };
        REQUIRE(encode_server_record(event, encoded));
        const auto roundtrip = decode_server_record(wire::ByteSegments{.first = encoded, .second = {}});
        REQUIRE(roundtrip);
        CHECK(*roundtrip == event);
    }
}

TEST_CASE("output records admit bounded scene collections and complete bootstrap payloads", "[controller][browser][protocol][limits]") {
    namespace cbor = mmltk::frameworks::serialization;
    wire::ByteBuffer encoded;
    wire::Value::Array objects(4096U, wire::Value(std::uint64_t{1U}));
    const ServerRecord event = SystemEvent{.system_id = 1U,
                                           .event_id = 2U,
                                           .delivery = mmltk::controller::contracts::reflection::EventDelivery::LatestState,
                                           .state_revision = 1U,
                                           .value = wire::Value(std::move(objects))};
    REQUIRE(encode_server_record(event, encoded));
    const auto event_payload_bytes =
        cbor::measure(std::get<SystemEvent>(event).value,
                      {.max_bytes = kMaxOutputValueBytes, .max_items = kMaxOutputValueItems, .max_depth = kMaxIntentValueDepth});
    REQUIRE(event_payload_bytes);
    CHECK(encoded.size() <= cbor::reflected_structural_cbor_bytes<std::variant<SystemEvent>>(*event_payload_bytes));
    const auto decoded = decode_server_record(wire::ByteSegments{.first = encoded, .second = {}});
    REQUIRE(decoded);
    CHECK(*decoded == event);
    const wire::Value payload(std::string(kMaxOutputValueBytes, 'x'));
    const ServerRecord bootstrap = Bootstrap{.schema_fingerprint = {11U, 13U},
                                             .input_epoch = 1U,
                                             .snapshots = {{.system_id = 1U, .value = payload}, {.system_id = 2U, .value = payload}}};
    REQUIRE(encode_server_record(bootstrap, encoded));
    CHECK(encoded.size() > kMaxOutputValueBytes * 2U);
    CHECK(encoded.size() <= kMaxRecordWireBytes);
    const auto snapshot_bytes =
        cbor::measure(payload, {.max_bytes = kMaxRecordWireBytes, .max_items = kMaxOutputValueItems, .max_depth = kMaxIntentValueDepth});
    REQUIRE(snapshot_bytes);
    CHECK(encoded.size() <= cbor::reflected_structural_cbor_bytes<std::variant<Bootstrap>>(2U * *snapshot_bytes));
    REQUIRE(decode_server_record(wire::ByteSegments{.first = encoded, .second = {}}));
    const ServerRecord oversized =
        SystemEvent{.system_id = 1U, .event_id = 2U, .value = wire::Value(std::string(kMaxOutputValueBytes + 1U, 'x'))};
    CHECK_FALSE(encode_server_record(oversized, encoded));
    const ClientRecord oversized_input = Interaction{.endpoint_id = 2U, .value = wire::ByteBuffer(kMaxIntentValueBytes + 1U)};
    CHECK_FALSE(encode_client_record(oversized_input, encoded));
}

TEST_CASE("materialized Annotation categories retain native fixed text validation") {
    FixtureAnnotationSystem annotation;
    FixtureApplicationSystems systems{.annotation = &annotation};
    const auto intent = [](mmltk::controller::contracts::AnnotationText category) {
        const mmltk::controller::AnnotationEditRequest request{
            .edit = {.value = mmltk::controller::AnnotationCategoryEdit{std::move(category)}},
        };
        auto encoded = mmltk::frameworks::serialization::reflected_value(request);
        REQUIRE(encoded);
        auto object = std::get<wire::Value::Object>(std::move(encoded->storage));
        REQUIRE(object.size() == 1U);
        return Intent{
            .correlation = 1U,
            .endpoint_id = application_stable_id("annotation", "Edit"),
            .fields = {{
                .field_id = application_field_stable_id(application_stable_id("annotation", "Edit"), "edit"),
                .value = std::move(object.front().second),
            }},
        };
    };
    const auto rejected = [&systems, &intent](mmltk::controller::contracts::AnnotationText category) {
        const auto reply = dispatch_intent(systems, intent(std::move(category)));
        REQUIRE(reply.error);
        CHECK(reply.error->category == mmltk::controller::contracts::ApplicationErrorCategory::InvalidIntent);
    };

    rejected({});
    auto over_capacity = mmltk::controller::contracts::AnnotationText::From("x");
    over_capacity.size = static_cast<std::uint8_t>(over_capacity.bytes.size() + 1U);
    rejected(over_capacity);
    auto control = mmltk::controller::contracts::AnnotationText{};
    control.bytes[0] = '\n';
    control.size = 1U;
    rejected(control);
    auto nonzero_tail = mmltk::controller::contracts::AnnotationText::From("x");
    nonzero_tail.bytes[1] = 'y';
    rejected(nonzero_tail);

    const auto accepted = dispatch_intent(systems, intent(mmltk::controller::contracts::AnnotationText::From("category")));
    CHECK_FALSE(accepted.error);
    REQUIRE(accepted.result);
}

TEST_CASE("Bootstrap uses the compact protocol-17 fingerprint and bounded snapshots", "[controller][browser][protocol][limits]") {
    STATIC_REQUIRE(kMaxRecordWireBytes >=
                   mmltk::frameworks::serialization::reflected_structural_cbor_bytes<std::variant<SystemEvent>>(kMaxOutputValueBytes));
    STATIC_REQUIRE(mmltk::frameworks::serialization::reflected_cbor_member_count<SystemEvent>() == 6U);
    STATIC_REQUIRE(mmltk::frameworks::serialization::reflected_structural_cbor_bytes<std::variant<Bootstrap>>() == 954U);
    STATIC_REQUIRE(kMaxOutputValueBytes == 8388608U);
    STATIC_REQUIRE(kMaxRecordWireBytes == 33554432U);
    STATIC_REQUIRE(kMaxIntentValueBytes == 65536U);
    STATIC_REQUIRE(kMaxIntentValueItems == 1024U);
    STATIC_REQUIRE(kMaxIntentValueDepth == 64U);
    STATIC_REQUIRE(kMaxSnapshotCount == 32U);
    STATIC_REQUIRE(kMaxIntentFields == 64U);
    STATIC_REQUIRE(kMaxErrorDetailBytes == 512U);

    const ServerRecord bootstrap = Bootstrap{
        .schema_fingerprint = {11U, 13U},
        .input_epoch = 1U,
        .snapshots = {},
    };
    wire::ByteBuffer encoded;
    REQUIRE(encode_server_record(bootstrap, encoded));
    REQUIRE(decode_server_record(wire::ByteSegments{.first = encoded, .second = {}}));

    auto zero_epoch = std::get<Bootstrap>(bootstrap);
    zero_epoch.input_epoch = 0U;
    CHECK_FALSE(encode_server_record(ServerRecord{zero_epoch}, encoded));
    for (const ServerRecord& record : std::array<ServerRecord, 2U>{
             InteractionRejected{.endpoint_id = 7U, .error = {.detail = "input unavailable"}},
             InteractionRejected{
                 .endpoint_id = 1U,
                 .error = {.category = mmltk::controller::contracts::ApplicationErrorCategory::Unavailable, .detail = "unavailable"}}}) {
        REQUIRE(encode_server_record(record, encoded));
        const auto decoded = decode_server_record(wire::ByteSegments{.first = encoded, .second = {}});
        REQUIRE(decoded);
        CHECK(*decoded == record);
    }
    std::vector<SystemSnapshot> too_many_snapshots;
    too_many_snapshots.reserve(kMaxSnapshotCount + 1U);
    for (std::size_t index = 0U; index <= kMaxSnapshotCount; ++index)
        too_many_snapshots.push_back({.system_id = index + 1U, .value = wire::Value{}});
    CHECK_FALSE(encode_server_record(
        ServerRecord{Bootstrap{.schema_fingerprint = {11U, 13U}, .input_epoch = 1U, .snapshots = std::move(too_many_snapshots)}}, encoded));

    CHECK_FALSE(encode_server_record(ServerRecord{Bootstrap{
                                         .schema_fingerprint = {11U, 13U},
                                         .input_epoch = 1U,
                                         .snapshots = {{
                                             .system_id = 1U,
                                             .value = wire::Value(std::string(kMaxOutputValueBytes + 1U, 'x')),
                                         }},
                                     }},
                                     encoded));

    REQUIRE(encode_server_record(bootstrap, encoded));
    encoded.push_back(std::byte{0xf6});
    CHECK_FALSE(decode_server_record(wire::ByteSegments{.first = encoded, .second = {}}));
    const wire::ByteBuffer malformed{std::byte{0xbf}};
    CHECK_FALSE(decode_server_record(wire::ByteSegments{.first = malformed, .second = {}}));
}

TEST_CASE("browser records reject duplicate identities and invalid renderer observations", "[controller][browser][protocol]") {
    wire::ByteBuffer encoded;
    CHECK_FALSE(encode_client_record(ClientRecord{Intent{
                                         .correlation = 1U,
                                         .endpoint_id = 2U,
                                         .fields =
                                             {
                                                 {.field_id = 3U, .value = wire::Value{}},
                                                 {.field_id = 3U, .value = wire::Value{}},
                                             },
                                     }},
                                     encoded));
}

TEST_CASE("settings leaf traversal retains every reflected constraint dimension", "[controller][browser][schema][settings]") {
    std::size_t leaves = 0U;
    bool observed_byte_bound = false;
    bool observed_item_bound = false;
    ApplicationSchema<FixtureApplicationSystems>::template VisitSettingsLeaves<mmltk::controller::contracts::GuiSettingsState>(
        [&]<class, class, class>(const ApplicationSettingsLeafFact& fact) {
            ++leaves;
            observed_byte_bound = observed_byte_bound || fact.constraint.minimum_bytes != 0U || fact.constraint.maximum_bytes != 0U;
            observed_item_bound = observed_item_bound || fact.constraint.maximum_items != 0U;
            CHECK((fact.constraint.maximum_bytes == 0U || fact.constraint.minimum_bytes <= fact.constraint.maximum_bytes));
            CHECK((!fact.constraint.has_minimum || !fact.constraint.has_maximum || fact.constraint.minimum <= fact.constraint.maximum));
        });
    CHECK(leaves != 0U);
    CHECK(observed_byte_bound);
    CHECK(observed_item_bound);
}

TEST_CASE("exception mapping preserves the common bounded error vocabulary", "[controller][browser][protocol]") {
    using mmltk::controller::contracts::ApplicationErrorCategory;
    const auto invalid = map_exception(mmltk::controller::contracts::InvalidIntentError("invalid"));
    const auto busy = map_exception(mmltk::controller::contracts::BusyError("busy"));
    const auto unavailable = map_exception(mmltk::controller::contracts::UnavailableError("unavailable"));
    const auto failed = map_exception(std::runtime_error("failed"));
    CHECK(invalid.category == ApplicationErrorCategory::InvalidIntent);
    CHECK(busy.category == ApplicationErrorCategory::Busy);
    CHECK(unavailable.category == ApplicationErrorCategory::Unavailable);
    CHECK(failed.category == ApplicationErrorCategory::Failed);

    std::string oversized(kMaxErrorDetailBytes * 2U, 'x');
    oversized[1U] = '\n';
    oversized[2U] = static_cast<char>(0xff);
    const auto bounded = map_exception(mmltk::controller::contracts::BusyError(std::move(oversized)));
    CHECK(bounded.category == ApplicationErrorCategory::Busy);
    CHECK(bounded.detail.size() == kMaxErrorDetailBytes);
    CHECK(bounded.detail[1U] == '?');
    CHECK(bounded.detail[2U] == '?');

    wire::ByteBuffer encoded;
    InteractionRejected rejected{.endpoint_id = 1U, .error = {.detail = std::string(kMaxErrorDetailBytes, 'r')}};
    REQUIRE(encode_server_record(ServerRecord{rejected}, encoded));
    REQUIRE(decode_server_record({.first = encoded}));
    rejected.error.detail.push_back('r');
    CHECK_FALSE(encode_server_record(ServerRecord{rejected}, encoded));
    REQUIRE(encode_server_record(ServerRecord{IntentReply{.correlation = 1U, .result = {}, .error = bounded}}, encoded));
    CHECK_FALSE(encode_server_record(ServerRecord{IntentReply{
                                         .correlation = 1U,
                                         .result = {},
                                         .error =
                                             ApplicationErrorRecord{
                                                 .category = ApplicationErrorCategory::Failed,
                                                 .detail = std::string(kMaxErrorDetailBytes + 1U, 'x'),
                                             },
                                     }},
                                     encoded));
}

}  // namespace
}  // namespace mmltk::controller::browser

TEST_CASE("Native graphics records reject unknown raw opcodes and malformed descriptor counts", "[browser][workspace][protocol]") {
    namespace abi = mmltk::controller::presentation::detail::workspace_surface_import;
    abi::Record record{.id_high = 1U,
                       .width = 4U,
                       .height = 3U,
                       .stride = 32U,
                       .size = 96U,
                       .descriptors = abi::kAllocateDescriptorCount,
                       .arena_high = 2U,
                       .allocation_identity = 3U,
                       .device_incarnation = 4U,
                       .alignment = 32U,
                       .device_uuid = {1U},
                       .memory_type_bits = 1U};
    REQUIRE(abi::valid(record));
    record.opcode = static_cast<abi::Opcode>(0xffff'ffffU);
    CHECK_FALSE(abi::valid(record));
    record.opcode = abi::Opcode::Allocate;
    --record.descriptors;
    CHECK_FALSE(abi::valid(record));
    record.descriptors = abi::kAllocateDescriptorCount;
    record.size = 95U;
    CHECK_FALSE(abi::valid(record));
    auto packet = abi::layout_packet();
    REQUIRE(abi::valid(packet));
    ++packet.presentation_revision_offset;
    CHECK_FALSE(abi::valid(packet));
}

TEST_CASE("Graphics arena negotiation and source completion use independent record shapes", "[browser][workspace][protocol]") {
    namespace abi = mmltk::controller::presentation::detail::workspace_surface_import;
    abi::Record arena{.opcode = abi::Opcode::Arena, .id_high = 1U, .width = 4U, .height = 3U};
    REQUIRE(abi::valid(arena));
    arena.allocation_identity = 1U;
    CHECK_FALSE(abi::valid(arena));
    abi::Record layout{.opcode = abi::Opcode::ArenaReady,
                       .id_high = 1U,
                       .width = 4U,
                       .height = 3U,
                       .stride = 32U,
                       .size = 128U,
                       .device_incarnation = 2U,
                       .offset = 32U,
                       .alignment = 32U,
                       .device_uuid = {1U},
                       .memory_type_bits = 1U};
    REQUIRE(abi::valid(layout));
    SECTION("direct sampling is a negotiated binary capability") {
        layout.direct_sampling = 1U;
        REQUIRE(abi::valid(layout));
        layout.direct_sampling = 2U;
        CHECK_FALSE(abi::valid(layout));
    }
    SECTION("subresource offset is included in allocation bounds") {
        ++layout.offset;
        CHECK_FALSE(abi::valid(layout));
    }
    SECTION("unknown physical UUID is rejected") {
        layout.device_uuid[0] = 0U;
        CHECK_FALSE(abi::valid(layout));
    }
    SECTION("alignment is a power of two") {
        layout.alignment = 3U;
        CHECK_FALSE(abi::valid(layout));
    }
    abi::Record completed{
        .opcode = abi::Opcode::ReadSettled, .id_high = 3U, .stride = 7U, .size = 8U, .presentation_revision = 9U, .offset = 1U};
    REQUIRE(abi::valid(completed));
    SECTION("exact acquisition and submitted release use the same physical identity") {
        completed.opcode = GENERATE(abi::Opcode::Acquired, abi::Opcode::ReleaseSubmitted);
        REQUIRE(abi::valid(completed));
        CHECK(abi::descriptor_count(completed.opcode) == 0U);
        auto malformed = completed;
        malformed.descriptors = 1U;
        CHECK_FALSE(abi::valid(malformed));
        malformed = completed;
        malformed.presentation_revision = 0U;
        CHECK_FALSE(abi::valid(malformed));
        malformed = completed;
        malformed.stride = malformed.size = 0U;
        CHECK_FALSE(abi::valid(malformed));
        malformed = completed;
        malformed.abi_version = abi::kAbiVersion - 1U;
        CHECK_FALSE(abi::valid(malformed));
        completed.offset = 0U;
        CHECK_FALSE(abi::valid(completed));
    }
    SECTION("source completion requires a physical transfer") {
        completed.offset = 0U;
        CHECK_FALSE(abi::valid(completed));
    }
    SECTION("source completion has no sample slot code") {
        completed.code = 1U;
        CHECK_FALSE(abi::valid(completed));
    }
}

TEST_CASE("compact workspace mouse records preserve fractional coordinates and wheel units", "[controller][browser][protocol]") {
    using namespace mmltk::controller;
    using namespace mmltk::controller::browser;
    namespace cbor = mmltk::frameworks::serialization;
    constexpr wire::Limits limits{.max_bytes = kMaxIntentValueBytes, .max_items = kMaxIntentValueItems, .max_depth = kMaxIntentValueDepth};
    const WorkspaceMouse source{.source = PresentationSourceKind::Annotation,
                                .peer_epoch = 9U,
                                .document_epoch = 3U,
                                .kind = WorkspaceMouseKind::Wheel,
                                .point = WorkspacePoint{1.25F, 2.5F},
                                .button = WorkspaceMouseButton::Other,
                                .other_button = 127U,
                                .click_count = 2U,
                                .modifiers = 15U,
                                .wheel_unit = WorkspaceWheelUnit::Pixels,
                                .wheel = {0.125F, -0.25F}};
    wire::ByteBuffer bytes(cbor::compact_maximum_cbor_bytes<WorkspaceMouse>());
    cbor::FixedCborEncoder encoder(bytes);
    REQUIRE(cbor::encode_compact(encoder, source));
    bytes.resize(encoder.size());
    for (std::size_t split = 0U; split <= bytes.size(); ++split) {
        WorkspaceMouse decoded;
        const auto span = std::span<const std::byte>(bytes);
        REQUIRE(cbor::decode_compact_into(decoded, {.first = span.first(split), .second = span.subspan(split)}, limits));
        CHECK(decoded.point == source.point);
        CHECK(decoded.wheel == source.wheel);
        CHECK(decoded.wheel_unit == source.wheel_unit);
        CHECK(decoded.other_button == source.other_button);
        CHECK(decoded.modifiers == source.modifiers);
        CHECK(decoded.brush_radius == WorkspaceMouse{}.brush_radius);
        CHECK(decoded.click_count == source.click_count);
        CHECK(decoded.peer_epoch == source.peer_epoch);
        CHECK(decoded.document_epoch == source.document_epoch);
    }
}
