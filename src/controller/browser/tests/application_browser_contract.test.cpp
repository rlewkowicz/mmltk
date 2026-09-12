#include "src/controller/browser/application_schema.h"
#include "src/controller/browser/application_workspace_abi_emitter.h"
#include "src/controller/browser/application_materializer.h"
#include "src/controller/browser/application_outer_routing_emitter.h"
#include "src/controller/browser/application_visual_projection_emitter.h"
#include "src/controller/browser/application_event_publisher.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>

#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/model_selection.h"
#include "src/controller/presentation/detail/workspace_surface_import_abi.h"
#include "src/controller/services/file_dialog_catalog.h"
#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/subsystems/upscale/upscale_system.h"
#include "src/frameworks/reflection/field_policy.h"

namespace mmltk::controller::browser::relation_audit_test {

struct OrphanProvider final {
    [[nodiscard]] static consteval bool valid() noexcept { return true; }
};
struct FirstProvider final {
    [[nodiscard]] static consteval bool valid() noexcept { return true; }
};
struct SecondProvider final {
    [[nodiscard]] static consteval bool valid() noexcept { return true; }
};
struct InvalidDestinationProvider final {
    [[nodiscard]] static consteval bool valid() noexcept { return true; }
};

class[[= mmltk::frameworks::reflection::OpaqueRelationStorage{}]] OverrideState final {
   public:
    constexpr OverrideState() noexcept = default;
    constexpr bool operator==(const OverrideState&) const noexcept = default;

   private:
    [[= mmltk::frameworks::reflection::Maximum<std::uint16_t>{1U}]] std::uint16_t mask = 0U;
};

struct Row final {
    std::uint16_t value = 0U;
};

struct OrphanSettings final {
    OverrideState overrides;
};

struct MultipleClaimSettings final {
    [[= mmltk::frameworks::reflection::CatalogProvider<FirstProvider>{}]] std::uint16_t first = 0U;
    [[= mmltk::frameworks::reflection::CatalogProvider<SecondProvider>{}]] std::uint16_t second = 0U;
    std::uint16_t value = 0U;
    OverrideState overrides;
};

struct InvalidDestinationSettings final {
    [[= mmltk::frameworks::reflection::CatalogProvider<InvalidDestinationProvider>{}]] std::uint16_t selector = 0U;
    [[= mmltk::controller::contracts::reflection::PersistenceMetadata{}]] std::uint16_t persisted = 0U;
    OverrideState overrides;
};

MMLTK_REFLECT_FIELDS(OverrideState)
MMLTK_REFLECT_FIELDS(Row)
MMLTK_REFLECT_FIELDS(OrphanSettings)
MMLTK_REFLECT_FIELDS(MultipleClaimSettings)
MMLTK_REFLECT_FIELDS(InvalidDestinationSettings)

template <class Settings, auto Selector, auto Destination>
struct RelationFixture
    : mmltk::frameworks::reflection::StaticMemberRelation<
          Row, Settings, 1U,
          mmltk::frameworks::reflection::MemberRelationEntry<mmltk::frameworks::reflection::member_path<&Row::value>, Destination>> {
    using override_state_type = OverrideState;
    inline static constexpr auto source_selector = mmltk::frameworks::reflection::member_path<&Row::value>;
    inline static constexpr auto destination_selector = Selector;
    inline static constexpr auto destination_override_state = mmltk::frameworks::reflection::member_path<&Settings::overrides>;
    inline static constexpr std::uint16_t valid_bits = 1U;

    [[nodiscard]] static consteval bool audit() { return RelationFixture::valid(); }
};

}  // namespace mmltk::controller::browser::relation_audit_test

namespace mmltk::frameworks::reflection {

template <>
struct catalog_provider_relation<mmltk::controller::browser::relation_audit_test::FirstProvider>
    : mmltk::controller::browser::relation_audit_test::RelationFixture<
          mmltk::controller::browser::relation_audit_test::MultipleClaimSettings,
          member_path<&mmltk::controller::browser::relation_audit_test::MultipleClaimSettings::first>,
          member_path<&mmltk::controller::browser::relation_audit_test::MultipleClaimSettings::value>> {};

template <>
struct catalog_provider_relation<mmltk::controller::browser::relation_audit_test::SecondProvider>
    : mmltk::controller::browser::relation_audit_test::RelationFixture<
          mmltk::controller::browser::relation_audit_test::MultipleClaimSettings,
          member_path<&mmltk::controller::browser::relation_audit_test::MultipleClaimSettings::second>,
          member_path<&mmltk::controller::browser::relation_audit_test::MultipleClaimSettings::value>> {};

// CLEANUP-IGNORE: Each specialization installs a distinct provider selector and malformed relation topology.
template <>
struct catalog_provider_relation<mmltk::controller::browser::relation_audit_test::InvalidDestinationProvider>
    : mmltk::controller::browser::relation_audit_test::RelationFixture<
          mmltk::controller::browser::relation_audit_test::InvalidDestinationSettings,
          member_path<&mmltk::controller::browser::relation_audit_test::InvalidDestinationSettings::selector>,
          member_path<&mmltk::controller::browser::relation_audit_test::InvalidDestinationSettings::persisted>> {};

}  // namespace mmltk::frameworks::reflection

namespace mmltk::controller::browser {
namespace {

enum class IntegrationPolicyBaseline : std::uint8_t {
    Command[[= contracts::IntegrationCommandDirection{false, false, false}]] = 7U,
};
enum class IntegrationPolicyUnchanged : std::uint8_t {
    Command[[= contracts::IntegrationCommandDirection{false, false, false}]] = 7U,
};
enum class IntegrationPolicyServer : std::uint8_t {
    Command[[= contracts::IntegrationCommandDirection{true, false, false}]] = 7U,
};
enum class IntegrationPolicyReadGeneration : std::uint8_t {
    Command[[= contracts::IntegrationCommandDirection{false, true, false}]] = 7U,
};
enum class IntegrationPolicyCompiledIndex : std::uint8_t {
    Command[[= contracts::IntegrationCommandDirection{false, false, true}]] = 7U,
};
enum class IntegrationPolicyRenamed : std::uint8_t {
    Renamed[[= contracts::IntegrationCommandDirection{false, false, false}]] = 7U,
};
enum class IntegrationPolicyReassigned : std::uint8_t {
    Command[[= contracts::IntegrationCommandDirection{false, false, false}]] = 8U,
};

static_assert(!application_schema_detail::settings_relations_are_valid<relation_audit_test::OrphanSettings>());
static_assert(!application_schema_detail::settings_relations_are_valid<relation_audit_test::MultipleClaimSettings>());
static_assert(!application_schema_detail::settings_relations_are_valid<relation_audit_test::InvalidDestinationSettings>());
static_assert(application_schema_detail::settings_relations_are_valid<contracts::GuiSettingsState>());

struct Increment final {
    [[= mmltk::frameworks::reflection::Minimum{std::int32_t{1}}]] std::int32_t amount = 1;
};
struct CounterSnapshot final {
    std::int32_t value = 0;
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] CounterChanged final {
    std::int32_t value = 0;
};

MMLTK_REFLECT_FIELDS(Increment)
MMLTK_REFLECT_FIELDS(CounterSnapshot)
MMLTK_REFLECT_FIELDS(CounterChanged)

[[nodiscard]] Interaction counter_interaction(const std::uint64_t endpoint_id, const std::int32_t amount) {
    wire::ByteBuffer bytes(64U);
    mmltk::frameworks::serialization::FixedCborEncoder writer(bytes);
    REQUIRE(mmltk::frameworks::serialization::encode_compact(writer, Increment{.amount = amount}));
    bytes.resize(writer.size());
    return {.endpoint_id = endpoint_id, .value = std::move(bytes)};
}

enum class DiscoveryMode : std::uint8_t {
    Ready,
    Complete,
};
MMLTK_REFLECT_ENUM(DiscoveryMode)

struct DiscoveryLeaf final {
    DiscoveryMode mode = DiscoveryMode::Ready;
    std::vector<std::uint32_t> samples;
};
struct DiscoveryEnvelope final {
    std::optional<DiscoveryLeaf> value;
};
MMLTK_REFLECT_FIELDS(DiscoveryLeaf)
MMLTK_REFLECT_FIELDS(DiscoveryEnvelope)

struct UnreflectedReachable final {
    std::uint32_t value = 0U;
};

class InvalidAritySystem final {
   public:
    [[= contracts::reflection::direct::IntentEndpoint{}]] std::int32_t Invalid(Increment, Increment) { return 0; }
};

class AnnotationEligibilityExamples final {
   public:
    [[= contracts::reflection::direct::IntentEndpoint{}]] std::int32_t ValidEndpoint(Increment) { return 0; }
    [[= contracts::reflection::direct::IntentEndpoint{}]] static std::int32_t StaticEndpoint(Increment) { return 0; }
    [[= contracts::reflection::Snapshot{256U}]] CounterSnapshot ValidSnapshot() const { return {}; }
    [[= contracts::reflection::Snapshot{256U}]] CounterSnapshot RefQualifiedSnapshot() const& { return {}; }
    [[= contracts::reflection::Snapshot{256U}]] static CounterSnapshot StaticSnapshot() { return {}; }
};

class CounterSystem final {
   public:
    using event_type = std::variant<CounterChanged>;
    [[= contracts::reflection::direct::IntentEndpoint{}]] std::int32_t IncrementBy(Increment request) {
        value_ += request.amount;
        return value_;
    }
    [[= contracts::reflection::direct::InteractionEndpoint{}]] void Replace(Increment request) {
        if (request.amount == 13) throw contracts::FailedError("counter replacement failed");
        value_ = request.amount;
    }
    [[= contracts::reflection::Snapshot{256U}]] [[nodiscard]] CounterSnapshot snapshot() const noexcept { return {.value = value_}; }

