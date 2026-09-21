#include "src/controller/services/vast_client.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <ranges>
#include <system_error>
#include <type_traits>
#include "src/controller/services/vast_provider_owner.h"
namespace mmltk::controller::services {
namespace contracts = mmltk::controller::contracts;
namespace {
VastOffers bounded_offers(std::vector<VastOfferSummary> source) {
 if (source.size() > kVastOfferCapacity) throw std::runtime_error("Vast offer query exceeds fixed result capacity");
 VastOffers result;
 for (auto& value : source) result.push_back(std::move(value));
 return result;
}
VastInventory bounded_instances(std::vector<VastInstanceInfo> source) {
 if (source.size() > kVastInventoryCapacity) throw std::runtime_error("Vast inventory exceeds fixed result capacity");
 VastInventory result;
 for (auto& value : source) result.push_back(std::move(value));
 return result;
}
[[nodiscard]] bool bounded_field(const std::string& value) noexcept { return value.size() <= kVastFieldCapacity; }
[[nodiscard]] bool valid_family(const mmltk::controller::contracts::ProviderGpuFamily family) noexcept {
 return static_cast<std::underlying_type_t<mmltk::controller::contracts::ProviderGpuFamily>>(family) <=
        static_cast<std::underlying_type_t<mmltk::controller::contracts::ProviderGpuFamily>>(mmltk::controller::contracts::ProviderGpuFamily::LSeries);
}
void validate_query(const VastQueryConfig& config, const std::vector<mmltk::controller::contracts::ProviderGpuFamily>& families) {
 if (config.min_gpus <= 0 || config.result_limit == 0U || config.result_limit > kVastOfferCapacity || families.empty()) {
  throw std::invalid_argument("invalid Vast offer query");
 }
 for (std::size_t index = 0U; index < families.size(); ++index) {
  if (!valid_family(families[index]) || std::ranges::find(families.begin(), families.begin() + static_cast<std::ptrdiff_t>(index), families[index]) !=
                                         families.begin() + static_cast<std::ptrdiff_t>(index)) {
   throw std::invalid_argument("Vast query families must be unique and valid");
  }
 }
}
void validate_template(const VastLaunchTemplateOptions& options) {
 const std::array<const std::string*, 10U> fields{&options.user,       &options.login,       &options.label, &options.onstart, &options.onstart_cmd,
                                                  &options.entrypoint, &options.jupyter_dir, &options.extra, &options.env,     &options.template_hash};
 std::size_t total = 0U;
 for (const std::string* field : fields) {
  if (field->size() > kVastFieldCapacity || field->size() > kVastTemplateCapacity - total) {
   throw std::invalid_argument("Vast launch template exceeds fixed capacity");
  }
  total += field->size();
 }
 if (options.args.size() > kVastTemplateArgumentCount) throw std::invalid_argument("Vast launch arguments exceed fixed capacity");
 for (const std::string& argument : options.args) {
  if (argument.size() > kVastTemplateArgumentCapacity || argument.size() > kVastTemplateCapacity - total) {
   throw std::invalid_argument("Vast launch template exceeds fixed capacity");
  }
  total += argument.size();
 }
 const auto valid_number = [](const std::optional<double> value) { return !value || (std::isfinite(*value) && *value >= 0.0); };
 if (!valid_number(options.bid_price) || !valid_number(options.disk)) throw std::invalid_argument("invalid Vast launch numeric option");
}
void validate_create_result(const VastCreateInstanceResult& result, const int requested_offer_id) {
 // A returned record is either a completed allocation for exactly the
 // requested offer, or an unsuccessful request with no allocated resource
 // or credential.  The runtime raises provider failures directly, while
 // this validation also protects alternate transports at the service edge.
 if (result.offer_id != requested_offer_id || result.instance_api_key.size() > kVastApiKeyCapacity) {
  throw std::runtime_error("Vast create result exceeds the public service contract");
 }
 if (result.success) {
  if (result.instance_id <= 0) { throw std::runtime_error("Vast create success is missing an instance id"); }
  return;
 }
 if (result.instance_id != 0 || !result.instance_api_key.empty()) {
  throw std::runtime_error("Vast create failure contains an allocated instance or credential");
 }
}
void validate_offer(const VastOfferSummary& offer) {
 if (offer.offer_id <= 0 || !bounded_field(offer.gpu_name) || !bounded_field(offer.geolocation)) {
  throw std::runtime_error("Vast offer exceeds the public service contract");
 }
}
void validate_instance(const VastInstanceInfo& instance) {
 const std::array<const std::string*, 12U> fields{
  &instance.actual_status, &instance.current_state, &instance.next_state,    &instance.intended_status, &instance.image_uuid,    &instance.label,
  &instance.gpu_name,      &instance.ssh_host,      &instance.public_ipaddr, &instance.status_message,  &instance.jupyter_token, &instance.ports,
 };
 if (instance.instance_id <= 0 || instance.num_gpus < 0 || instance.ssh_port < 0 || !std::isfinite(instance.duration_seconds) ||
     instance.label.size() > kVastLaunchTokenCapacity ||
     std::ranges::any_of(fields, [](const std::string* field) { return field != nullptr && !bounded_field(*field); })) {
  throw std::runtime_error("Vast instance exceeds the public service contract");
 }
}
void validate_exact_instance(const VastInstanceInfo& instance, const int requested_instance_id) {
 validate_instance(instance);
 if (instance.instance_id != requested_instance_id) { throw std::runtime_error("Vast provider returned a different instance identity"); }
}
VastOffers bounded_offers(std::vector<VastOfferSummary> source, const std::size_t requested_limit) {
 if (source.size() > kVastOfferCapacity) { throw std::runtime_error("Vast offer query exceeds fixed result capacity"); }
 if (source.size() > requested_limit) throw std::runtime_error("Vast provider exceeded requested result limit");
 std::array<int, kVastOfferCapacity> offer_ids{};
 for (std::size_t index = 0U; index < source.size(); ++index) {
  const auto& offer = source[index];
  validate_offer(offer);
  if (!valid_family(offer.family) || offer.num_gpus <= 0 || !std::isfinite(offer.gpu_ram) || !std::isfinite(offer.dph) || !std::isfinite(offer.dlperf) ||
      !std::isfinite(offer.dlperf_usd) || !std::isfinite(offer.reliability) || !std::isfinite(offer.inet_down) || !std::isfinite(offer.disk_space)) {
   throw std::runtime_error("Vast offer is malformed");
  }
  offer_ids[index] = offer.offer_id;
 }
 const auto offer_ids_end = offer_ids.begin() + static_cast<std::ptrdiff_t>(source.size());
 std::ranges::sort(offer_ids.begin(), offer_ids_end);
 if (std::ranges::adjacent_find(offer_ids.begin(), offer_ids_end) != offer_ids_end) {
  throw std::runtime_error("Vast provider returned duplicate offer identities");
 }
 return bounded_offers(std::move(source));
}
VastInventory bounded_inventory(std::vector<VastInstanceInfo> source) {
 if (source.size() > kVastInventoryCapacity) { throw std::runtime_error("Vast inventory exceeds fixed result capacity"); }
 for (const auto& instance : source) validate_instance(instance);
 return bounded_instances(std::move(source));
}
[[nodiscard]] VastBridgeInvocation cancellation_invocation(const VastCancellationToken& cancellation, const std::chrono::milliseconds deadline) noexcept {
 return {.deadline = deadline, .cancellation_fd = cancellation.descriptor()};
}
}  // namespace
namespace {
void validate_create(const int offer_id, const std::string_view image, const std::string_view launch_token, const VastLaunchTemplateOptions& options) {
 if (offer_id <= 0 || image.empty() || image.size() > kVastImageCapacity || launch_token.empty() || launch_token.size() > kVastLaunchTokenCapacity ||
     (!options.label.empty() && options.label != launch_token)) {
  throw std::invalid_argument("invalid Vast create request");
 }
 auto labeled = options;
 labeled.label = launch_token;
 validate_template(labeled);
}
void validate_mutation(const mmltk::controller::contracts::ProviderMutation mutation, const int instance_id) {
 if (!valid_vast_mutation(mutation) || mutation == mmltk::controller::contracts::ProviderMutation::Create || instance_id <= 0) {
  throw std::invalid_argument("invalid Vast mutation");
 }
}
}  // namespace
contracts::ProviderQueryResult materialize_provider_query_result(const std::span<const VastOfferSummary> offers) {
 const auto failed = [](const std::string_view detail) {
  return contracts::ProviderQueryResult{.outcome = contracts::ProviderQueryOutcome::Failed, .detail = contracts::bounded_provider_detail(detail)};
 };
 if (offers.size() > contracts::kProviderOfferCapacity) return failed("provider returned too many offers");
 try {
  contracts::ProviderQueryResult result{.outcome = contracts::ProviderQueryOutcome::Succeeded};
  result.offers.reserve(offers.size());
  for (const auto& offer : offers) {
   contracts::ProviderOffer converted{.offer_id = offer.offer_id,
                                      .gpu_name = offer.gpu_name,
                                      .gpu_count = offer.num_gpus,
                                      .gpu_ram_gib = offer.gpu_ram,
                                      .hourly_price = offer.dph,
                                      .reliability = offer.reliability,
                                      .location = offer.geolocation,
                                      .family = offer.family};
   result.offers.push_back(std::move(converted));
  }
  return contracts::normalize_provider_query_result(std::move(result));
 } catch (const std::exception& error) { return failed(error.what()); } catch (...) {
  return failed("provider offer conversion failed");
 }
}
const VastProviderState& VastClient::ResolveProvider() const {
 if (!provider_.state_) throw std::runtime_error("Vast provider capability is unavailable");
 return *provider_.state_;
}
VastOffers VastClient::query(const mmltk::controller::contracts::ProviderPreferences& preferences, const VastCancellationToken& cancellation) const {
 const auto& provider = ResolveProvider();
 if (!preferences.valid()) throw std::invalid_argument("invalid provider preferences");
 VastQueryConfig config{};
 static_cast<VastBridgeConfig&>(config) = provider.config;
 config.min_gpus = preferences.minimum_gpus;
 config.result_limit = preferences.result_limit;
 std::vector<mmltk::controller::contracts::ProviderGpuFamily> families;
 families.reserve(preferences.families.size());
 for (const auto family : preferences.families) families.push_back(family);
 validate_query(config, families);
 if (cancellation.cancelled()) throw VastBridgeError(VastBridgeFailureKind::Cancelled, "Vast query cancelled");
 return bounded_offers(provider.operations->query(config, families, cancellation_invocation(cancellation, config.http_timeout)), config.result_limit);
}
VastCreateInstanceResult VastClient::create(const int offer_id, const mmltk::controller::contracts::ProviderPreferences& preferences,
                                            const std::string_view launch_token, VastEffectAttempt& attempt, const VastCancellationToken& cancellation) const {
 const auto& provider = ResolveProvider();
 if (!preferences.valid()) throw std::invalid_argument("invalid provider preferences");
 auto options = parse_vast_launch_template(preferences.template_text);
 validate_create(offer_id, preferences.image, launch_token, options);
 options.label = launch_token;
 if (cancellation.cancelled()) throw VastBridgeError(VastBridgeFailureKind::Cancelled, "Vast create cancelled");
 attempt.MarkStarted();
 VastCreateInstanceResult result =
  provider.operations->create(provider.config, offer_id, preferences.image, options, cancellation_invocation(cancellation, provider.config.http_timeout));
 validate_create_result(result, offer_id);
 return result;
}
void VastClient::mutate(const mmltk::controller::contracts::ProviderMutation mutation, const int instance_id, VastEffectAttempt& attempt,
                        const VastCancellationToken& cancellation) const {
 const auto& provider = ResolveProvider();
 validate_mutation(mutation, instance_id);
 if (cancellation.cancelled()) throw VastBridgeError(VastBridgeFailureKind::Cancelled, "Vast mutation cancelled");
 const auto invocation = cancellation_invocation(cancellation, provider.config.http_timeout);
 attempt.MarkStarted();
 if (mutation == mmltk::controller::contracts::ProviderMutation::Start) provider.operations->start(provider.config, instance_id, invocation);
 if (mutation == mmltk::controller::contracts::ProviderMutation::Stop) provider.operations->stop(provider.config, instance_id, invocation);
}
VastReconciliation VastClient::reconcile(const VastReconciliationRequest& request, const VastCancellationToken& cancellation) const {
 const auto& provider = ResolveProvider();
 if (!request.valid()) throw std::invalid_argument("invalid Vast reconciliation");
 if (cancellation.cancelled()) throw VastBridgeError(VastBridgeFailureKind::Cancelled, "Vast reconciliation cancelled");
 if (request.mutation == mmltk::controller::contracts::ProviderMutation::Create) {
  VastInventory inventory =
   bounded_inventory(provider.operations->inventory(provider.config, cancellation_invocation(cancellation, provider.config.http_timeout)));
  auto match = inventory.end();
  for (auto current = inventory.begin(); current != inventory.end(); ++current) {
   if (current->label != request.launch_token) continue;
   if (match != inventory.end()) return {.disposition = VastReconciliation::Disposition::Inconclusive, .instance = std::nullopt};
   match = current;
  }
  if (match == inventory.end()) return {.disposition = VastReconciliation::Disposition::NotApplied, .instance = std::nullopt};
  return {.disposition = VastReconciliation::Disposition::Applied, .instance = std::move(*match)};
 }
 VastInstanceInfo record = provider.operations->show(provider.config, request.instance_id, cancellation_invocation(cancellation, provider.config.http_timeout));
 validate_exact_instance(record, request.instance_id);
 std::string state = record.actual_status + " " + record.current_state + " " + record.intended_status + " " + record.next_state + " " + record.status_message;
 for (char& value : state) value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
 const bool running = state.contains("running") || state.contains("active") || state.contains("online");
 const bool stopped = state.contains("stopped") || state.contains("offline") || state.contains("exited") || state.contains("dead");
 VastReconciliation::Disposition disposition = VastReconciliation::Disposition::Inconclusive;
 if ((request.mutation == mmltk::controller::contracts::ProviderMutation::Start && running) ||
     (request.mutation == mmltk::controller::contracts::ProviderMutation::Stop && stopped))
  disposition = VastReconciliation::Disposition::Applied;
 else if ((request.mutation == mmltk::controller::contracts::ProviderMutation::Start && stopped) ||
          (request.mutation == mmltk::controller::contracts::ProviderMutation::Stop && running))
  disposition = VastReconciliation::Disposition::NotApplied;
 return {.disposition = disposition, .instance = std::move(record)};
}
VastInstanceInfo VastClient::instance(const int instance_id, const VastCancellationToken& cancellation) const {
 const auto& provider = ResolveProvider();
 if (instance_id <= 0) throw std::invalid_argument("invalid Vast instance query");
 if (cancellation.cancelled()) throw VastBridgeError(VastBridgeFailureKind::Cancelled, "Vast instance query cancelled");
 VastInstanceInfo result = provider.operations->show(provider.config, instance_id, cancellation_invocation(cancellation, provider.config.http_timeout));
 validate_exact_instance(result, instance_id);
 return result;
}
VastInventory VastClient::instances(const VastCancellationToken& cancellation) const {
 const auto& provider = ResolveProvider();
 if (cancellation.cancelled()) throw VastBridgeError(VastBridgeFailureKind::Cancelled, "Vast inventory cancelled");
 return bounded_inventory(provider.operations->inventory(provider.config, cancellation_invocation(cancellation, provider.config.http_timeout)));
}
std::string VastClient::logs(const int instance_id, const std::optional<std::size_t> tail, const VastCancellationToken& cancellation) const {
 const auto& provider = ResolveProvider();
 if (instance_id <= 0 || (tail && (*tail == 0U || *tail > kVastLogTailLineLimit))) throw std::invalid_argument("invalid Vast log request");
 if (cancellation.cancelled()) throw VastBridgeError(VastBridgeFailureKind::Cancelled, "Vast log query cancelled");
 auto output = provider.operations->logs(provider.config, instance_id, tail, cancellation_invocation(cancellation, provider.config.http_timeout));
 if (output.size() > kVastLogCapacity) output.erase(0U, output.size() - kVastLogCapacity);
 return output;
}
}  // namespace mmltk::controller::services
