#pragma once
#include <ATen/core/ivalue.h>
#include <c10/core/DeviceType.h>
#include <memory>
#include <vector>
namespace torch::jit {
struct Graph;
struct Module;
struct CompilationUnit;
}  // namespace torch::jit
namespace mmltk::backend::models::rfdetr::detail {
void install_recorded_forward(torch::jit::Module& module, torch::jit::CompilationUnit& compilation_unit, const std::shared_ptr<torch::jit::Graph>& graph);
c10::IValue run_recorded_forward(torch::jit::Module& module, std::vector<c10::IValue> inputs, c10::DeviceType device);
}  // namespace mmltk::backend::models::rfdetr::detail