   private:
    std::int32_t value_ = 0;
};

struct TestSystems final {
    CounterSystem* counter = nullptr;
};

struct TestSettings final {
    std::uint32_t limit = 7U;
};
struct TestSettingsSnapshot final {
    TestSettings settings{};
};
struct OtherSettingsSnapshot final {
    TestSettings settings{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] TestSettingsChanged final {
    TestSettingsSnapshot snapshot{};
};

MMLTK_REFLECT_FIELDS(TestSettings)
MMLTK_REFLECT_FIELDS(TestSettingsSnapshot)
MMLTK_REFLECT_FIELDS(OtherSettingsSnapshot)
MMLTK_REFLECT_FIELDS(TestSettingsChanged)

[[nodiscard]] TestSettings default_test_settings() { return {}; }
[[nodiscard]] std::uint32_t wrong_test_settings_factory() { return 0U; }

class TestSettingsSystem final {
   public:
    using event_type = std::variant<TestSettingsChanged>;
    using settings_surface = SettingsSurface<&TestSettingsSnapshot::settings, &default_test_settings>;
    [[= contracts::reflection::Snapshot{256U}]] [[nodiscard]] TestSettingsSnapshot snapshot() const noexcept { return {}; }
};

class WrongOwnerSettingsSystem final {
   public:
    using event_type = std::variant<TestSettingsChanged>;
    using settings_surface = SettingsSurface<&OtherSettingsSnapshot::settings, &default_test_settings>;
    [[= contracts::reflection::Snapshot{256U}]] [[nodiscard]] TestSettingsSnapshot snapshot() const noexcept { return {}; }
};

class WrongFactorySettingsSystem final {
   public:
    using event_type = std::variant<TestSettingsChanged>;
    using settings_surface = SettingsSurface<&TestSettingsSnapshot::settings, &wrong_test_settings_factory>;
    [[= contracts::reflection::Snapshot{256U}]] [[nodiscard]] TestSettingsSnapshot snapshot() const noexcept { return {}; }
};

struct TestSettingsSystems final {
    TestSettingsSystem* settings = nullptr;
    CounterSystem* counter = nullptr;
};
struct TwoSettingsSystems final {
    TestSettingsSystem* first = nullptr;
    TestSettingsSystem* second = nullptr;
};
struct WrongOwnerSystems final {
    WrongOwnerSettingsSystem* settings = nullptr;
};
struct WrongFactorySystems final {
    WrongFactorySettingsSystem* settings = nullptr;
};

struct SyntheticRequest final {
    std::uint32_t value = 0U;
};
struct SyntheticSnapshot final {
    std::uint32_t value = 0U;
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] SyntheticChanged final {
    std::uint32_t value = 0U;
};
MMLTK_REFLECT_FIELDS(SyntheticRequest)
MMLTK_REFLECT_FIELDS(SyntheticSnapshot)
MMLTK_REFLECT_FIELDS(SyntheticChanged)

class AdditionalSyntheticSystem final {
   public:
    using event_type = std::variant<SyntheticChanged>;
    [[= contracts::reflection::direct::IntentEndpoint{}]] std::uint32_t Apply(SyntheticRequest request) { return request.value; }
    [[= contracts::reflection::direct::InteractionEndpoint{}]] void Observe(SyntheticRequest) {}
    [[= contracts::reflection::Snapshot{256U}]] [[nodiscard]] SyntheticSnapshot snapshot() const noexcept { return {}; }
};

struct ChangedTestSettingsSystems final {
    TestSettingsSystem* settings = nullptr;
    CounterSystem* counter = nullptr;
    AdditionalSyntheticSystem* additional = nullptr;
};

struct RoutingSystems final {
    CounterSystem* counter = nullptr;
    AdditionalSyntheticSystem* additional = nullptr;
};

struct SyntheticVisualOperation final {
    std::uint64_t revision = 0U;
    std::uint64_t alternate = 0U;
};
struct SyntheticVisualSnapshot final {
    SyntheticVisualOperation operation{};
    VisualFrame product{};
};
struct UnreflectedVisualOperation final {
    std::uint64_t revision = 0U;
};
struct UnreflectedNestedVisualSnapshot final {
    UnreflectedVisualOperation operation{};
    VisualFrame product{};
};
MMLTK_REFLECT_FIELDS(SyntheticVisualOperation)
MMLTK_REFLECT_FIELDS(SyntheticVisualSnapshot)
MMLTK_REFLECT_FIELDS(UnreflectedNestedVisualSnapshot)

template <PresentationSourceKind Kind, auto Revision = mmltk::frameworks::reflection::member_path<&SyntheticVisualSnapshot::operation,
                                                                                                  &SyntheticVisualOperation::revision>>
class SyntheticVisualSystem final {
   public:
    using event_type = std::variant<SyntheticChanged>;
    using visual_source = VisualSourceProjection<SyntheticVisualSnapshot, Kind,
                                                 mmltk::frameworks::reflection::member_path<&SyntheticVisualSnapshot::product>, Revision>;
    [[= contracts::reflection::direct::IntentEndpoint{}]] std::uint32_t Apply(SyntheticRequest request) { return request.value; }
    [[= contracts::reflection::Snapshot{64U * 1024U}]] [[nodiscard]] SyntheticVisualSnapshot snapshot() const {
        ++samples;
        SyntheticVisualSnapshot result;
        result.operation.revision = 91U;
        result.product = visual_frame({Kind, 1U}, {12U, 8U}, 7U);
        return result;
    }
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowFrame() const { return {}; }
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageWorkspace BorrowWorkspace() const { return {}; }
    [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceObservation ObserveWorkspace() const { return {}; }
    void RequestWorkspace(VisualWorkspaceRequest) {}
    mutable std::size_t samples = 0U;
};
struct SyntheticVisualComposition final {
    SyntheticVisualSystem<PresentationSourceKind::Predict>* producer = nullptr;
};
struct ExtendedVisualComposition final {
    SyntheticVisualSystem<PresentationSourceKind::Predict>* producer = nullptr;
    SyntheticVisualSystem<PresentationSourceKind::Explore>* additional = nullptr;
};
struct DuplicateVisualComposition final {
    SyntheticVisualSystem<PresentationSourceKind::Predict>* producer = nullptr;
    SyntheticVisualSystem<PresentationSourceKind::Predict>* duplicate = nullptr;
};
struct VisualFingerprintComposition final {
    TestSettingsSystem* settings = nullptr;
    SyntheticVisualSystem<PresentationSourceKind::Predict>* producer = nullptr;
};
struct AlternateVisualFingerprintComposition final {
    TestSettingsSystem* settings = nullptr;
    SyntheticVisualSystem<
        PresentationSourceKind::Predict,
        mmltk::frameworks::reflection::member_path<&SyntheticVisualSnapshot::operation, &SyntheticVisualOperation::alternate>>* producer =
        nullptr;
};

class RoutingTextWriter final {
   public:
    explicit RoutingTextWriter(std::ostringstream& output) : output_(output) {}

    [[nodiscard]] std::ostream& output() const noexcept { return output_; }

    void reserve(const std::string_view scope, const std::string_view symbol, std::string_view) {
        if (!symbols_.emplace(std::string(scope), std::string(symbol)).second) throw std::logic_error("projected symbol collision");
    }

    [[nodiscard]] std::string identifier(const std::string_view source, const bool upper) const {
        std::string result(source);
        if (!result.empty()) {
            result.front() = upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(result.front())))
                                   : static_cast<char>(std::tolower(static_cast<unsigned char>(result.front())));
        }
        return result;
    }

    template <class Type>
    [[nodiscard]] std::string rust_type() const {
        if constexpr (std::same_as<Type, void>) return "()";
        return identifier(mmltk::frameworks::serialization::reflected_schema_type_name<Type>(), true);
    }

    template <class Type>
    [[nodiscard]] std::string native_source() const {
        return std::string(std::meta::display_string_of(^^Type));
    }

   private:
    std::ostringstream& output_;
    std::set<std::pair<std::string, std::string>> symbols_;
};

static_assert(!application_settings_surface_is_valid<TestSystems>());
static_assert(!application_settings_surface_is_valid<TwoSettingsSystems>());
static_assert(!application_settings_surface_is_valid<WrongOwnerSystems>());
static_assert(!application_settings_surface_is_valid<WrongFactorySystems>());
static_assert(application_settings_surface_is_valid<TestSettingsSystems>());

struct ChangedSystems final {
    CounterSystem* renamed_counter = nullptr;
};

inline constexpr mmltk::frameworks::reflection::FixedText kValidFixedText{
    .capacity = 4U,
    .characters = mmltk::frameworks::reflection::FixedTextCharacterPolicy::PrintableAscii,
};
struct[[= kValidFixedText]] ValidFixedText final {
    std::array<char, kValidFixedText.capacity> bytes{};
    std::uint8_t size = 0U;
};
MMLTK_REFLECT_FIELDS(ValidFixedText)

inline constexpr mmltk::frameworks::reflection::FixedText kMismatchedFixedText{
    .capacity = 5U,
    .characters = mmltk::frameworks::reflection::FixedTextCharacterPolicy::PrintableAscii,
};
struct[[= kMismatchedFixedText]] MismatchedFixedText final {
    std::array<char, 4U> bytes{};
    std::uint8_t size = 0U;
};
MMLTK_REFLECT_FIELDS(MismatchedFixedText)

inline constexpr mmltk::frameworks::reflection::FixedText kUnsupportedFixedText{
    .capacity = 4U,
    .characters = static_cast<mmltk::frameworks::reflection::FixedTextCharacterPolicy>(255U),
};
struct[[= kUnsupportedFixedText]] UnsupportedFixedText final {
    std::array<char, 4U> bytes{};
    std::uint8_t size = 0U;
};
MMLTK_REFLECT_FIELDS(UnsupportedFixedText)

struct[[= kValidFixedText]] SignedSizeFixedText final {
    std::array<char, kValidFixedText.capacity> bytes{};
    std::int8_t size = 0;
};
MMLTK_REFLECT_FIELDS(SignedSizeFixedText)

inline constexpr mmltk::frameworks::reflection::FixedText kWideFixedText{
    .capacity = 300U,
    .characters = mmltk::frameworks::reflection::FixedTextCharacterPolicy::PrintableAscii,
};
struct[[= kWideFixedText]] NarrowSizeFixedText final {
    std::array<char, kWideFixedText.capacity> bytes{};
    std::uint8_t size = 0U;
};
MMLTK_REFLECT_FIELDS(NarrowSizeFixedText)

struct[[= kValidFixedText]] WrongByteStorageFixedText final {
    std::array<std::uint8_t, kValidFixedText.capacity> bytes{};
    std::uint8_t size = 0U;
};
MMLTK_REFLECT_FIELDS(WrongByteStorageFixedText)

struct DirectInheritanceRequest final {
    [[= mmltk::frameworks::reflection::Minimum<std::int32_t>{
        1}]][[= mmltk::frameworks::reflection::Maximum<std::int32_t>{9}]] std::int32_t inherited_limit = 4;
    bool derived_enabled = true;
};

struct InheritedRequestBase {
    [[= mmltk::frameworks::reflection::Minimum<std::int32_t>{
        1}]][[= mmltk::frameworks::reflection::Maximum<std::int32_t>{9}]] std::int32_t inherited_limit = 4;
};

struct InheritedRequest final : InheritedRequestBase {
    bool derived_enabled = true;
};

struct ChangedInheritedRequestBase {
    [[= mmltk::frameworks::reflection::Minimum<std::int32_t>{
        2}]][[= mmltk::frameworks::reflection::Maximum<std::int32_t>{9}]] std::int32_t inherited_limit = 4;
};

struct ChangedInheritedRequest final : ChangedInheritedRequestBase {
    bool derived_enabled = true;
};

MMLTK_REFLECT_FIELDS(DirectInheritanceRequest)
MMLTK_REFLECT_FIELDS(InheritedRequestBase)
MMLTK_REFLECT_FIELDS(InheritedRequest)
MMLTK_REFLECT_FIELDS(ChangedInheritedRequestBase)
MMLTK_REFLECT_FIELDS(ChangedInheritedRequest)

template <class Request>
class InheritanceFixtureSystem final {
   public:
    using event_type = std::variant<CounterChanged>;

