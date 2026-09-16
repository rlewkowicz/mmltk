#pragma once
#include "src/controller/contracts/application_systems.h"
#include <memory>
#include "src/controller/browser/client_record.h"
#include "src/controller/services/runtime_diagnostics.h"
#include "src/frameworks/transport/browser_server.h"
namespace mmltk::controller {
struct ApplicationSystems;
class ExploreAcceptanceGate;
class PresentationAcceptanceGate;
}  // namespace mmltk::controller
namespace mmltk::controller::browser {
class ApplicationBrowserHost final {
   public:
    ApplicationBrowserHost(mmltk::frameworks::transport::BrowserServer&, services::RuntimeDiagnosticTarget = {});
    ApplicationBrowserHost(const ApplicationBrowserHost&) = delete;
    ApplicationBrowserHost& operator=(const ApplicationBrowserHost&) = delete;
    [[nodiscard]] bool install(ApplicationSystems&) noexcept;
    void install_integration(std::shared_ptr<ExploreAcceptanceGate>, std::shared_ptr<PresentationAcceptanceGate> = {});
    [[nodiscard]] mmltk::frameworks::transport::BrowserServer::Callbacks callbacks() const noexcept;
    void publish(SystemEvent) noexcept;
    void continuity_lost() noexcept;
    void close_admission() noexcept;
    [[nodiscard]] bool accepting() const noexcept;

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}  // namespace mmltk::controller::browser
