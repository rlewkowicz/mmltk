#pragma once
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "src/controller/services/vast_client.h"
namespace mmltk::controller::services {
// The sole injectable provider-I/O seam.
class VastOperations {
public:
 virtual ~VastOperations() = default;
 [[nodiscard]] virtual std::vector<VastOfferSummary> query(const VastQueryConfig&, const std::vector<mmltk::controller::contracts::ProviderGpuFamily>&,
                                                           const VastBridgeInvocation&) const = 0;
 [[nodiscard]] virtual VastCreateInstanceResult create(const VastBridgeConfig&, int, std::string_view, const VastLaunchTemplateOptions&,
                                                       const VastBridgeInvocation&) const = 0;
 virtual void start(const VastBridgeConfig&, int, const VastBridgeInvocation&) const = 0;
 virtual void stop(const VastBridgeConfig&, int, const VastBridgeInvocation&) const = 0;
 [[nodiscard]] virtual VastInstanceInfo show(const VastBridgeConfig&, int, const VastBridgeInvocation&) const = 0;
 [[nodiscard]] virtual std::vector<VastInstanceInfo> inventory(const VastBridgeConfig&, const VastBridgeInvocation&) const = 0;
 [[nodiscard]] virtual std::string logs(const VastBridgeConfig&, int, std::optional<std::size_t>, const VastBridgeInvocation&) const = 0;
};
struct VastProviderState final {
 VastBridgeConfig config;
 std::shared_ptr<const VastOperations> operations;
};
// Immutable ordinary owner for one validated provider configuration and
// concrete provider implementation. Clients share this state directly.
class VastProviderOwner final {
public:
 VastProviderOwner() noexcept = default;
 explicit VastProviderOwner(std::optional<VastBridgeConfig> config);
 explicit VastProviderOwner(VastBridgeConfig config);
 VastProviderOwner(VastBridgeConfig config, std::unique_ptr<const VastOperations> operations);
 ~VastProviderOwner() noexcept = default;
 VastProviderOwner(const VastProviderOwner&) = delete;
 VastProviderOwner& operator=(const VastProviderOwner&) = delete;
 VastProviderOwner(VastProviderOwner&&) = delete;
 VastProviderOwner& operator=(VastProviderOwner&&) = delete;
 [[nodiscard]] VastProviderClient client() const noexcept;

private:
 void register_owner(VastBridgeConfig config, std::shared_ptr<const VastOperations> operations);
 VastProviderClient client_{};
};
}  // namespace mmltk::controller::services
