#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <torch/torch.h>
namespace mmltk::backend::models::rfdetr {
class ModelEma final {
   public:
    struct ShadowCandidate {
        std::vector<torch::Tensor> tensors;
    };
    ModelEma(const std::vector<torch::Tensor>& model_params, double decay, double tau);
    ModelEma(ModelEma&&) noexcept = default;
    ModelEma& operator=(ModelEma&&) noexcept = default;
    void update(int64_t step);
    static void validate_cpu_shadow(const std::vector<torch::Tensor>& parameters, const std::vector<torch::Tensor>& cpu_shadow);
    [[nodiscard]] static ModelEma from_cpu_shadow(const std::vector<torch::Tensor>& model_params, const std::vector<torch::Tensor>& cpu_shadow, double decay,
                                                  double tau);
    class Selection final {
       public:
        Selection(ModelEma&, torch::nn::Module&);
        ~Selection() noexcept;
        Selection(const Selection&) = delete;
        Selection& operator=(const Selection&) = delete;
        void restore();

       private:
        ModelEma* owner_;
        torch::nn::Module* module_;
        bool training_;
    };
    [[nodiscard]] const std::vector<torch::Tensor>& shadow_params() const noexcept;
    [[nodiscard]] ShadowCandidate stage_shadow_params(const std::vector<torch::Tensor>& shadow_params) const;
    void commit_shadow_params(ShadowCandidate candidate) noexcept;
    void load_shadow_params(const std::vector<torch::Tensor>& shadow_params);
    void copy_to(std::vector<torch::Tensor>& model_params) const;

   private:
    ModelEma(const std::vector<torch::Tensor>&, ShadowCandidate, double, double);
    double decay_;
    double tau_;
    std::vector<torch::Tensor> source_;
    std::vector<torch::Tensor> shadow_;
    std::vector<torch::Tensor> backup_;
    bool selected_ = false;
};
}  // namespace mmltk::backend::models::rfdetr
