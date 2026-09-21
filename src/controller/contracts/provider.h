#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/controller/contracts/terminal_presentation.h"
#include "src/controller/contracts/workflows.h"
namespace mmltk::controller::contracts {
inline constexpr std::size_t kProviderOfferCapacity = 32U;
inline constexpr std::size_t kProviderFamilyCapacity = 5U;
inline constexpr std::size_t kProviderTextCapacity = 1024U;
inline constexpr std::size_t kProviderDetailCapacity = 4096U;
[[nodiscard]] inline std::string bounded_provider_detail(const std::string_view value) {
 const std::size_t size = value.size() < kProviderDetailCapacity ? value.size() : kProviderDetailCapacity;
 return {value.data(), size};
}
// This is the provider domain vocabulary shared by systems, worker input,
// snapshots, and browser projection. Vast runtime structs and
// provider configuration remain service-local.
enum class ProviderGpuFamily : std::uint8_t { A100, B200, H100, H200, LSeries };
[[nodiscard]] constexpr std::optional<ProviderGpuFamily> provider_gpu_family_from_index(const std::size_t index) noexcept {
 if (index > static_cast<std::size_t>(ProviderGpuFamily::LSeries)) return std::nullopt;
 return static_cast<ProviderGpuFamily>(index);
}
struct ProviderOffer final {
 int offer_id = 0;
 [[= mmltk::frameworks::reflection::MaxBytes{128U}]] std::string gpu_name{};
 int gpu_count = 0;
 double gpu_ram_gib = 0.0;
 double hourly_price = 0.0;
 double reliability = 0.0;
 [[= mmltk::frameworks::reflection::MaxBytes{128U}]] std::string location{};
 ProviderGpuFamily family = ProviderGpuFamily::A100;
 [[nodiscard]] bool valid() const noexcept {
  return offer_id > 0 && gpu_count > 0 && gpu_name.size() <= 128U && location.size() <= 128U && std::isfinite(gpu_ram_gib) && gpu_ram_gib >= 0.0 &&
         std::isfinite(hourly_price) && hourly_price >= 0.0 && std::isfinite(reliability) && reliability >= 0.0 && reliability <= 1.0 &&
         static_cast<std::uint8_t>(family) <= static_cast<std::uint8_t>(ProviderGpuFamily::LSeries);
 }
 bool operator==(const ProviderOffer&) const = default;
};
struct ProviderPreferences final {
 int minimum_gpus = 4;
 std::uint32_t result_limit = 2U;
 [[= mmltk::frameworks::reflection::MaxItems{kProviderFamilyCapacity}]] std::vector<ProviderGpuFamily> families{};
 [[= mmltk::frameworks::reflection::MaxBytes{kProviderTextCapacity}]] std::string image{};
 [[= mmltk::frameworks::reflection::MaxBytes{kProviderTextCapacity}]] std::string template_text{};
 [[nodiscard]] bool valid() const noexcept {
  if (minimum_gpus <= 0 || result_limit == 0U || result_limit > kProviderOfferCapacity || families.empty() || image.empty() ||
      image.size() > kProviderTextCapacity || template_text.size() > kProviderTextCapacity) {
   return false;
  }
  for (std::size_t index = 0U; index < families.size(); ++index) {
   const auto family = static_cast<std::uint8_t>(families[index]);
   if (family > static_cast<std::uint8_t>(ProviderGpuFamily::LSeries)) return false;
   for (std::size_t prior = 0U; prior < index; ++prior) {
    if (families[prior] == families[index]) return false;
   }
  }
  return true;
 }
};
struct[[= mmltk::controller::contracts::reflection::feature_scope(mmltk::controller::contracts::FeatureId::Train)]] ProviderQueryIntent final {};
struct[[= mmltk::controller::contracts::reflection::feature_scope(mmltk::controller::contracts::FeatureId::Train)]] ProviderOfferIdentity final {
 int offer_id = 0;
 [[nodiscard]] bool valid() const noexcept { return offer_id > 0; }
 bool operator==(const ProviderOfferIdentity&) const = default;
};
struct[[= mmltk::controller::contracts::reflection::feature_scope(mmltk::controller::contracts::FeatureId::Train)]] ProviderClearIntent final {};
struct[[= mmltk::controller::contracts::reflection::feature_scope(mmltk::controller::contracts::FeatureId::Train)]] ProviderStartIntent final {};
struct[[= mmltk::controller::contracts::reflection::feature_scope(mmltk::controller::contracts::FeatureId::Train)]] ProviderStopIntent final {};
enum class ProviderQueryOutcome : std::uint8_t { Idle, Succeeded, Failed, Cancelled, Rejected };
inline constexpr std::array kProviderQueryTerminalPresentations{
 terminal_presentation::Policy{ProviderQueryOutcome::Idle, terminal_presentation::Classification::Refused, "provider.query.idle",
                               "No provider query has completed."},
 terminal_presentation::Policy{ProviderQueryOutcome::Succeeded, terminal_presentation::Classification::Success, "provider.query.succeeded", ""},
 terminal_presentation::Policy{ProviderQueryOutcome::Failed, terminal_presentation::Classification::Failed, "provider.query.failed",
                               "The provider query failed."},
 terminal_presentation::Policy{ProviderQueryOutcome::Cancelled, terminal_presentation::Classification::Cancelled, "provider.query.cancelled",
                               "The provider query was cancelled."},
 terminal_presentation::Policy{ProviderQueryOutcome::Rejected, terminal_presentation::Classification::Refused, "provider.query.rejected",
                               "The provider query was rejected."},
};
static_assert(terminal_presentation::complete(kProviderQueryTerminalPresentations));
[[nodiscard]] consteval const auto& materialized_terminal_presentation_policy(std::type_identity<ProviderQueryOutcome>) {
 return kProviderQueryTerminalPresentations;
}
enum class RemoteSessionPhase : std::uint8_t { Absent, Running, Stopped, Unknown };
enum class RemoteOperationOutcome : std::uint8_t { Idle, Applied, Failed, Inconclusive, Cancelled, CancellationRequested, Refused };
inline constexpr std::array kRemoteOperationTerminalPresentations{
 terminal_presentation::Policy{RemoteOperationOutcome::Idle, terminal_presentation::Classification::Refused, "remote_operation.idle",
                               "No remote operation has completed."},
 terminal_presentation::Policy{RemoteOperationOutcome::Applied, terminal_presentation::Classification::Success, "remote_operation.applied", ""},
 terminal_presentation::Policy{RemoteOperationOutcome::Failed, terminal_presentation::Classification::Failed, "remote_operation.failed",
                               "The remote operation failed."},
 terminal_presentation::Policy{RemoteOperationOutcome::Inconclusive, terminal_presentation::Classification::Failed, "remote_operation.inconclusive",
                               "The remote operation outcome is inconclusive."},
 terminal_presentation::Policy{RemoteOperationOutcome::Cancelled, terminal_presentation::Classification::Cancelled, "remote_operation.cancelled",
                               "The remote operation was cancelled."},
 terminal_presentation::Policy{RemoteOperationOutcome::CancellationRequested, terminal_presentation::Classification::Cancelled,
                               "remote_operation.cancellation_requested", "Remote operation cancellation was requested."},
 terminal_presentation::Policy{RemoteOperationOutcome::Refused, terminal_presentation::Classification::Refused, "remote_operation.refused",
                               "The remote operation was refused."},
};
static_assert(terminal_presentation::complete(kRemoteOperationTerminalPresentations));
[[nodiscard]] consteval const auto& materialized_terminal_presentation_policy(std::type_identity<RemoteOperationOutcome>) {
 return kRemoteOperationTerminalPresentations;
}
enum class ProviderMutation : std::uint8_t { Create, Start, Stop };
enum class ProviderReconciliationDisposition : std::uint8_t { Applied, NotApplied, Inconclusive };
// These are the complete provider-worker results. A provider fault is a
// domain outcome, while process infrastructure failures remain distinct.
struct ProviderQueryResult final {
 ProviderQueryOutcome outcome = ProviderQueryOutcome::Failed;
 [[= mmltk::frameworks::reflection::MaxItems{kProviderOfferCapacity}]] std::vector<ProviderOffer> offers{};
 [[= mmltk::frameworks::reflection::MaxBytes{kProviderDetailCapacity}]] std::string detail{};
 [[nodiscard]] bool valid() const noexcept {
  if (offers.size() > kProviderOfferCapacity || detail.size() > kProviderDetailCapacity) return false;
  switch (outcome) {
   case ProviderQueryOutcome::Succeeded:
    if (!detail.empty()) return false;
    break;
   case ProviderQueryOutcome::Failed:
   case ProviderQueryOutcome::Cancelled:
   case ProviderQueryOutcome::Rejected: return offers.empty();
   case ProviderQueryOutcome::Idle: return false;
   default: return false;
  }
  for (auto current = offers.begin(); current != offers.end(); ++current) {
   if (!current->valid()) return false;
   for (auto prior = offers.begin(); prior != current; ++prior)
    if (prior->offer_id == current->offer_id) return false;
  }
  return true;
 }
};
[[nodiscard]] inline ProviderQueryResult normalize_provider_query_result(ProviderQueryResult result) {
 if (!result.valid()) return {.outcome = ProviderQueryOutcome::Failed, .detail = bounded_provider_detail("provider returned an invalid result")};
 result.detail = bounded_provider_detail(result.detail);
 return result;
}
struct ProviderEffectResult final {
 ProviderReconciliationDisposition disposition = ProviderReconciliationDisposition::Inconclusive;
 int instance_id = 0;
 bool cancelled = false;
 [[= mmltk::frameworks::reflection::MaxBytes{kProviderDetailCapacity}]] std::string detail{};
 [[nodiscard]] bool valid() const noexcept {
  if (instance_id < 0 || detail.size() > kProviderDetailCapacity) return false;
  switch (disposition) {
   case ProviderReconciliationDisposition::Applied: return !cancelled && detail.empty();
   case ProviderReconciliationDisposition::NotApplied: return instance_id == 0;
   case ProviderReconciliationDisposition::Inconclusive: return true;
  }
  return false;
 }
};
[[nodiscard]] inline ProviderEffectResult provider_effect_not_applied(const std::string_view detail, const bool cancelled = false) {
 return {.disposition = ProviderReconciliationDisposition::NotApplied, .instance_id = 0, .cancelled = cancelled, .detail = bounded_provider_detail(detail)};
}
[[nodiscard]] inline ProviderEffectResult provider_effect_inconclusive(const std::string_view detail, const bool cancelled = false) {
 return {.disposition = ProviderReconciliationDisposition::Inconclusive, .instance_id = 0, .cancelled = cancelled, .detail = bounded_provider_detail(detail)};
}
struct ProviderOfferState final {
 ProviderQueryOutcome outcome = ProviderQueryOutcome::Idle;
 std::uint64_t revision = 0U;
 bool cancellation_requested = false;
 [[= mmltk::frameworks::reflection::MaxItems{kProviderOfferCapacity}]] std::vector<ProviderOffer> offers{};
 std::optional<ProviderOfferIdentity> selected{};
 [[= mmltk::frameworks::reflection::MaxBytes{kProviderDetailCapacity}]] std::string detail{};
 bool operator==(const ProviderOfferState&) const = default;
};
struct RemoteSessionState final {
 RemoteSessionPhase phase = RemoteSessionPhase::Absent;
 RemoteOperationOutcome outcome = RemoteOperationOutcome::Idle;
 std::uint64_t revision = 0U;
 int instance_id = 0;
 bool reconciliation_pending = false;
 [[= mmltk::frameworks::reflection::MaxBytes{kProviderDetailCapacity}]] std::string detail{};
 // CLEANUP-IGNORE: Equality closes the canonical provider state declaration before its reflected inventory.
 bool operator==(const RemoteSessionState&) const = default;
};
// CLEANUP-IGNORE: Provider reflection entries are the canonical compile-time inventory for generated consumers.
MMLTK_REFLECT_FIELDS(ProviderOffer)
// CLEANUP-IGNORE: ProviderPreferences remains a separately named reflected domain type.
MMLTK_REFLECT_FIELDS(ProviderPreferences)
MMLTK_REFLECT_FIELDS(ProviderQueryIntent)
MMLTK_REFLECT_FIELDS(ProviderOfferIdentity)
MMLTK_REFLECT_FIELDS(ProviderClearIntent)
MMLTK_REFLECT_FIELDS(ProviderStartIntent)
MMLTK_REFLECT_FIELDS(ProviderStopIntent)
MMLTK_REFLECT_FIELDS(ProviderQueryResult)
MMLTK_REFLECT_FIELDS(ProviderEffectResult)
MMLTK_REFLECT_FIELDS(ProviderOfferState)
MMLTK_REFLECT_FIELDS(RemoteSessionState)
MMLTK_REFLECT_ENUM(ProviderGpuFamily)
MMLTK_REFLECT_ENUM(ProviderQueryOutcome)
MMLTK_REFLECT_ENUM(RemoteSessionPhase)
MMLTK_REFLECT_ENUM(RemoteOperationOutcome)
MMLTK_REFLECT_ENUM(ProviderMutation)
MMLTK_REFLECT_ENUM(ProviderReconciliationDisposition)
}  // namespace mmltk::controller::contracts
