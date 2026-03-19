#include "rfdetr/native_optimizer.h"

#include <torch/serialize.h>

#include <cassert>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

using fastloader::rfdetr::NativeAdamW;
using fastloader::rfdetr::NativeAdamWGroupConfig;
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

void assign_grad(torch::Tensor& param, const torch::Tensor& grad) {
    param.mutable_grad() = grad.clone();
}

void assert_close(const torch::Tensor& lhs, const torch::Tensor& rhs, const double atol, const double rtol) {
    assert(torch::allclose(lhs.cpu(), rhs.cpu(), rtol, atol));
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

} // namespace

int main() {
    test_eager_fused_parity();
    test_optimizer_roundtrip();
    test_invalid_fused_request_fails_loud();
    return 0;
}
