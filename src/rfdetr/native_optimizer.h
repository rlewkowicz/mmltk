#pragma once

#include "mmltk/rfdetr/train.h"

#include <torch/serialize.h>
#include <torch/torch.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace mmltk::rfdetr {

enum class NativeOptimizerBackend : std::uint8_t {
    eager,
    foreach,
    fused,
};

struct NativeAdamWGroupConfig {
    double lr = 0.0;
    double weight_decay = 0.0;
    bool amsgrad = false;
};

class NativeAdamW {
public:
    struct Group {
        NativeAdamWGroupConfig config;
        std::vector<size_t> param_indices;
    };

    struct NamedParameter {
        std::string name;
        torch::Tensor tensor;
    };

    NativeAdamW() = default;
    NativeAdamW(std::vector<Group> groups,
                std::vector<NamedParameter> params,
                NativeOptimizerBackend backend);

    std::vector<torch::Tensor>& parameters();
    [[nodiscard]] const std::vector<torch::Tensor>& parameters() const;
    [[nodiscard]] const std::vector<std::string>& parameter_names() const;
    [[nodiscard]] const std::vector<Group>& groups() const;

    [[nodiscard]] NativeOptimizerBackend backend() const;
    [[nodiscard]] const char* backend_name() const;

    void zero_grad(bool set_to_none);
    void set_lrs(const std::vector<double>& base_lrs, double scale);
    void step();

    void save(torch::serialize::OutputArchive& archive) const;
    void load(torch::serialize::InputArchive& archive);

private:
    struct ParamState {
        torch::Tensor step;
        torch::Tensor exp_avg;
        torch::Tensor exp_avg_sq;
        torch::Tensor max_exp_avg_sq;
    };

    void initialize_state();
    void step_group_eager(const Group& group);
    void step_group_foreach(const Group& group);
    void step_group_fused(const Group& group);

    std::vector<Group> groups_;
    std::vector<NamedParameter> params_;
    std::vector<ParamState> state_;
    std::vector<torch::Tensor> all_params_;
    std::vector<std::string> all_param_names_;
    NativeOptimizerBackend backend_ = NativeOptimizerBackend::eager;
};

bool native_optimizer_supports_foreach(const std::vector<torch::Tensor>& params);
bool native_optimizer_supports_fused(const std::vector<torch::Tensor>& params);
const char* native_optimizer_backend_name(NativeOptimizerBackend backend);

struct NativeMuonGroupConfig {
    double lr = 0.0;
    double weight_decay = 0.0;
    double momentum = 0.95;
    bool use_muon = false;
    bool nesterov = true;
};

class NativeMuonWithAuxAdam {
public:
    struct Group {
        NativeMuonGroupConfig config;
        std::vector<size_t> param_indices;
    };

    struct NamedParameter {
        std::string name;
        torch::Tensor tensor;
    };

    NativeMuonWithAuxAdam() = default;
    NativeMuonWithAuxAdam(std::vector<Group> groups,
                          std::vector<NamedParameter> params);

    std::vector<torch::Tensor>& parameters();
    [[nodiscard]] const std::vector<torch::Tensor>& parameters() const;
    [[nodiscard]] const std::vector<std::string>& parameter_names() const;
    [[nodiscard]] const std::vector<Group>& groups() const;

    [[nodiscard]] const char* backend_name() const;

    void zero_grad(bool set_to_none);
    void set_lrs(const std::vector<double>& base_lrs, double scale);
    void set_muon_momentum(double momentum);
    void step();

    void save(torch::serialize::OutputArchive& archive) const;
    void load(torch::serialize::InputArchive& archive);

private:
    struct ParamState {
        int64_t step = 0;
        torch::Tensor momentum_buffer;
        torch::Tensor exp_avg;
        torch::Tensor exp_avg_sq;
    };

    void initialize_state();

    std::vector<Group> groups_;
    std::vector<NamedParameter> params_;
    std::vector<ParamState> state_;
    std::vector<torch::Tensor> all_params_;
    std::vector<std::string> all_param_names_;
};

class NativeOptimizer {
public:
    NativeOptimizer() = default;
    explicit NativeOptimizer(NativeAdamW optimizer);
    explicit NativeOptimizer(NativeMuonWithAuxAdam optimizer);

    [[nodiscard]] TrainOptimizerKind kind() const;
    [[nodiscard]] const char* kind_name() const;
    [[nodiscard]] const char* backend_name() const;

    std::vector<torch::Tensor>& parameters();
    [[nodiscard]] const std::vector<torch::Tensor>& parameters() const;
    [[nodiscard]] const std::vector<std::string>& parameter_names() const;

    void zero_grad(bool set_to_none);
    void set_lrs(const std::vector<double>& base_lrs, double scale);
    void set_muon_momentum(double momentum);
    void step();

    void save(torch::serialize::OutputArchive& archive) const;
    void load(torch::serialize::InputArchive& archive);

private:
    std::variant<NativeAdamW, NativeMuonWithAuxAdam> storage_;
};

const char* train_optimizer_kind_name(TrainOptimizerKind kind);

} // namespace mmltk::rfdetr
