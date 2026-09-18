#pragma once
#include <stop_token>
#include "src/backend/models/rfdetr/core/model.h"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <torch/types.h>
namespace mmltk::backend::models::rfdetr {
class ModelEma final {
   public:
    struct ShadowCandidate {
        std::vector<torch::Tensor> tensors;
    };
    ModelEma(const std::vector<torch::Tensor>& model_params, double decay, double tau);
    ModelEma(ModelEma&&) noexcept = default;
    ModelEma& operator=(ModelEma&&) noexcept = default;
    void update();
    [[nodiscard]] int64_t completed_updates() const noexcept { return completed_updates_; }
    static void validate_cpu_shadow(const std::vector<torch::Tensor>& parameters, const std::vector<torch::Tensor>& cpu_shadow, std::stop_token stop = {});
    [[nodiscard]] static ModelEma from_cpu_shadow(const std::vector<torch::Tensor>& model_params, const std::vector<torch::Tensor>& cpu_shadow, double decay,
                                                  double tau, int64_t completed_updates);
    class Selection final {
       public:
        Selection(ModelEma&, NativeRfDetrModel&);
        ~Selection() noexcept;
        Selection(const Selection&) = delete;
        Selection& operator=(const Selection&) = delete;
        void restore();

       private:
        ModelEma* owner_;
        NativeRfDetrModel* module_;
        bool training_;
    };
    [[nodiscard]] const std::vector<torch::Tensor>& shadow_params() const noexcept;
    [[nodiscard]] ShadowCandidate stage_shadow_params(const std::vector<torch::Tensor>& shadow_params) const;
    void commit_shadow_params(ShadowCandidate candidate) noexcept;
    void load_shadow_params(const std::vector<torch::Tensor>& shadow_params);
    void copy_to(std::vector<torch::Tensor>& model_params) const;

   private:
    ModelEma(const std::vector<torch::Tensor>&, ShadowCandidate, double, double, int64_t);
    double decay_;
    double tau_;
    int64_t completed_updates_ = 0;
    std::vector<torch::Tensor> source_;
    std::vector<torch::Tensor> shadow_;
    std::vector<torch::Tensor> backup_;
    bool selected_ = false;
};
}  // namespace mmltk::backend::models::rfdetr
