#include "rfdetr/native_optimizer.h"

#include <torch/serialize.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

using fastloader::rfdetr::NativeAdamW;
using fastloader::rfdetr::NativeAdamWGroupConfig;
using fastloader::rfdetr::NativeMuonGroupConfig;
using fastloader::rfdetr::NativeMuonWithAuxAdam;
using fastloader::rfdetr::NativeOptimizerBackend;
using fastloader::rfdetr::native_optimizer_supports_fused;

NativeAdamW make_optimizer(const torch::Tensor& param,
                           const NativeOptimizerBackend backend,
                           const double lr = 1.0e-3,
                           const double weight_decay = 1.0e-2) {
    return NativeAdamW(
        {NativeAdamW::Group{NativeAdamWGroupConfig{lr, weight_decay, false}, {0}}},
        {NativeAdamW::NamedParameter{"weight", param}},
        backend);
}

NativeMuonWithAuxAdam make_muon_optimizer(const torch::Tensor& param,
                                          const bool use_muon,
                                          const double lr = 1.0e-3,
                                          const double weight_decay = 1.0e-2,
                                          const double momentum = 0.95) {
    return NativeMuonWithAuxAdam(
        {NativeMuonWithAuxAdam::Group{
            NativeMuonGroupConfig{lr, weight_decay, momentum, use_muon, true},
            {0}}},
        {NativeMuonWithAuxAdam::NamedParameter{"weight", param}});
}

void assign_grad(torch::Tensor& param, const torch::Tensor& grad) {
    param.mutable_grad() = grad.clone();
}

void assert_close(const torch::Tensor& lhs, const torch::Tensor& rhs, const double atol, const double rtol) {
    assert(torch::allclose(lhs.cpu(), rhs.cpu(), rtol, atol));
}

torch::Tensor reference_zeropower(torch::Tensor grad, const int steps = 5) {
    assert(grad.dim() >= 2);
    auto update = grad.to(torch::kBFloat16);
    const bool transpose = grad.size(-2) > grad.size(-1);
    if (transpose) {
        update = update.transpose(-2, -1);
    }
    update = update / (update.flatten(-2).norm(2, -1, true).unsqueeze(-1) + 1.0e-7);
    for (int step = 0; step < steps; ++step) {
        auto gram = torch::matmul(update, update.transpose(-2, -1));
        auto poly = gram.mul(-4.7750).add(torch::matmul(gram, gram), 2.0315);
        update = update.mul(3.4445).add(torch::matmul(poly, update));
    }
    if (transpose) {
        update = update.transpose(-2, -1);
    }
    return update;
}

torch::Tensor reference_muon_update(torch::Tensor grad,
                                    torch::Tensor& momentum_buffer,
                                    const double beta = 0.95,
                                    const bool nesterov = true) {
    momentum_buffer.lerp_(grad, 1.0 - beta);
    auto update = nesterov ? grad.lerp_(momentum_buffer, beta) : momentum_buffer;
    if (update.dim() == 4) {
        update = update.view({update.size(0), -1});
    }
    update = reference_zeropower(update);
    update.mul_(std::sqrt(std::max(1.0, static_cast<double>(update.size(-2)) / static_cast<double>(update.size(-1)))));
    return update;
}

torch::Tensor reference_adam_update(torch::Tensor grad,
                                    torch::Tensor& exp_avg,
                                    torch::Tensor& exp_avg_sq,
                                    const int64_t step) {
    exp_avg.lerp_(grad, 1.0 - 0.9);
    exp_avg_sq.lerp_(grad.square(), 1.0 - 0.95);
    const auto exp_avg_corrected = exp_avg / (1.0 - std::pow(0.9, static_cast<double>(step)));
    const auto exp_avg_sq_corrected = exp_avg_sq / (1.0 - std::pow(0.95, static_cast<double>(step)));
    return exp_avg_corrected / (exp_avg_sq_corrected.sqrt() + 1.0e-10);
}

