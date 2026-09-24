#pragma once
#include <filesystem>
#include <functional>
#include <cstdint>
#include "src/controller/contracts/prediction_output.h"
#include "src/controller/contracts/workflow_output.h"
namespace mmltk::controller {
struct PredictionRunOutput final {
 std::filesystem::path directory;
 contracts::PredictionOutputSettings saving{};
 contracts::PredictionRunPreview preview{};
 std::uint64_t population = 0;
 std::uint64_t processing_population = 0;
 std::function<void(const contracts::WorkflowOutputFacts&)> progress{};
};
}  // namespace mmltk::controller