    [[= contracts::reflection::direct::IntentEndpoint{}]] std::int32_t Apply(Request request) { return request.inherited_limit; }

    [[= contracts::reflection::Snapshot{256U}]] [[nodiscard]] CounterSnapshot snapshot() const noexcept { return {}; }
};

struct DirectInheritanceSystems final {
    InheritanceFixtureSystem<DirectInheritanceRequest>* fixture = nullptr;
};

struct InheritedSystems final {
    InheritanceFixtureSystem<InheritedRequest>* fixture = nullptr;
};

struct ChangedInheritedSystems final {
    InheritanceFixtureSystem<ChangedInheritedRequest>* fixture = nullptr;
};

struct DirectInheritanceSettings final {
    [[= mmltk::frameworks::reflection::Minimum<std::uint32_t>{
        2U}]][[= mmltk::frameworks::reflection::Maximum<std::uint32_t>{12U}]] std::uint32_t inherited_limit = 6U;
    [[= contracts::reflection::PersistenceMetadata{}]] std::uint64_t persisted_revision = 17U;
    bool derived_enabled = true;
};

struct InheritedSettingsBase {
    [[= mmltk::frameworks::reflection::Minimum<std::uint32_t>{
        2U}]][[= mmltk::frameworks::reflection::Maximum<std::uint32_t>{12U}]] std::uint32_t inherited_limit = 6U;
    [[= contracts::reflection::PersistenceMetadata{}]] std::uint64_t persisted_revision = 17U;
};

struct InheritedSettings final : InheritedSettingsBase {
    bool derived_enabled = true;
};

MMLTK_REFLECT_FIELDS(DirectInheritanceSettings)
MMLTK_REFLECT_FIELDS(InheritedSettingsBase)
MMLTK_REFLECT_FIELDS(InheritedSettings)

TEST_CASE("canonical application schema owns endpoint dispatch and stable identities", "[controller][browser][reflection]") {
    using Surface = ApplicationIntentSurface<TestSystems>;
    STATIC_REQUIRE(Surface::count == 2U);
    STATIC_REQUIRE(Surface::HasUniqueStableIds());

    CounterSystem counter;
    TestSystems systems{.counter = &counter};
    std::uint64_t intent_id = 0U;
    std::uint64_t interaction_id = 0U;
    Surface::Visit([&]<class Endpoint>() {
        if constexpr (Endpoint::interaction)
            interaction_id = Endpoint::stable_id;
        else
            intent_id = Endpoint::stable_id;
    });

    std::uint64_t amount_id = 0U;
    Surface::Visit([&]<class Endpoint>() {
        if constexpr (!Endpoint::interaction) {
            ApplicationSchema<TestSystems>::template VisitRequestFields<Endpoint>(
                [&]<class Owner, class Declaration>(const ApplicationRequestFieldFact& field) { amount_id = field.stable_id; });
        }
    });
    REQUIRE(amount_id != 0U);
    const auto reply = dispatch_intent(
        systems,
        Intent{.correlation = 7U, .endpoint_id = intent_id, .fields = {{.field_id = amount_id, .value = wire::Value(std::int64_t{4})}}});
    REQUIRE(reply.result.has_value());
    CHECK(counter.snapshot().value == 4);

    const auto accepted = dispatch_interaction(systems, counter_interaction(interaction_id, 9));
    CHECK(accepted.disposition == InteractionDispatchDisposition::Accepted);
    CHECK_FALSE(accepted.error.has_value());
    CHECK(counter.snapshot().value == 9);

    const auto unknown = dispatch_interaction(systems, Interaction{.endpoint_id = std::numeric_limits<std::uint64_t>::max(), .value = {}});
    CHECK(unknown.disposition == InteractionDispatchDisposition::ProtocolInvalid);
    CHECK_FALSE(unknown.error.has_value());

    const auto malformed = dispatch_interaction(systems, Interaction{.endpoint_id = interaction_id, .value = {std::byte{0xf6}}});
    CHECK(malformed.disposition == InteractionDispatchDisposition::ProtocolInvalid);
    CHECK_FALSE(malformed.error.has_value());

    TestSystems unavailable_systems{};
    const auto unavailable = dispatch_interaction(unavailable_systems, counter_interaction(interaction_id, 7));
    CHECK(unavailable.disposition == InteractionDispatchDisposition::ApplicationRejected);
    REQUIRE(unavailable.error.has_value());
    CHECK(unavailable.error->category == contracts::ApplicationErrorCategory::Unavailable);

    const auto failed = dispatch_interaction(systems, counter_interaction(interaction_id, 13));
    CHECK(failed.disposition == InteractionDispatchDisposition::ApplicationRejected);
    REQUIRE(failed.error.has_value());
    CHECK(failed.error->category == contracts::ApplicationErrorCategory::Failed);
}

TEST_CASE("visual producer projections derive nested observations and composition routing", "[controller][browser][reflection]") {
    STATIC_REQUIRE(ApplicationSchema<ApplicationSystems>::VisualSourceCount() == 5U);
    STATIC_REQUIRE(ApplicationSchema<SyntheticVisualComposition>::VisualSourceCount() == 1U);
    STATIC_REQUIRE(ApplicationSchema<ExtendedVisualComposition>::VisualSourceCount() == 2U);
    STATIC_REQUIRE_FALSE(ApplicationSchema<DuplicateVisualComposition>::VisualSourcesAreUnique());
    using namespace mmltk::frameworks::reflection;
    using WrongOwner =
        VisualSourceProjection<SyntheticVisualSnapshot, PresentationSourceKind::Predict, member_path<&ExploreSnapshot::frame>,
                               member_path<&SyntheticVisualSnapshot::operation, &SyntheticVisualOperation::revision>>;
    using WrongType =
        VisualSourceProjection<SyntheticVisualSnapshot, PresentationSourceKind::Predict, member_path<&SyntheticVisualSnapshot::operation>,
                               member_path<&SyntheticVisualSnapshot::product>>;
    using EmptyKind =
        VisualSourceProjection<SyntheticVisualSnapshot, PresentationSourceKind::None, member_path<&SyntheticVisualSnapshot::product>,
                               member_path<&SyntheticVisualSnapshot::operation, &SyntheticVisualOperation::revision>>;
    using UnknownKind = VisualSourceProjection<SyntheticVisualSnapshot, static_cast<PresentationSourceKind>(255U),
                                               member_path<&SyntheticVisualSnapshot::product>,
                                               member_path<&SyntheticVisualSnapshot::operation, &SyntheticVisualOperation::revision>>;
    constexpr auto callable_frame = [](SyntheticVisualSnapshot& value) -> VisualFrame& { return value.product; };
    constexpr auto callable_operation = [](SyntheticVisualSnapshot& value) -> SyntheticVisualOperation& { return value.operation; };
    using CallableFrame = VisualSourceProjection<SyntheticVisualSnapshot, PresentationSourceKind::Predict, callable_frame,
                                                 member_path<&SyntheticVisualSnapshot::operation, &SyntheticVisualOperation::revision>>;
    using CallableSegment =
        VisualSourceProjection<SyntheticVisualSnapshot, PresentationSourceKind::Predict, member_path<&SyntheticVisualSnapshot::product>,
                               member_path<callable_operation, &SyntheticVisualOperation::revision>>;
    using UnreflectedSegment =
        VisualSourceProjection<UnreflectedNestedVisualSnapshot, PresentationSourceKind::Predict,
                               member_path<&UnreflectedNestedVisualSnapshot::product>,
                               member_path<&UnreflectedNestedVisualSnapshot::operation, &UnreflectedVisualOperation::revision>>;
    STATIC_REQUIRE_FALSE(WrongOwner::valid());
    STATIC_REQUIRE_FALSE(WrongType::valid());
    STATIC_REQUIRE_FALSE(EmptyKind::valid());
    STATIC_REQUIRE_FALSE(UnknownKind::valid());
    STATIC_REQUIRE_FALSE(CallableFrame::valid());
    STATIC_REQUIRE_FALSE(CallableSegment::valid());
    STATIC_REQUIRE_FALSE(UnreflectedSegment::valid());
    STATIC_REQUIRE_FALSE(application_event_is_member<TestSystems, &TestSystems::counter, SyntheticChanged>());

    SyntheticVisualSystem<PresentationSourceKind::Predict> producer;
    SyntheticVisualSystem<PresentationSourceKind::Explore> additional;
    auto readers = materialize_visual_source_readers(ExtendedVisualComposition{&producer, &additional});
    for (const auto& reader : readers) {
        const auto observation = reader.observe();
        CHECK(observation.snapshot_revision == 91U);
        CHECK(observation.frame.revision == 7U);
        CHECK(observation.frame.source == reader.source);
    }
    CHECK(producer.samples == 1U);
    CHECK(additional.samples == 1U);
    std::ostringstream output;
    RoutingTextWriter writer(output);
    emit_application_visual_projection<ExtendedVisualComposition>(writer);
    CHECK(output.str().find("snapshot.operation.revision") != std::string::npos);
    CHECK(output.str().find("self.additional.map") != std::string::npos);
    CHECK(output.str().find("snapshot.product") != std::string::npos);
    std::ostringstream collision_output;
    RoutingTextWriter collision(collision_output);
    collision.reserve("module", "ApplicationVisualSnapshots", "conflicting declaration");
    CHECK_THROWS_AS(emit_application_visual_projection<ExtendedVisualComposition>(collision), std::logic_error);
    CHECK(application_schema_fingerprint<VisualFingerprintComposition>() !=
          application_schema_fingerprint<AlternateVisualFingerprintComposition>());
}

TEST_CASE("materialized event publisher preserves transient and essential failure policy", "[controller][browser][reflection]") {
    std::size_t lost = 0U;
    std::function<void(SystemEvent)> failing = [](SystemEvent) { throw std::runtime_error("publication failed"); };
    ApplicationEventPublisher<&RoutingSystems::additional, RoutingSystems> transient(failing, [&] { ++lost; });
    transient(AdditionalSyntheticSystem::event_type{SyntheticChanged{}});
    CHECK(lost == 0U);
    ApplicationEventPublisher<&ApplicationSystems::explore> essential(failing, [&] { ++lost; });
    essential(ExploreSystem::event_type{ExploreChanged{}});
    CHECK(lost == 1U);
    std::size_t delivered = 0U;
    std::function<void(SystemEvent)> receiving = [&](SystemEvent) { ++delivered; };
    ApplicationEventPublisher<&ApplicationSystems::explore> encoding(receiving, [&] { ++lost; });
    ExploreFailed oversized;
    oversized.detail.assign(kVisualFailureByteCapacity + 1U, 'x');
    encoding(ExploreSystem::event_type{oversized});
    CHECK(delivered == 0U);
    CHECK(lost == 2U);
    SystemEvent published;
    std::function<void(SystemEvent)> latest_sink = [&](SystemEvent event) { published = std::move(event); };
    ApplicationEventPublisher<&ApplicationSystems::presentation> latest(latest_sink, [&] { ++lost; });
    latest(PresentationSystem::event_type{PresentationCompleted{.snapshot = {.revision = 73U}}});
    CHECK(published.delivery == contracts::reflection::EventDelivery::LatestState);
    CHECK(published.state_revision == 73U);
    CHECK(published.event_id ==
          ApplicationEventIdentity<ApplicationSystems, &ApplicationSystems::presentation, PresentationCompleted>::event_id);
    CHECK(lost == 2U);
    std::optional<PresentationSourceIdentity> notified;
    std::function<void(SystemEvent)> absent;
    ApplicationEventPublisher<&ApplicationSystems::annotation> annotation(
        absent, {}, [&](const PresentationSourceIdentity source) { notified = source; });
    annotation(AnnotationSystem::event_type{AnnotationFrameChanged{}});
    CHECK((notified == std::optional{PresentationSourceIdentity{PresentationSourceKind::Annotation, 1U}}));
}

TEST_CASE("clean identity preserves semantic updates and distinguishes geometry and legacy products", "[controller][browser][reflection]") {
    const std::array kinds{PresentationSourceKind::None,    PresentationSourceKind::Explore, PresentationSourceKind::Annotation,
                           PresentationSourceKind::Predict, PresentationSourceKind::Live,    PresentationSourceKind::Upscale};
    for (std::size_t session = 0U; session < kinds.size(); ++session)
        CHECK(presentation_source_session(kinds[session]) == session);
    auto frame = visual_frame({PresentationSourceKind::Explore, 1U}, {32U, 24U}, 7U);
    frame.content = {1U, 2U, 20U, 16U};
    CHECK(visual_clean_content_identity(frame).revision == 7U);
    frame.clean_revision = 43U;
    const auto identity = visual_clean_content_identity(frame);
    auto semantic = frame;
    ++semantic.revision;
    CHECK(visual_clean_content_identity(semantic) == identity);
    const VisualSourceObservation metadata{semantic, 100U};
    CHECK(visual_clean_content_identity(metadata.frame) == identity);
    ++semantic.extent.width;
    CHECK(visual_clean_content_identity(semantic) != identity);
    semantic = frame;
    ++semantic.content.x;
    CHECK(visual_clean_content_identity(semantic) != identity);
    semantic = frame;
    ++semantic.source.instance;
    CHECK(visual_clean_content_identity(semantic) != identity);
}

TEST_CASE("fixed text schema policy validates its complete native storage shape") {
    STATIC_REQUIRE(application_schema_detail::valid_fixed_text_shape<ValidFixedText>());
    STATIC_REQUIRE_FALSE(application_schema_detail::valid_fixed_text_shape<MismatchedFixedText>());
    STATIC_REQUIRE_FALSE(application_schema_detail::valid_fixed_text_shape<UnsupportedFixedText>());
    STATIC_REQUIRE_FALSE(application_schema_detail::valid_fixed_text_shape<SignedSizeFixedText>());
    STATIC_REQUIRE_FALSE(application_schema_detail::valid_fixed_text_shape<NarrowSizeFixedText>());
    STATIC_REQUIRE_FALSE(application_schema_detail::valid_fixed_text_shape<WrongByteStorageFixedText>());
}

TEST_CASE("closed application categories discover nested reflected declarations transitively") {
    STATIC_REQUIRE(application_schema_detail::runtime_boundary_projectable<DiscoveryEnvelope>());
    STATIC_REQUIRE(application_schema_detail::runtime_boundary_projectable<std::variant<DiscoveryEnvelope, std::array<std::byte, 4U>>>());
    STATIC_REQUIRE_FALSE(application_schema_detail::runtime_boundary_projectable<UnreflectedReachable>());
    STATIC_REQUIRE_FALSE(application_schema_detail::runtime_boundary_projectable<long double>());
    STATIC_REQUIRE_FALSE(application_schema_detail::runtime_boundary_projectable<std::string_view>());
    STATIC_REQUIRE(application_schema_detail::catalog_boundary_projectable<mmltk::backend::models::rfdetr::PresetCatalogEntry>());
    STATIC_REQUIRE(application_schema_detail::catalog_boundary_projectable<std::array<std::uint16_t, 2U>>());
    STATIC_REQUIRE(application_schema_detail::catalog_boundary_projectable<std::optional<std::array<std::string_view, 2U>>>());
    STATIC_REQUIRE(application_schema_detail::catalog_boundary_projectable<std::string_view>());
    STATIC_REQUIRE_FALSE(application_schema_detail::catalog_boundary_projectable<std::optional<long double>>());
    STATIC_REQUIRE_FALSE(application_schema_detail::catalog_boundary_projectable<std::vector<std::uint16_t>>());
    STATIC_REQUIRE_FALSE(application_schema_detail::catalog_boundary_projectable<std::inplace_vector<std::uint16_t, 2U>>());
    STATIC_REQUIRE_FALSE(application_schema_detail::catalog_boundary_projectable<std::array<std::uint16_t, 0U>>());
    STATIC_REQUIRE_FALSE(application_schema_detail::catalog_boundary_projectable<std::array<std::uint8_t, 2U>>());
    STATIC_REQUIRE_FALSE(application_schema_detail::catalog_boundary_projectable<std::array<char, 2U>>());
    STATIC_REQUIRE_FALSE(application_schema_detail::catalog_boundary_projectable<std::array<std::byte, 2U>>());
    STATIC_REQUIRE_FALSE(application_schema_detail::SupportedEndpointMethod<^^InvalidAritySystem::Invalid>);
    STATIC_REQUIRE(application_schema_detail::endpoint_member_eligible<^^AnnotationEligibilityExamples::ValidEndpoint>());
    STATIC_REQUIRE_FALSE(application_schema_detail::endpoint_member_eligible<^^AnnotationEligibilityExamples::StaticEndpoint>());
    STATIC_REQUIRE(application_schema_detail::endpoint_annotation_count<^^AnnotationEligibilityExamples::StaticEndpoint>() == 1U);
    STATIC_REQUIRE(application_schema_detail::snapshot_member_eligible<^^AnnotationEligibilityExamples::ValidSnapshot>());
    STATIC_REQUIRE(application_schema_detail::SupportedSnapshotMethod<^^AnnotationEligibilityExamples::ValidSnapshot>);
    STATIC_REQUIRE_FALSE(application_schema_detail::SupportedSnapshotMethod<^^AnnotationEligibilityExamples::RefQualifiedSnapshot>);
    STATIC_REQUIRE_FALSE(application_schema_detail::snapshot_member_eligible<^^AnnotationEligibilityExamples::StaticSnapshot>());
    STATIC_REQUIRE(
        application_schema_detail::annotation_count<^^AnnotationEligibilityExamples::StaticSnapshot, contracts::reflection::Snapshot>() ==
        1U);
    STATIC_REQUIRE(application_schema_detail::annotation_count<^^CounterChanged, contracts::reflection::Event>() == 1U);
}

TEST_CASE("protocol-16 fingerprint is deterministic and covers stable composition identity", "[controller][browser][reflection]") {
    namespace cbor = mmltk::frameworks::serialization;
    STATIC_REQUIRE(cbor::compact_shape<AnnotationInputBatch> == cbor::CompactShape::Object);
    STATIC_REQUIRE(cbor::compact_shape<ExploreViewportUpdate> == cbor::CompactShape::Object);
    STATIC_REQUIRE(cbor::compact_shape<std::array<std::uint16_t, 2U>> == cbor::CompactShape::Sequence);
    STATIC_REQUIRE(cbor::compact_shape<std::vector<AnnotationPointer>> == cbor::CompactShape::Unsupported);
    STATIC_REQUIRE(cbor::compact_shape<std::string> == cbor::CompactShape::Unsupported);
    STATIC_REQUIRE(cbor::compact_shape<wire::Value> == cbor::CompactShape::Unsupported);
    STATIC_REQUIRE(cbor::compact_shape<std::variant<AnnotationPointer>> == cbor::CompactShape::Variant);
    STATIC_REQUIRE(cbor::compact_shape<long double> == cbor::CompactShape::Unsupported);
    STATIC_REQUIRE(kAnnotationInputBatchCapacity == 32U);
    STATIC_REQUIRE(cbor::compact_maximum_cbor_bytes<AnnotationInputBatch>() <= kMaxIntentValueBytes);
    const auto structural = []<class T>() {
        application_schema_detail::FingerprintSink sink;
        application_schema_detail::append_type<T>(sink);
        return sink.words();
    };
    CHECK(structural.template operator()<std::string>() == structural.template operator()<std::filesystem::path>());
    CHECK(structural.template operator()<std::array<std::uint16_t, 2U>>() !=
          structural.template operator()<std::array<std::uint16_t, 3U>>());
    CHECK(structural.template operator()<std::array<std::uint16_t, 2U>>() !=
          structural.template operator()<std::array<std::uint32_t, 2U>>());
    CHECK(structural.template operator()<std::inplace_vector<std::uint16_t, 0U>>() !=
          structural.template operator()<std::vector<std::uint16_t>>());
    application_schema_detail::FingerprintSink default_value;
    application_schema_detail::append_wire_value(default_value, wire::Value{std::uint64_t{7U}});
    application_schema_detail::FingerprintSink expected_default;
    expected_default.append("integer");
    expected_default.append_number(sizeof(std::uint64_t));
    expected_default.append_number(false);
    expected_default.append_number(std::uint64_t{7U});
    CHECK(default_value.words() == expected_default.words());
    const auto repeated = []<class First, class Second>() {
        application_schema_detail::FingerprintSink sink;
        application_schema_detail::append_type<First>(sink);
        application_schema_detail::append_type<Second>(sink);
        return sink.words();
    };
    CHECK((repeated.template operator()<std::optional<std::uint16_t>, std::optional<std::uint32_t>>() !=
           repeated.template operator()<std::optional<std::uint32_t>, std::optional<std::uint16_t>>()));
    const auto object_order = [](const bool reverse) {
        using Sink = application_schema_detail::FingerprintSink;
        std::vector<std::function<void(Sink&)>> fields;
        auto field = [&]<class, class Declaration>(const auto& fact) {
            fields.emplace_back([fact](Sink& sink) {
                sink.append(fact.member_name);
                application_schema_detail::append_constraint(sink, fact.constraint);
                sink.append_number(static_cast<std::uint8_t>(fact.presentation));
                application_schema_detail::append_annotations<Declaration>(sink);
                application_schema_detail::append_type<typename Declaration::member_type>(sink);
            });
        };
        application_schema_detail::visit_fields<IntentField>(field);
        if (reverse) std::ranges::reverse(fields);
        Sink sink;
        sink.append("type");
        sink.append("object");
        sink.append_number(fields.size());
        for (const auto& append : fields)
            append(sink);
        sink.append("end-type");
        return sink.words();
    };
    CHECK(object_order(false) == structural.template operator()<IntentField>());
    CHECK(object_order(true) != object_order(false));
    const auto verify_variant = [&]<class Variant>() {
        application_schema_detail::FingerprintSink expected;
        expected.append("type");
        expected.append("variant");
        expected.append_number(std::variant_size_v<Variant>);
        application_schema_detail::Variant<Variant>::Visit([&]<class Alternative>() {
            expected.append(std::meta::identifier_of(^^Alternative));
            application_schema_detail::append_type<Alternative>(expected);
        });
        expected.append("end-type");
        CHECK(expected.words() == structural.template operator()<Variant>());
    };
    verify_variant.template operator()<ClientRecord>();
    verify_variant.template operator()<ServerRecord>();
    application_schema_detail::FingerprintSink actual_enum;
    application_schema_detail::append_type<RendererObservationKind>(actual_enum);
    for (const bool reassign : {false, true}) {
        application_schema_detail::FingerprintSink expected_enum;
        expected_enum.append("type");
        expected_enum.append("enum");  // CLEANUP-IGNORE: This altered-enum oracle independently verifies the production fingerprint.
        using Underlying = std::underlying_type_t<RendererObservationKind>;
        expected_enum.append_number(sizeof(Underlying));
        expected_enum.append_number(std::is_signed_v<Underlying>);
        expected_enum.append_number(mmltk::frameworks::reflection::enum_entries<RendererObservationKind>().size());
        for (const auto entry : mmltk::frameworks::reflection::enum_entries<RendererObservationKind>()) {
            expected_enum.append(entry.name);
            expected_enum.append_number(static_cast<Underlying>(static_cast<Underlying>(entry.value) + (reassign ? 1U : 0U)));
        }
        expected_enum.append("end-type");
        CHECK((actual_enum.words() == expected_enum.words()) == !reassign);
    }
    const auto first = application_schema_fingerprint<TestSettingsSystems>();
    const auto second = application_schema_fingerprint<TestSettingsSystems>();
    const auto changed = application_schema_fingerprint<ChangedTestSettingsSystems>();
    CHECK(first == second);
    CHECK(first != changed);
    CHECK(first.words[0] != 0U);
    CHECK(first.words[1] != 0U);
    const auto settings_facts = []<class Composition>() {
        std::vector<std::pair<std::string, std::uint64_t>> leaves;
        ApplicationSchema<Composition>::VisitApplicationSettingsLeaves(
            [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsLeafFact& fact) {
                leaves.emplace_back(fact.path, fact.stable_id);
            });
        std::vector<std::pair<std::string, std::uint32_t>> defaults;
        ApplicationSchema<Composition>::VisitApplicationSettingsDefaults(
            [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsDefaultFact& fact, const Member& value) {
                if constexpr (std::same_as<Member, std::uint32_t>) defaults.emplace_back(fact.path, value);
            });
        return std::pair{leaves, defaults};
    };
    CHECK(settings_facts.template operator()<TestSettingsSystems>() == settings_facts.template operator()<ChangedTestSettingsSystems>());
}

TEST_CASE("application fingerprint includes canonical integration command identity and complete policy",
          "[controller][browser][reflection]") {
    const auto schema = application_schema_detail::application_schema_sink<TestSettingsSystems>();
    const auto with_policy = [&]<class Kind>() {
        auto sink = schema;
        application_schema_detail::append_integration_command_policy<Kind>(sink);
        return ApplicationSchemaFingerprint{sink.words()};
    };
    const auto baseline = with_policy.template operator()<IntegrationPolicyBaseline>();
    CHECK(baseline == with_policy.template operator()<IntegrationPolicyBaseline>());
    CHECK(baseline == with_policy.template operator()<IntegrationPolicyUnchanged>());
    CHECK(baseline != with_policy.template operator()<IntegrationPolicyServer>());
    CHECK(baseline != with_policy.template operator()<IntegrationPolicyReadGeneration>());
    CHECK(baseline != with_policy.template operator()<IntegrationPolicyCompiledIndex>());
    CHECK(baseline != with_policy.template operator()<IntegrationPolicyRenamed>());
    CHECK(baseline != with_policy.template operator()<IntegrationPolicyReassigned>());

    const auto canonical = with_policy.template operator()<contracts::IntegrationControlKind>();
    const auto actual = application_schema_fingerprint<TestSettingsSystems>();
    CHECK(actual == canonical);
    CHECK(actual != ApplicationSchemaFingerprint{schema.words()});
}

TEST_CASE("static outer routing derives intent-only ownership from an endpoint-only composition",
          "[controller][browser][reflection][routing]") {
    std::ostringstream output;
    RoutingTextWriter writer(output);
    emit_application_outer_routing<RoutingSystems>(writer);
    const auto text = output.str();
    const auto endpoint_begin = text.find("pub enum ApplicationEndpoint");
    const auto intent_begin = text.find("pub enum ApplicationIntentEndpoint");
    REQUIRE(endpoint_begin != std::string::npos);
    const auto intent_end = text.find("pub const fn application_endpoint_stable_id", intent_begin);
    REQUIRE(intent_begin != std::string::npos);
    REQUIRE(intent_end != std::string::npos);
    const auto intents = std::string_view(text).substr(intent_begin, intent_end - intent_begin);
    const auto endpoints = std::string_view(text).substr(endpoint_begin, intent_begin - endpoint_begin);
    CHECK(endpoints.find("CounterReplace") != std::string_view::npos);
    CHECK(endpoints.find("AdditionalObserve") != std::string_view::npos);
    CHECK(intents.find("CounterIncrementBy") != std::string_view::npos);
    CHECK(intents.find("AdditionalApply") != std::string_view::npos);
    CHECK(intents.find("CounterReplace") == std::string_view::npos);
    CHECK(intents.find("AdditionalObserve") == std::string_view::npos);

    const auto decode_begin = text.find("decode_application_intent_endpoint");
    const auto decode_end = text.find("application_intent_endpoint_stable_id", decode_begin);
    REQUIRE(decode_begin != std::string::npos);
    REQUIRE(decode_end != std::string::npos);
    const auto decode = std::string_view(text).substr(decode_begin, decode_end - decode_begin);
    CHECK(decode.find(std::to_string(application_stable_id("counter", "IncrementBy"))) != std::string_view::npos);
    CHECK(decode.find(std::to_string(application_stable_id("additional", "Apply"))) != std::string_view::npos);
    CHECK(decode.find(std::to_string(application_stable_id("counter", "Replace"))) == std::string_view::npos);
    CHECK(decode.find(std::to_string(application_stable_id("additional", "Observe"))) == std::string_view::npos);
    const auto reply_begin = text.find("application_reply_endpoint");
    const auto reply_end = text.find("decode_application_snapshot", reply_begin);
    REQUIRE(reply_begin != std::string::npos);
    REQUIRE(reply_end != std::string::npos);
    const auto replies = std::string_view(text).substr(reply_begin, reply_end - reply_begin);
    CHECK(replies.find("CounterIncrementBy") != std::string_view::npos);
    CHECK(replies.find("AdditionalApply") != std::string_view::npos);
    CHECK(replies.find("CounterReplace") == std::string_view::npos);
    CHECK(replies.find("AdditionalObserve") == std::string_view::npos);

    const std::array fixture_constants{
        std::pair{"SYSTEM_Counter", application_stable_id("counter")},
        std::pair{"SYSTEM_Additional", application_stable_id("additional")},
        std::pair{"ENDPOINT_Counter_IncrementBy", application_stable_id("counter", "IncrementBy")},
        std::pair{"ENDPOINT_Counter_Replace", application_stable_id("counter", "Replace")},
        std::pair{"ENDPOINT_Additional_Apply", application_stable_id("additional", "Apply")},
        std::pair{"ENDPOINT_Additional_Observe", application_stable_id("additional", "Observe")},
    };
    std::set<std::uint64_t> fixture_ids;
    for (const auto& [symbol, native_id] : fixture_constants) {
        CAPTURE(symbol, native_id);
        CHECK(text.find("pub const " + std::string(symbol) + ": u64 = " + std::to_string(native_id) + ";") != std::string::npos);
        CHECK(fixture_ids.insert(native_id).second);
    }
    CHECK(fixture_ids.size() == fixture_constants.size());
    CHECK(text.find("SCHEMA_FINGERPRINT") == std::string::npos);
}

TEST_CASE("native application schema flattens inherited request and settings declarations base first",
          "[controller][browser][reflection][inheritance]") {
    std::vector<std::string_view> request_names;
    std::vector<std::int32_t> request_defaults;
    std::vector<std::uint64_t> request_ids;
    bool inherited_request_owner = false;
    ApplicationSchema<InheritedSystems>::VisitEndpoints([&]<class Endpoint>() {
        ApplicationSchema<InheritedSystems>::template VisitRequestFields<Endpoint>(
            [&]<class Owner, class Declaration>(const ApplicationRequestFieldFact& field) {
                request_names.push_back(field.name);
                request_ids.push_back(field.stable_id);
                if constexpr (std::same_as<Owner, InheritedRequestBase>) {
                    inherited_request_owner = field.name == "inherited_limit";
                    CHECK(field.constraint.has_minimum);
                    CHECK(field.constraint.minimum == 1.0L);
                    CHECK(field.constraint.has_maximum);
                    CHECK(field.constraint.maximum == 9.0L);
                }
            });
        ApplicationSchema<InheritedSystems>::template VisitRequestDefaults<Endpoint>(
            [&]<class Owner, class Declaration, class Member>(const ApplicationRequestFieldFact& field, const Member& value) {
                if constexpr (std::same_as<Member, std::int32_t>) {
                    CHECK(field.name == "inherited_limit");
                    request_defaults.push_back(value);
                }
            });
    });
    REQUIRE((request_names == std::vector<std::string_view>{"inherited_limit", "derived_enabled"}));
    REQUIRE((request_defaults == std::vector<std::int32_t>{4}));
    REQUIRE(request_ids.size() == 2U);
    CHECK(inherited_request_owner);

    std::uint64_t direct_request_id = 0U;
    ApplicationSchema<DirectInheritanceSystems>::VisitRequestFields(
        [&]<class Owner, class Declaration>(const ApplicationRequestFieldFact& field) {
            if (field.name == "inherited_limit") direct_request_id = field.stable_id;
        });
    CHECK(request_ids.front() == direct_request_id);

    struct SettingsFact final {
        std::string path;
        std::uint64_t stable_id;
        bool mutable_leaf;
    };
    std::vector<SettingsFact> settings;
    bool inherited_settings_owner = false;
    ApplicationSchema<InheritedSystems>::VisitSettingsLeaves<InheritedSettings>(
        [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsLeafFact& field) {
            settings.push_back({std::string(field.path), field.stable_id, field.mutable_leaf});
            if constexpr (std::same_as<Owner, InheritedSettingsBase>) {
                if (field.path == "inherited_limit") {
                    inherited_settings_owner = true;
                    CHECK(field.constraint.has_minimum);
                    CHECK(field.constraint.minimum == 2.0L);
                    CHECK(field.constraint.has_maximum);
                    CHECK(field.constraint.maximum == 12.0L);
                }
            }
        });
    REQUIRE(settings.size() == 3U);
    CHECK(settings[0].path == "inherited_limit");
    CHECK(settings[0].stable_id == application_settings_field_stable_id("inherited_limit"));
    CHECK(settings[0].mutable_leaf);
    CHECK(settings[1].path == "persisted_revision");
    CHECK(settings[1].stable_id == application_settings_field_stable_id("persisted_revision"));
    CHECK_FALSE(settings[1].mutable_leaf);
    CHECK(settings[2].path == "derived_enabled");
    CHECK(settings[2].mutable_leaf);
    CHECK(inherited_settings_owner);

    std::uint64_t direct_settings_id = 0U;
    ApplicationSchema<InheritedSystems>::VisitSettingsLeaves<DirectInheritanceSettings>(
        [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsLeafFact& field) {
            if (field.path == "inherited_limit") direct_settings_id = field.stable_id;
        });
    CHECK(settings.front().stable_id == direct_settings_id);

    std::vector<std::uint64_t> settings_default_ids;
    std::vector<std::uint64_t> integer_defaults;
    ApplicationSchema<InheritedSystems>::VisitSettingsDefaults(
        InheritedSettings{},
        [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsDefaultFact& field, const Member& value) {
            settings_default_ids.push_back(field.stable_id);
            if constexpr (std::unsigned_integral<Member> && !std::same_as<Member, bool>)
                integer_defaults.push_back(static_cast<std::uint64_t>(value));
        });
    CHECK((settings_default_ids == std::vector<std::uint64_t>{application_settings_field_stable_id("inherited_limit"),
                                                              application_settings_field_stable_id("persisted_revision"),
                                                              application_settings_field_stable_id("derived_enabled")}));
    CHECK((integer_defaults == std::vector<std::uint64_t>{6U, 17U}));
}

TEST_CASE("native application fingerprint contribution changes with an inherited field policy",
          "[controller][browser][reflection][inheritance]") {
    struct FieldContribution final {
        std::array<std::uint64_t, 2U> words;
        std::uint64_t endpoint_id = 0U;
        std::uint64_t field_id = 0U;
        std::string_view name;
    };
    const auto inherited_field_contribution = []<class Systems, class InheritedBase>() {
        application_schema_detail::FingerprintSink sink;
        bool appended = false;
        FieldContribution result{};
        ApplicationSchema<Systems>::VisitEndpoints([&]<class Endpoint>() {
            ApplicationSchema<Systems>::template VisitRequestFields<Endpoint>(
                [&]<class Owner, class Declaration>(const ApplicationRequestFieldFact& field) {
                    if constexpr (std::same_as<Owner, InheritedBase>) {
                        REQUIRE(field.name == "inherited_limit");
                        application_schema_detail::append_request_field_fingerprint<Declaration>(sink, field);
                        appended = true;
                        result.endpoint_id = field.endpoint_id;
                        result.field_id = field.stable_id;
                        result.name = field.name;
                    }
                });
        });
        REQUIRE(appended);
        result.words = sink.words();
        return result;
    };

    const auto inherited = inherited_field_contribution.template operator()<InheritedSystems, InheritedRequestBase>();
    const auto repeated = inherited_field_contribution.template operator()<InheritedSystems, InheritedRequestBase>();
    // CLEANUP-IGNORE: This independent schema-change oracle is unrelated to the import-channel provenance field assertions.
    const auto changed = inherited_field_contribution.template operator()<ChangedInheritedSystems, ChangedInheritedRequestBase>();
    CHECK(inherited.endpoint_id == repeated.endpoint_id);
    CHECK(inherited.field_id == repeated.field_id);
    CHECK(inherited.name == repeated.name);
    CHECK(inherited.words == repeated.words);
    CHECK(inherited.endpoint_id == changed.endpoint_id);
    CHECK(inherited.field_id == changed.field_id);
    CHECK(inherited.name == changed.name);
    CHECK(inherited.words != changed.words);
}

TEST_CASE("canonical schema publishes unique request and recursive settings identities", "[controller][browser][reflection]") {
    std::set<std::uint64_t> identities;
    ApplicationSchema<TestSystems>::VisitRequestFields([&]<class Owner, class Declaration>(const ApplicationRequestFieldFact& field) {
        REQUIRE(field.stable_id != 0U);
        CHECK(identities.insert(field.stable_id).second);
    });

    std::size_t dialog_count = 0U;
    std::size_t mutable_count = 0U;
    bool workspace_aspect_is_typed = false;
    bool explore_catalog_identity_is_persistence_metadata = false;
    bool opaque_override_storage_was_exposed = false;
    ApplicationSchema<mmltk::controller::ApplicationSystems>::VisitApplicationSettingsLeaves(
        [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsLeafFact& field) {
            REQUIRE(field.stable_id != 0U);
            REQUIRE_FALSE(field.path.empty());
            CHECK(identities.insert(field.stable_id).second);
            mutable_count += field.mutable_leaf ? 1U : 0U;
            if (field.path == "ui.workspace_aspect_ratio") {
                workspace_aspect_is_typed = std::same_as<std::remove_cvref_t<Member>, contracts::WorkspaceAspectRatio>;
            }
            if (field.path == "workflows.explore.class_catalog_identity") {
                explore_catalog_identity_is_persistence_metadata =
                    !field.mutable_leaf &&
                    field.stable_id == application_settings_field_stable_id("workflows.explore.class_catalog_identity");
            }
            opaque_override_storage_was_exposed =
                opaque_override_storage_was_exposed || field.path.find("recipe_overrides") != std::string_view::npos;
            if (!field.file_dialog) return;
            ++dialog_count;
            const auto entries = services::file_dialog_catalog().entries();
            const auto match = std::ranges::find(entries, field.stable_id, &services::FileDialogDescriptor::stable_id);
            REQUIRE(match != entries.end());
            CHECK(match->field_path.view() == field.path);
            CHECK(match->workflows == field.workflows);
            CHECK(match->mode == field.file_dialog->mode);
        });
    CHECK(mutable_count == contracts::settings_vocabulary::mutable_leaf_count<contracts::GuiSettingsState>());
    CHECK(workspace_aspect_is_typed);
    CHECK(explore_catalog_identity_is_persistence_metadata);
    CHECK_FALSE(opaque_override_storage_was_exposed);
    CHECK(dialog_count <= services::file_dialog_catalog().entries().size());

    std::size_t relation_count = 0U;
    ApplicationSchema<TestSystems>::VisitSettingsRelations<contracts::GuiSettingsState>(
        [&]<class Provider, class Relation, auto Selector>() {
            STATIC_REQUIRE(std::same_as<Provider, mmltk::backend::models::rfdetr::TrainRecipeCatalog>);
            STATIC_REQUIRE(Relation::member_count == 11U);
            constexpr auto selector_path = mmltk::frameworks::reflection::reflected_member_path<contracts::GuiSettingsState, Selector>();
            CHECK(selector_path.view() == "workflows.train.request.optimizer");
            Relation::VisitMembers([&]<class Entry>() {
                constexpr auto rebased =
                    mmltk::frameworks::reflection::rebase_member_path<contracts::GuiSettingsState, typename Relation::destination_type>(
                        Selector, Entry::destination);
                constexpr auto path = mmltk::frameworks::reflection::reflected_member_path<contracts::GuiSettingsState, rebased>();
                CHECK(path.view().starts_with("workflows.train.request."));
                ++relation_count;
            });
        });
    CHECK(relation_count == 11U);
}

TEST_CASE("custom model dialogs derive from the canonical compatibility catalog", "[controller][browser][reflection][dialog][model]") {
    const auto entries = services::file_dialog_catalog().entries();
    std::size_t row_index = 0U;
    contracts::ModelSelectionRelation::VisitRows([&]<class Relation>(const auto& compatibility) {
        const auto typed_path = mmltk::frameworks::reflection::reflected_member_path<contracts::GuiSettingsState, Relation::artifact>();
        REQUIRE(row_index < contracts::kModelSelectionCompatibility.size());
        CHECK(typed_path.view() == compatibility.artifact_field_path);
        CHECK(std::ranges::count(contracts::kModelSelectionCompatibility, compatibility) == 1);
        ++row_index;
        if (!compatibility.custom_allowed) return;
        std::size_t leaf_matches = 0U;
        ApplicationSchema<mmltk::controller::ApplicationSystems>::VisitSettingsLeaves<contracts::GuiSettingsState>(
            [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsLeafFact& field) {
                if (field.path == typed_path.view()) {
                    ++leaf_matches;
                    CHECK(field.stable_id == application_settings_field_stable_id(typed_path.view()));
                }
            });
        CHECK(leaf_matches == 1U);
        const auto found = std::ranges::find_if(entries, [&](const services::FileDialogDescriptor& entry) {
            return entry.workflows.allows(compatibility.workflow) && entry.model_input == compatibility.input;
        });
        CHECK(found != entries.end());
        CHECK(found->stable_id == application_settings_field_stable_id(typed_path.view()));
        CHECK(found->field_path.view() == typed_path.view());
        CHECK(found->title.view() == compatibility.dialog_title);
        CHECK(found->filter.name.view() == compatibility.dialog_filter);
        CHECK(found->filter.pattern.view() == compatibility.dialog_pattern);
    });
    CHECK(row_index == 9U);
}

TEST_CASE("export ONNX input and output publish distinct canonical dialogs", "[controller][browser][reflection][dialog]") {
    const auto entries = services::file_dialog_catalog().entries();
    const auto find_path = [&](const std::string_view path) {
        return std::ranges::find(entries, path, [](const services::FileDialogDescriptor& entry) { return entry.field_path.view(); });
    };
    const auto input = find_path("workflows.export_state.onnx_input_path");
    const auto output = find_path("workflows.export_state.onnx_output_path");
    REQUIRE(input != entries.end());
    REQUIRE(output != entries.end());
    CHECK(input->stable_id == application_settings_field_stable_id(input->field_path.view()));
    CHECK(output->stable_id == application_settings_field_stable_id(output->field_path.view()));
    CHECK(input->stable_id != output->stable_id);
    CHECK(input->mode == contracts::FileDialogMode::OpenFile);
    CHECK(input->model_input == mmltk::backend::models::catalog::ModelArtifactInputKind::Onnx);
    CHECK(output->mode == contracts::FileDialogMode::SaveFile);
    CHECK_FALSE(output->model_input);
}

TEST_CASE("canonical schema projects typed catalog rows and settings defaults", "[controller][browser][reflection]") {
    std::set<std::uint64_t> identities;
    std::size_t provider_count = 0U;
    std::size_t catalog_row_count = 0U;
    ApplicationSchema<mmltk::controller::ApplicationSystems>::VisitCatalogProviders(
        [&]<class Provider, class Row>(const ApplicationCatalogProviderFact& provider) {
            ++provider_count;
            REQUIRE(provider.stable_id != 0U);
            REQUIRE_FALSE(provider.identity.empty());
            REQUIRE_FALSE(provider.row_type.empty());
            CHECK(identities.insert(provider.stable_id).second);
            ApplicationSchema<mmltk::controller::ApplicationSystems>::template VisitCatalogRows<Provider>(
                [&]<class ActualProvider, class ActualRow>(const ApplicationCatalogRowFact& row, const ActualRow& value) {
                    STATIC_REQUIRE(std::same_as<Row, ActualRow>);
                    ++catalog_row_count;
                    CHECK(row.provider_id == provider.stable_id);
                    REQUIRE(row.stable_id != 0U);
                    REQUIRE_FALSE(row.key.empty());
                    CHECK(identities.insert(row.stable_id).second);
                    CHECK(row.key == Provider::row_key(value));
                });
        });
    CHECK(provider_count != 0U);
    CHECK(catalog_row_count != 0U);

    std::set<std::uint64_t> default_ids;
    std::size_t default_count = 0U;
    ApplicationSchema<mmltk::controller::ApplicationSystems>::VisitApplicationSettingsDefaults(
        [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsDefaultFact& fact, const Member& value) {
            ++default_count;
            REQUIRE(fact.stable_id != 0U);
            REQUIRE_FALSE(fact.path.empty());
            CHECK(default_ids.insert(fact.stable_id).second);
            CHECK(mmltk::frameworks::serialization::reflected_value(value).has_value());
        });
    CHECK(default_count == contracts::settings_vocabulary::leaf_count<contracts::GuiSettingsState>());

    std::size_t request_default_count = 0U;
    ApplicationSchema<TestSystems>::VisitEndpoints([&]<class Endpoint>() {
        ApplicationSchema<TestSystems>::template VisitRequestDefaults<Endpoint>(
            [&]<class Owner, class Declaration, class Member>(const ApplicationRequestFieldFact& fact, const Member& value) {
                ++request_default_count;
                REQUIRE(fact.endpoint_id != 0U);
                REQUIRE(fact.stable_id != 0U);
                REQUIRE_FALSE(fact.name.empty());
                CHECK(mmltk::frameworks::serialization::reflected_value(value).has_value());
            });
    });
    std::size_t request_field_count = 0U;
    ApplicationSchema<TestSystems>::VisitRequestFields(
        [&]<class Owner, class Declaration>(const ApplicationRequestFieldFact&) { ++request_field_count; });
    CHECK(request_default_count == request_field_count);
}

TEST_CASE("model selection compatibility is a reachable deterministic nine-row catalog", "[controller][browser][reflection][model]") {
    constexpr std::array expected_keys{
        std::string_view{"train.weights"},     std::string_view{"validate.weights"}, std::string_view{"validate.onnx"},
        std::string_view{"validate.tensorrt"}, std::string_view{"predict.weights"},  std::string_view{"predict.onnx"},
        std::string_view{"predict.tensorrt"},  std::string_view{"export.weights"},   std::string_view{"export.onnx"},
    };
    STATIC_REQUIRE(contracts::ModelSelectionCompatibilityCatalog::identity == "model.selection.compatibility");
    STATIC_REQUIRE(contracts::ModelSelectionCompatibilityCatalog::valid());

    std::size_t provider_count = 0U;
    ApplicationSchema<mmltk::controller::ApplicationSystems>::VisitCatalogProviders(
        [&]<class Provider, class Row>(const ApplicationCatalogProviderFact& provider) {
            if constexpr (std::same_as<Provider, contracts::ModelSelectionCompatibilityCatalog>) {
                STATIC_REQUIRE(std::same_as<Row, contracts::ModelSelectionCompatibility>);
                ++provider_count;
                CHECK(provider.identity == contracts::ModelSelectionCompatibilityCatalog::identity);
                CHECK(provider.row_type == mmltk::frameworks::reflection::type_name<contracts::ModelSelectionCompatibility>());
                CHECK(provider.stable_id == application_stable_id(contracts::ModelSelectionCompatibilityCatalog::identity));
                std::size_t row_index = 0U;
                ApplicationSchema<mmltk::controller::ApplicationSystems>::template VisitCatalogRows<Provider>(
                    [&]<class ActualProvider, class ActualRow>(const ApplicationCatalogRowFact& fact, const ActualRow& row) {
                        STATIC_REQUIRE(std::same_as<ActualProvider, Provider>);
                        STATIC_REQUIRE(std::same_as<ActualRow, Row>);
                        REQUIRE(row_index < expected_keys.size());
                        CHECK(row.key == expected_keys[row_index]);
                        CHECK(fact.key == expected_keys[row_index]);
                        CHECK_FALSE(row.artifact_field_path.empty());
                        CHECK_FALSE(row.dialog_title.empty());
                        CHECK_FALSE(row.dialog_filter.empty());
                        CHECK_FALSE(row.dialog_pattern.empty());
                        CHECK(fact.index == row_index);
                        CHECK(fact.stable_id == application_stable_id(provider.identity, fact.key));
                        ++row_index;
                    });
                CHECK(row_index == expected_keys.size());
            }
        });
    CHECK(provider_count == 1U);

    for (std::uint8_t workflow_index = 0U; workflow_index <= static_cast<std::uint8_t>(contracts::FeatureId::Explore); ++workflow_index) {
        const auto workflow = static_cast<contracts::FeatureId>(workflow_index);
        CHECK(contracts::ModelSelectionRequest{.workflow = workflow}.valid() == contracts::model_selection_workflow_supported(workflow));
        for (const auto source : {contracts::ModelSelectionSource::Canonical, contracts::ModelSelectionSource::Custom}) {
            for (const auto input : {contracts::ModelArtifactInputKind::Weights, contracts::ModelArtifactInputKind::Onnx,
                                     contracts::ModelArtifactInputKind::TensorRt, contracts::ModelArtifactInputKind::None}) {
                const contracts::ModelSelectionKey key{
                    .workflow = workflow, .source = source, .input = input, .preset = "rf-detr-nano", .resolution = 1U};
                CHECK(key.valid() == contracts::model_selection_compatible(workflow, source, input));
            }
        }
    }
}

TEST_CASE("protocol-16 Bootstrap contains fingerprint and current snapshots only", "[controller][browser][reflection]") {
    CounterSystem counter;
    TestSettingsSystem settings;
    const auto bootstrap = materialize_bootstrap(TestSettingsSystems{.settings = &settings, .counter = &counter});
    CHECK(bootstrap.protocol_version == 16U);
    CHECK(bootstrap.input_epoch == 0U);  // The physical host installs the peer identity before encoding.
    CHECK(bootstrap.schema_fingerprint == application_schema_fingerprint<TestSettingsSystems>().words);
    REQUIRE(bootstrap.snapshots.size() == 2U);
    CHECK(bootstrap.snapshots[0].system_id == application_stable_id("settings"));
    CHECK(bootstrap.snapshots[1].system_id == application_stable_id("counter"));
}

}  // namespace
}  // namespace mmltk::controller::browser