void test_eager_fused_parity() {
    if (!torch::cuda::is_available()) {
        return;
    }

    auto eager_param =
        torch::randn({16, 16}, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA)).requires_grad_(true);
    if (!native_optimizer_supports_fused({eager_param})) {
        return;
    }
    auto fused_param = eager_param.detach().clone().requires_grad_(true);

    auto eager = make_optimizer(eager_param, NativeOptimizerBackend::eager);
    auto fused = make_optimizer(fused_param, NativeOptimizerBackend::fused);

    for (int step = 0; step < 3; ++step) {
        auto grad = torch::randn_like(eager_param);
        assign_grad(eager_param, grad);
        assign_grad(fused_param, grad);
        eager.step();
        fused.step();
        eager.zero_grad(true);
        fused.zero_grad(true);
        assert_close(eager_param, fused_param, 1.0e-5, 1.0e-5);
    }
}

void run_roundtrip_for_backend(const NativeOptimizerBackend backend, const torch::Device& device) {
    auto reference_param =
        torch::randn({8, 8}, torch::TensorOptions().dtype(torch::kFloat32).device(device)).requires_grad_(true);
    auto resumed_param = reference_param.detach().clone().requires_grad_(true);

    auto reference = make_optimizer(reference_param, backend);
    auto resumed = make_optimizer(resumed_param, backend);

    auto first_grad = torch::randn_like(reference_param);
    assign_grad(reference_param, first_grad);
    reference.step();
    reference.zero_grad(true);

    {
        torch::NoGradGuard no_grad;
        resumed_param.copy_(reference_param.detach());
    }
    const fs::path archive_path =
        fs::temp_directory_path() /
        ("fastloader_rfdetr_native_optimizer_" + std::string(reference.backend_name()) + ".pt");
    {
        torch::serialize::OutputArchive archive;
        reference.save(archive);
        archive.save_to(archive_path.string());
    }
    {
        torch::serialize::InputArchive archive;
        archive.load_from(archive_path.string());
        resumed.load(archive);
    }

    auto next_grad = torch::randn_like(reference_param);
    assign_grad(reference_param, next_grad);
    assign_grad(resumed_param, next_grad);
    reference.step();
    resumed.step();
    reference.zero_grad(true);
    resumed.zero_grad(true);

    assert_close(reference_param, resumed_param, 1.0e-6, 1.0e-6);
    fs::remove(archive_path);
}

void test_optimizer_roundtrip() {
    run_roundtrip_for_backend(NativeOptimizerBackend::eager, torch::Device(torch::kCPU));

    if (!torch::cuda::is_available()) {
        return;
    }
    auto probe =
        torch::randn({1}, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA)).requires_grad_(true);
    if (!native_optimizer_supports_fused({probe})) {
        return;
    }
    run_roundtrip_for_backend(NativeOptimizerBackend::fused, torch::Device(torch::kCUDA));
}

