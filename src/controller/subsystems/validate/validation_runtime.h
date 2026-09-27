#pragma once
#include "src/controller/services/diagnostics/runtime_diagnostics.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/controller/subsystems/system/compute_runtime.h"
namespace mmltk::controller {
struct ValidationRuntimeResult final {
 contracts::ComputeTerminal terminal{};
 std::filesystem::path report{};
 std::optional<mmltk::backend::models::rfdetr::ValidationBackendResult> evaluation{};
};
class ValidationRuntime {
public:
 virtual ~ValidationRuntime() = default;
 // Complete physical retirement; successful repeated Close is inert.
 virtual void Close() {}
 [[nodiscard]] virtual bool HasUnsafeCustody() const noexcept { return false; }
 [[nodiscard]] virtual ValidationRuntimeResult Run(
  mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token, const ComputeProgressSink&, const mmltk::backend::models::rfdetr::ValidationDelivery&, std::uint64_t generation = 0) = 0;
};
class CudaValidationRuntime final : public ValidationRuntime {
public:
 explicit CudaValidationRuntime(DirectComputeConfiguration, services::RuntimeDiagnosticTarget = {});
 ~CudaValidationRuntime() override;
 void Close() override;
 [[nodiscard]] bool HasUnsafeCustody() const noexcept override;
 [[nodiscard]] ValidationRuntimeResult Run(
  mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token, const ComputeProgressSink&, const mmltk::backend::models::rfdetr::ValidationDelivery&, std::uint64_t generation = 0) override;

private:
 services::RuntimeDiagnosticTarget diagnostics_;
 class Impl;
 std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::controller
