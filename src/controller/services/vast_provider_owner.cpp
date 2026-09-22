#include "src/controller/services/vast_provider_owner.h"
#include <chrono>
namespace mmltk::controller::services {
namespace {
constexpr std::size_t kProviderFieldCapacity = 4U * 1024U;
constexpr std::size_t kProviderApiKeyCapacity = 512U;
class RuntimeVastOperations final : public VastOperations {
public:
 std::vector<VastOfferSummary> query(
  const VastQueryConfig& config, const std::vector<mmltk::controller::contracts::ProviderGpuFamily>& families, const VastBridgeInvocation& invocation) const override {
  return query_vast_offers(config, families, invocation);
 }
 VastCreateInstanceResult create(
  const VastBridgeConfig& config, const int offer, const std::string_view image, const VastLaunchTemplateOptions& options, const VastBridgeInvocation& invocation) const override {
  VastCreateInstanceResult result = create_vast_instance(config, offer, image, options, invocation);
  if (result.offer_id == 0) result.offer_id = offer;
  return result;
 }
 void start(const VastBridgeConfig& config, const int instance, const VastBridgeInvocation& invocation) const override { start_vast_instance(config, instance, invocation); }
 void stop(const VastBridgeConfig& config, const int instance, const VastBridgeInvocation& invocation) const override { stop_vast_instance(config, instance, invocation); }
 VastInstanceInfo show(const VastBridgeConfig& config, const int instance, const VastBridgeInvocation& invocation) const override { return show_vast_instance(config, instance, invocation); }
 std::vector<VastInstanceInfo> inventory(const VastBridgeConfig& config, const VastBridgeInvocation& invocation) const override { return show_vast_instances(config, invocation); }
 std::string logs(const VastBridgeConfig& config, const int instance, const std::optional<std::size_t> tail, const VastBridgeInvocation& invocation) const override {
  return fetch_vast_instance_logs(config, instance, tail, invocation);
 }
};
void validate_config(const VastBridgeConfig& config) {
 if (config.api_key.empty() || config.api_key.size() > kProviderApiKeyCapacity || config.python_executable.empty() || config.bridge_script_path.empty() ||
     config.python_executable.native().size() > kProviderFieldCapacity || config.bridge_script_path.native().size() > kProviderFieldCapacity ||
     config.http_timeout <= std::chrono::milliseconds::zero()) {
  throw std::invalid_argument("invalid Vast bridge configuration");
 }
}
}  // namespace
VastProviderOwner::VastProviderOwner(std::optional<VastBridgeConfig> config) {
 if (config) register_owner(std::move(*config), std::make_shared<RuntimeVastOperations>());
}
VastProviderOwner::VastProviderOwner(VastBridgeConfig config) { register_owner(std::move(config), std::make_shared<RuntimeVastOperations>()); }
VastProviderOwner::VastProviderOwner(VastBridgeConfig config, std::unique_ptr<const VastOperations> operations) {
 register_owner(std::move(config), std::shared_ptr<const VastOperations>{std::move(operations)});
}
void VastProviderOwner::register_owner(VastBridgeConfig config, std::shared_ptr<const VastOperations> operations) {
 validate_config(config);
 if (!operations) throw std::invalid_argument("Vast provider operations are unavailable");
 client_ = VastProviderClient{std::make_shared<const VastProviderState>(VastProviderState{.config = std::move(config), .operations = std::move(operations)})};
}
VastProviderClient VastProviderOwner::client() const noexcept { return client_; }
bool VastProviderClient::valid() const noexcept { return state_ && state_->operations; }
}  // namespace mmltk::controller::services
