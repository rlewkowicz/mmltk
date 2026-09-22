#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <inplace_vector>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/controller/contracts/provider.h"
namespace mmltk::controller::services {
class VastProviderOwner;
class VastOperations;
struct VastProviderState;
// Copyable ordinary handle retaining one immutable provider configuration and
// operation implementation. No global registry or generation lookup is used.
class VastProviderClient final {
public:
 VastProviderClient() noexcept = default;
 [[nodiscard]] bool valid() const noexcept;
 [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
 [[nodiscard]] bool operator==(const VastProviderClient&) const noexcept = default;

private:
 explicit VastProviderClient(std::shared_ptr<const VastProviderState> state) noexcept : state_(std::move(state)) {}
 std::shared_ptr<const VastProviderState> state_;
 friend class VastProviderOwner;
 friend class VastClient;
};
struct VastRawOffer {
 int offer_id = 0;
 std::string gpu_name;
 int num_gpus = 0;
 double gpu_ram = 0.0;
 double dph = 0.0;
 double dlperf = 0.0;
 double dlperf_usd = 0.0;
 double reliability = 0.0;
 double inet_down = 0.0;
 double disk_space = 0.0;
 std::string geolocation;
};
struct VastOfferSummary : VastRawOffer {
 mmltk::controller::contracts::ProviderGpuFamily family = mmltk::controller::contracts::ProviderGpuFamily::A100;
};
struct VastBridgeConfig {
 std::filesystem::path python_executable;
 std::filesystem::path bridge_script_path;
 std::string api_key;
 std::chrono::milliseconds http_timeout{20'000};
};
enum class VastBridgeFailureKind : std::uint8_t {
 TimedOut = 0U,
 Cancelled = 1U,
 Api = 2U,
 Parse = 3U,
 ChildSetup = 4U,
 Process = 5U,
 Count = 6U,
};
class VastBridgeError final : public std::runtime_error {
public:
 VastBridgeError(VastBridgeFailureKind kind, const std::string& message);
 [[nodiscard]] VastBridgeFailureKind kind() const noexcept;

private:
 VastBridgeFailureKind kind_ = VastBridgeFailureKind::Process;
};
struct VastBridgeInvocation {
 std::chrono::milliseconds deadline{0};
 int cancellation_fd = -1;
 [[nodiscard]] bool valid() const noexcept { return deadline.count() > 0 && cancellation_fd >= 0; }
};
struct VastInstanceStateFields {
 int instance_id = 0;
 std::string actual_status;
 std::string current_state;
 std::string next_state;
 std::string intended_status;
 std::string image_uuid;
 std::string label;
 int num_gpus = 0;
 std::string gpu_name;
 std::string ssh_host;
 int ssh_port = 0;
 std::string public_ipaddr;
 std::string status_message;
 std::string jupyter_token;
 std::string ports;
 double duration_seconds = 0.0;
};
struct VastQueryConfig : VastBridgeConfig {
 int min_gpus = 4;
 std::size_t result_limit = 2;
};
struct VastLaunchTemplateOptions {
 std::optional<double> bid_price;
 std::optional<double> disk;
 std::string user;
 std::string login;
 std::string label;
 std::string onstart;
 std::string onstart_cmd;
 std::string entrypoint;
 bool ssh = false;
 bool jupyter = false;
 bool direct = false;
 std::string jupyter_dir;
 bool jupyter_lab = false;
 bool lang_utf8 = false;
 bool python_utf8 = false;
 std::string extra;
 std::string env;
 std::vector<std::string> args;
 bool force = false;
 bool cancel_unavail = false;
 std::string template_hash;
};
struct VastCreateInstanceResult {
 bool success = false;
 int offer_id = 0;
 int instance_id = 0;
 std::string instance_api_key;
};
struct VastInstanceInfo : VastInstanceStateFields {
 int machine_id = 0;
};
VastBridgeConfig make_vast_bridge_config(std::string_view api_key);
const char* remote_gpu_family_label(mmltk::controller::contracts::ProviderGpuFamily family);
std::string summarize_selected_remote_gpu_families(const std::vector<mmltk::controller::contracts::ProviderGpuFamily>& families);
std::vector<VastRawOffer> parse_vast_offer_payload(std::string_view payload);
VastLaunchTemplateOptions parse_vast_launch_template(std::string_view template_text);
VastCreateInstanceResult parse_vast_create_instance_payload(std::string_view payload);
VastInstanceInfo parse_vast_instance_payload(std::string_view payload);
std::vector<VastInstanceInfo> parse_vast_instances_payload(std::string_view payload);
std::vector<VastOfferSummary> rank_vast_offers(
 const std::vector<VastRawOffer>& offers, const std::vector<mmltk::controller::contracts::ProviderGpuFamily>& selected_families, std::size_t result_limit, int min_gpus);
VastCreateInstanceResult create_vast_instance(const VastBridgeConfig& config, int offer_id, std::string_view image, const VastLaunchTemplateOptions& options = {});
VastCreateInstanceResult create_vast_instance(const VastBridgeConfig& config, int offer_id, std::string_view image, const VastLaunchTemplateOptions& options, const VastBridgeInvocation& invocation);
VastInstanceInfo show_vast_instance(const VastBridgeConfig& config, int instance_id);
VastInstanceInfo show_vast_instance(const VastBridgeConfig& config, int instance_id, const VastBridgeInvocation& invocation);
std::vector<VastInstanceInfo> show_vast_instances(const VastBridgeConfig& config);
std::vector<VastInstanceInfo> show_vast_instances(const VastBridgeConfig& config, const VastBridgeInvocation& invocation);
std::string fetch_vast_instance_logs(const VastBridgeConfig& config, int instance_id, std::optional<std::size_t> tail_lines = std::nullopt);
std::string fetch_vast_instance_logs(const VastBridgeConfig& config, int instance_id, std::optional<std::size_t> tail_lines, const VastBridgeInvocation& invocation);
void start_vast_instance(const VastBridgeConfig& config, int instance_id, const VastBridgeInvocation& invocation);
void stop_vast_instance(const VastBridgeConfig& config, int instance_id, const VastBridgeInvocation& invocation);
std::vector<VastOfferSummary> query_vast_offers(const VastQueryConfig& config, const std::vector<mmltk::controller::contracts::ProviderGpuFamily>& selected_families);
std::vector<VastOfferSummary> query_vast_offers(
 const VastQueryConfig& config, const std::vector<mmltk::controller::contracts::ProviderGpuFamily>& selected_families, const VastBridgeInvocation& invocation);
}  // namespace mmltk::controller::services
namespace mmltk::controller::services {
inline constexpr std::size_t kVastOfferCapacity = 32U;
inline constexpr std::size_t kVastInventoryCapacity = 64U;
inline constexpr std::size_t kVastLogCapacity = std::size_t{64U} * 1024U;
inline constexpr std::size_t kVastLogTailLineLimit = 10'000U;
inline constexpr std::size_t kVastLaunchTokenCapacity = 128U;
inline constexpr std::size_t kVastImageCapacity = std::size_t{4U} * 1024U;
inline constexpr std::size_t kVastFieldCapacity = std::size_t{4U} * 1024U;
inline constexpr std::size_t kVastApiKeyCapacity = 512U;
inline constexpr std::size_t kVastTemplateCapacity = std::size_t{16U} * 1024U;
inline constexpr std::size_t kVastTemplateArgumentCapacity = std::size_t{4U} * 1024U;
inline constexpr std::size_t kVastTemplateArgumentCount = 64U;
// A provider operation receives a one-shot token while its system keeps the
// paired source. The duplicated eventfd makes cancellation observable by the
// bridge without retaining system state in worker work.
struct VastCancellationTag;
using VastCancellationSignal = mmltk::common::concurrency::EventCancellationSignal<VastCancellationTag>;
using VastCancellationSource = mmltk::common::concurrency::EventCancellationSource<VastCancellationTag, true>;
using VastCancellationToken = mmltk::common::concurrency::EventCancellationToken<VastCancellationTag>;
using VastOffers = std::inplace_vector<VastOfferSummary, mmltk::controller::contracts::kProviderOfferCapacity>;
using VastInventory = std::inplace_vector<VastInstanceInfo, kVastInventoryCapacity>;
[[nodiscard]] constexpr bool valid_vast_mutation(const mmltk::controller::contracts::ProviderMutation mutation) noexcept {
 switch (mutation) {
  case mmltk::controller::contracts::ProviderMutation::Create:
  case mmltk::controller::contracts::ProviderMutation::Start:
  case mmltk::controller::contracts::ProviderMutation::Stop: return true;
 }
 return false;
}
struct VastReconciliationRequest final {
 mmltk::controller::contracts::ProviderMutation mutation = mmltk::controller::contracts::ProviderMutation::Create;
 int instance_id = 0;
 std::string launch_token;
 [[nodiscard]] bool valid() const noexcept {
  if (!valid_vast_mutation(mutation)) return false;
  if (mutation == mmltk::controller::contracts::ProviderMutation::Create) { return instance_id == 0 && !launch_token.empty() && launch_token.size() <= kVastLaunchTokenCapacity; }
  return instance_id > 0 && launch_token.empty();
 }
};
struct VastReconciliation final {
 enum class Disposition : std::uint8_t { Applied, NotApplied, Inconclusive };
 Disposition disposition = Disposition::Inconclusive;
 std::optional<VastInstanceInfo> instance;
};
// One invocation-local proof of whether a provider mutation crossed its
// external-effect edge. VastClient alone may mark it, after provider-capability
// resolution, validation, and cancellation checks and immediately before
// invoking VastOperations.
class VastEffectAttempt final {
public:
 VastEffectAttempt() noexcept = default;
 VastEffectAttempt(const VastEffectAttempt&) = delete;
 VastEffectAttempt& operator=(const VastEffectAttempt&) = delete;
 VastEffectAttempt(VastEffectAttempt&&) = delete;
 VastEffectAttempt& operator=(VastEffectAttempt&&) = delete;
 [[nodiscard]] bool started() const noexcept { return started_; }

private:
 friend class VastClient;
 void MarkStarted() noexcept { started_ = true; }
 bool started_ = false;
};
// Owns typed Vast bridge invocation and the provider-facing mutation/query/log
// protocol. Workflow scheduling remains with the caller.
class VastClient final {
public:
 explicit VastClient(VastProviderClient provider) noexcept : provider_(provider) {}
 [[nodiscard]] VastOffers query(const mmltk::controller::contracts::ProviderPreferences& preferences, const VastCancellationToken& cancellation) const;
 [[nodiscard]] VastCreateInstanceResult create(
  int offer_id, const mmltk::controller::contracts::ProviderPreferences& preferences, std::string_view launch_token, VastEffectAttempt& attempt, const VastCancellationToken& cancellation) const;
 void mutate(mmltk::controller::contracts::ProviderMutation mutation, int instance_id, VastEffectAttempt& attempt, const VastCancellationToken& cancellation) const;
 [[nodiscard]] VastReconciliation reconcile(const VastReconciliationRequest& request, const VastCancellationToken& cancellation) const;
 [[nodiscard]] VastInstanceInfo instance(int instance_id, const VastCancellationToken& cancellation) const;
 [[nodiscard]] VastInventory instances(const VastCancellationToken& cancellation) const;
 [[nodiscard]] std::string logs(int instance_id, std::optional<std::size_t> tail, const VastCancellationToken& cancellation) const;

private:
 [[nodiscard]] const VastProviderState& ResolveProvider() const;
 VastProviderClient provider_{};
};
[[nodiscard]] mmltk::controller::contracts::ProviderQueryResult materialize_provider_query_result(std::span<const VastOfferSummary> offers);
}  // namespace mmltk::controller::services
