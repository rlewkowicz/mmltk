module;
#include <cstdint>
#include <memory>

#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/core/class_layout.h"

export module mmltk.backend.models.rfdetr.core.model;

export namespace mmltk::backend::models::rfdetr {

class NativeRfDetrModel final {
   public:
    explicit NativeRfDetrModel(const NativeRfDetrConfig& config = {}, ModelClassLayout layout = {});
    ~NativeRfDetrModel();

    NativeRfDetrModel(const NativeRfDetrModel&) = delete;
    NativeRfDetrModel& operator=(const NativeRfDetrModel&) = delete;
    NativeRfDetrModel(NativeRfDetrModel&&) noexcept;
    NativeRfDetrModel& operator=(NativeRfDetrModel&&) noexcept;

    [[nodiscard]] const NativeRfDetrConfig& config() const noexcept;
    [[nodiscard]] const std::shared_ptr<const ResolvedClassLayout>& class_layout() const noexcept;
    [[nodiscard]] bool is_compiled(bool for_training) const noexcept;
    void optimize_for_inference(std::int32_t batch_size = 1, bool for_training = false, CompilationMode mode = CompilationMode::kSelective);
    [[nodiscard]] void* technical_handle() noexcept;
    [[nodiscard]] const void* technical_handle() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::backend::models::rfdetr