void test_invalid_fused_request_fails_loud() {
    auto param =
        torch::randn({4, 4}, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU)).requires_grad_(true);
    auto optimizer = make_optimizer(param, NativeOptimizerBackend::fused);
    assign_grad(param, torch::randn_like(param));
    bool threw = false;
    try {
        optimizer.step();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

void run_muon_reference_parity(const torch::Device& device) {
    auto param =
        torch::randn({8, 6}, torch::TensorOptions().dtype(torch::kFloat32).device(device)).requires_grad_(true);
    auto reference_param = param.detach().clone();
    auto optimizer = make_muon_optimizer(param, /*use_muon=*/true);
    auto momentum_buffer = torch::zeros_like(reference_param);

    for (int step = 0; step < 3; ++step) {
        auto grad = torch::randn_like(param);
        assign_grad(param, grad);
        auto update = reference_muon_update(grad.detach().clone(), momentum_buffer, 0.95, true);
        {
            torch::NoGradGuard no_grad;
            reference_param.mul_(1.0 - 1.0e-3 * 1.0e-2);
            reference_param.add_(update.reshape_as(reference_param), -1.0e-3);
        }
        optimizer.step();
        optimizer.zero_grad(true);
        assert_close(param, reference_param, 2.0e-4, 2.0e-4);
    }
}

void run_auxadam_reference_parity(const torch::Device& device) {
    auto param =
        torch::randn({5, 5}, torch::TensorOptions().dtype(torch::kFloat32).device(device)).requires_grad_(true);
    auto reference_param = param.detach().clone();
    auto optimizer = make_muon_optimizer(param, /*use_muon=*/false);
    auto exp_avg = torch::zeros_like(reference_param);
    auto exp_avg_sq = torch::zeros_like(reference_param);

    for (int64_t step = 1; step <= 3; ++step) {
        auto grad = torch::randn_like(param);
        assign_grad(param, grad);
        auto update = reference_adam_update(grad.detach().clone(), exp_avg, exp_avg_sq, step);
        {
            torch::NoGradGuard no_grad;
            reference_param.mul_(1.0 - 1.0e-3 * 1.0e-2);
            reference_param.add_(update, -1.0e-3);
        }
        optimizer.step();
        optimizer.zero_grad(true);
        assert_close(param, reference_param, 1.0e-6, 1.0e-5);
    }
}

void test_muon_reference_parity() {
    run_muon_reference_parity(torch::Device(torch::kCPU));
    run_auxadam_reference_parity(torch::Device(torch::kCPU));
    if (!torch::cuda::is_available()) {
        return;
    }
    run_muon_reference_parity(torch::Device(torch::kCUDA));
    run_auxadam_reference_parity(torch::Device(torch::kCUDA));
}

void test_muon_roundtrip() {
    auto reference_param =
        torch::randn({8, 8}, torch::TensorOptions().dtype(torch::kFloat32)).requires_grad_(true);
    auto resumed_param = reference_param.detach().clone().requires_grad_(true);

    auto reference = make_muon_optimizer(reference_param, /*use_muon=*/true);
    auto resumed = make_muon_optimizer(resumed_param, /*use_muon=*/true);

    assign_grad(reference_param, torch::randn_like(reference_param));
    reference.step();
    reference.zero_grad(true);
    {
        torch::NoGradGuard no_grad;
        resumed_param.copy_(reference_param.detach());
    }

    const fs::path archive_path = fs::temp_directory_path() / "fastloader_rfdetr_native_muon.pt";
    {
        torch::serialize::OutputArchive archive;
        reference.save(archive);
        archive.save_to(archive_path.string());
    }
    {
        torch::serialize::InputArchive archive;
        archive.load_from(archive_path.string());
        resumed.load(archive);
    }

    auto next_grad = torch::randn_like(reference_param);
    assign_grad(reference_param, next_grad);
    assign_grad(resumed_param, next_grad);
    reference.step();
    resumed.step();
    reference.zero_grad(true);
    resumed.zero_grad(true);

    assert_close(reference_param, resumed_param, 1.0e-6, 1.0e-6);
    fs::remove(archive_path);
}

void test_optimizer_kind_mismatch_fails_loud() {
    auto adam_param = torch::randn({4, 4}, torch::TensorOptions().dtype(torch::kFloat32)).requires_grad_(true);
    auto muon_param = adam_param.detach().clone().requires_grad_(true);
    auto adam = make_optimizer(adam_param, NativeOptimizerBackend::eager);
    auto muon = make_muon_optimizer(muon_param, /*use_muon=*/true);
    const fs::path archive_path = fs::temp_directory_path() / "fastloader_rfdetr_optimizer_mismatch.pt";
    {
        torch::serialize::OutputArchive archive;
        adam.save(archive);
        archive.save_to(archive_path.string());
    }

    bool threw = false;
    try {
        torch::serialize::InputArchive archive;
        archive.load_from(archive_path.string());
        muon.load(archive);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    fs::remove(archive_path);
}

} // namespace

int main() {
    test_eager_fused_parity();
    test_optimizer_roundtrip();
    test_invalid_fused_request_fails_loud();
    test_muon_reference_parity();
    test_muon_roundtrip();
    test_optimizer_kind_mismatch_fails_loud();
    return 0;
}
