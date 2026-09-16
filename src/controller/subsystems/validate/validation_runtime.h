#pragma once
#include <memory>
#include <optional>
#include <stop_token>
#include "src/backend/models/rfdetr/inference/validate.h"
#include "src/controller/subsystems/system/compute_runtime.h"
namespace mmltk::controller {
struct ValidationRuntimeResult final {
    contracts::ComputeTerminal terminal{};
    std::optional<mmltk::backend::models::rfdetr::ValidationBackendResult> evaluation{};
};
class ValidationRuntime {
   public:
    virtual ~ValidationRuntime() = default;
    [[nodiscard]] virtual ValidationRuntimeResult Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token, const ComputeProgressSink&,
                                                      const mmltk::backend::models::rfdetr::ValidationDelivery&) = 0;
};
class CudaValidationRuntime final : public ValidationRuntime {
   public:
    explicit CudaValidationRuntime(DirectComputeConfiguration);
    ~CudaValidationRuntime() override;
    [[nodiscard]] ValidationRuntimeResult Run(mmltk::backend::models::rfdetr::ValidateRequest, std::stop_token, const ComputeProgressSink&,
                                              const mmltk::backend::models::rfdetr::ValidationDelivery&) override;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace mmltk::controller