TEST_CASE("Workspace graphics projection derives every native field offset without application codecs",
          "[browser][workspace][generation]") {
    std::ostringstream output;
    mmltk::controller::browser::ApplicationWorkspaceAbiEmitter(output).Emit();
    const auto generated = output.str();
    CHECK(generated.find("pub const ABI_VERSION: u32 = " +
                         std::to_string(mmltk::controller::presentation::detail::workspace_surface_import::kAbiVersion) + ";") !=
          std::string::npos);
    CHECK(generated.find("pub opcode: u32") != std::string::npos);
    CHECK(generated.find("pub sequence_lock: u64") != std::string::npos);
    CHECK(generated.find("offset_of!(WorkspaceFrameSignal, content_height) == 60") != std::string::npos);
    CHECK(generated.find("offset_of!(Record, presentation_revision) == 64") != std::string::npos);
    CHECK(generated.find("Atomic") == std::string::npos);
    CHECK(generated.find("cbor") == std::string::npos);
    CHECK(generated.find("application_bindings") == std::string::npos);
}

TEST_CASE("Maximum Annotation logical and distinct displayed facts retain the existing wire budgets", "[browser][annotation][capacity]") {
    using namespace mmltk::controller;
    namespace c = contracts;
    AnnotationSnapshot snapshot;
    snapshot.revision = snapshot.ui_revision = snapshot.input_document_epoch = 1U;
    snapshot.ready = true;
    auto& ui = snapshot.ui;
    ui.document_revision = ui.scene_revision = ui.interaction_revision = 1U;
    auto& scene = ui.scene;
    scene.document = c::WorkspaceResource::From(std::string(c::kWorkspaceResourceCapacity, 'd'), 1U);
    scene.frame_width = scene.frame_height = std::numeric_limits<std::uint16_t>::max();
    scene.frame_ready = true;
    scene.categories.resize(c::kAnnotationCategoryCapacity, {.value = std::string(c::kArtifactClassNameCapacity, 'c')});
    scene.palette.resize(c::kAnnotationCategoryCapacity, {.hue = 123.4567F, .saturation = 0.1234567F, .value = 0.7654321F});
    const c::AnnotationPoint point{1234.5678F, 2345.6789F};
    c::AnnotationObject object{
        .name = c::AnnotationText::From(std::string(c::kAnnotationNameCapacity, 'n')),
        .shape = c::AnnotationShape::Box,
        .box = {point, point},
        .point = point,
        .mask = {.cleanup_radius = std::numeric_limits<std::uint16_t>::max()},
        .sup = {.center = scene.palette.front(), .minus = scene.palette.front(), .plus = scene.palette.front()},
        .nosup = {.center = scene.palette.front(), .minus = scene.palette.front(), .plus = scene.palette.front()},
        .mask_points = std::vector<c::AnnotationPoint>(c::kAnnotationGeometryCapacity, point),
        .spline_knots = std::vector<c::AnnotationSplineKnot>(
            c::kAnnotationGeometryCapacity,
            {.point = point, .in = {.point = point, .enabled = true}, .out = {.point = point, .enabled = true}}),
        .skeleton_nodes = std::vector<c::AnnotationSkeletonNode>(
            c::kAnnotationGeometryCapacity, {.key = c::AnnotationText::From(std::string(c::kAnnotationNameCapacity, 'k')), .point = point}),
        .skeleton_edges = std::vector<c::AnnotationEdge>(c::kAnnotationGeometryCapacity, {.source = 6U, .target = 7U}),
        .category = static_cast<std::uint16_t>(c::kAnnotationCategoryCapacity - 1U),
    };
    scene.objects.resize(c::kAnnotationObjectCapacity, object);
    scene.objects.front().mask = {
        .runs = std::vector<c::AnnotationMaskRun>(c::kAnnotationMaskRunCapacity, {65534U, 65532U, 65534U}),
        .cleanup_radius = std::numeric_limits<std::uint16_t>::max(),
        .present = true,
    };
    ui.editor.selected_object = 0U;
    REQUIRE(ui.valid());
    // The aggregate mask bound belongs to each scene, not each object. A
    // retained drawable may differ at every point from the logical document.
    auto displayed = scene;
    for (auto& item : displayed.objects) {
        item.shape = c::AnnotationShape::Skeleton;
        item.point.x += 0.12345F;
        for (auto& knot : item.spline_knots)
            knot.in.point.x += 0.23456F;
    }
    displayed.objects.front().shape = c::AnnotationShape::Mask;
    snapshot.rendered_scene.document_epoch = 2U;
    snapshot.rendered_scene.scene_revision = 3U;
    c::project_annotation_geometry(snapshot.rendered_scene.geometry, displayed);
    snapshot.rendered_scene.identities.resize(c::kAnnotationObjectCapacity, std::numeric_limits<std::uint64_t>::max());
    snapshot.rendered.document_epoch = 2U;
    snapshot.rendered.scene_revision = 3U;
    snapshot.rendered.editor = ui.editor;
    snapshot.rendered.selected.emplace();
    snapshot.rendered.selected_identity = c::AnnotationObjectIdentity{
        .object = std::numeric_limits<std::uint64_t>::max(),
        .elements = std::vector<std::uint64_t>(c::kAnnotationGeometryCapacity, std::numeric_limits<std::uint64_t>::max())};
    c::project_annotation_geometry(*snapshot.rendered.selected, displayed.objects.front());
    snapshot.rendered.preview.emplace();
    c::project_annotation_geometry(*snapshot.rendered.preview, displayed.objects.front());
    snapshot.rendered.preview_object = 0U;
    REQUIRE(snapshot.rendered_scene.geometry.objects.front().mask.has_value());
    CHECK(snapshot.rendered_scene.geometry.objects.front().mask->runs == displayed.objects.front().mask.runs);
    CHECK(snapshot.rendered.selected->spline_knots == displayed.objects.front().spline_knots);
    CHECK(snapshot.rendered_scene.geometry.objects.back().skeleton_nodes.back().point ==
          displayed.objects.back().skeleton_nodes.back().point);
    CHECK_FALSE(snapshot.rendered_scene.geometry.objects.back().box.has_value());
    CHECK_FALSE(snapshot.rendered_scene.geometry.objects.back().point.has_value());
    CHECK_FALSE(snapshot.rendered_scene.geometry.objects.back().mask.has_value());
    CHECK(snapshot.rendered_scene.geometry.objects.back().spline_knots.empty());
    CHECK_FALSE(ui.scene.objects.back().spline_knots.empty());

    auto value = browser::application_materializer_detail::reflected_value(snapshot);
    REQUIRE(value.has_value());
    browser::wire::ByteBuffer encoded;
    REQUIRE(browser::wire::encode(*value, encoded,
                                  {.max_bytes = browser::kMaxOutputValueBytes,
                                   .max_items = browser::kMaxOutputValueItems,
                                   .max_depth = browser::kMaxIntentValueDepth}));
    CHECK(encoded.size() <= c::kAnnotationUiStateByteBudget);
    browser::Bootstrap bootstrap{
        .schema_fingerprint = browser::application_schema_fingerprint<ApplicationSystems>().words,
        .input_epoch = 1U,
        .snapshots = {{.system_id = browser::application_stable_id("annotation"), .value = std::move(*value)}},
    };
    // Reconnect may also retain the full editable source in Explore and its
    // transformed Upscale result while Annotation shows a distinct body scene.
    ExploreSnapshot explore;
    explore.scene = scene;
    UpscaleSnapshot upscale;
    upscale.scene = displayed;
    const auto retain = [&](std::string_view name, const auto& state) {
        auto retained = browser::application_materializer_detail::reflected_value(state);
        REQUIRE(retained.has_value());
        bootstrap.snapshots.push_back({.system_id = browser::application_stable_id(name), .value = std::move(*retained)});
    };
    retain("explore", explore);
    retain("upscale", upscale);
    REQUIRE(browser::encode_server_record(browser::ServerRecord{std::move(bootstrap)}, encoded));
    CHECK(encoded.size() < browser::kMaxRecordWireBytes);
}
