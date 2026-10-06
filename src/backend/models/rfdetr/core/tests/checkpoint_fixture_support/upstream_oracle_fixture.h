#pragma once
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/core/detail/detection_sampling.h"
#include "src/backend/models/rfdetr/contract/train_recipe.h"
#include <filesystem>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json_fwd.hpp>
namespace mmltk::backend::models::rfdetr::testsupport {
struct OracleTensor final {
 bool defined = false;
 std::vector<int64_t> shape, stride;
 std::string dtype, device;
 int64_t finite_count = 0, elements = 0;
};
struct OracleArchiveRecord final {
 std::string file, sha256;
 std::uint64_t bytes = 0;
 std::map<std::string, OracleTensor> tensors;
};
struct OracleParameterGroup final {
 std::vector<std::string> names;
 double lr = 0, weight_decay = 0, beta1 = .9, beta2 = .999, eps = 1e-8;
 bool amsgrad = false;
};
struct OracleCase final {
 std::string id, preset, precision;
 bool training = false;
 int batch = 0, resolution = 0, groups = 0, classes = 0;
 std::vector<std::string> trainable;
 std::vector<OracleParameterGroup> parameter_groups;
 TrainRecipeValues recipe;
 double clip_max_norm = .1;
 std::map<std::string, double> loss_weights;
 std::vector<OracleArchiveRecord> steps;
 [[nodiscard]] at::ScalarType dtype() const;
};
class OracleArchive final {
public:
 OracleArchive(const std::filesystem::path& directory, const OracleArchiveRecord& record);
 [[nodiscard]] torch::Tensor tensor(const std::string& name, torch::Device device = torch::kCPU) const;
 [[nodiscard]] std::set<std::string> names(std::string_view prefix) const;
 [[nodiscard]] bool contains(const std::string& name) const;
private:
 TensorMap tensors_;
 std::set<std::string> undefined_;
};
class UpstreamOracle final {
public:
 // Absence alone is an optional skip. Empty, missing or malformed requested
 // paths throw, including provenance/inventory/hash mismatches.
 [[nodiscard]] static std::optional<std::filesystem::path> requested();
 explicit UpstreamOracle(const std::filesystem::path& root);
 [[nodiscard]] const std::vector<OracleCase>& cases() const noexcept { return cases_; }
 [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
 [[nodiscard]] std::filesystem::path checkpoint(std::string_view preset) const;
 [[nodiscard]] NativeRfDetrModel model(const OracleCase& entry) const;
 [[nodiscard]] OracleArchive archive(const OracleCase& entry, std::size_t step) const;
 [[nodiscard]] OracleArchive resize_archive() const;
private:
 std::filesystem::path directory_;
 std::map<std::string, std::filesystem::path> checkpoints_;
 std::vector<OracleCase> cases_;
 OracleArchiveRecord resize_;
};
// Comparison reports are flushed after every boundary, including failure. A
// diagnostic stock-policy differential never counts as controlled parity.
class OracleComparison final {
public:
 OracleComparison(const std::filesystem::path& root, std::string name, at::ScalarType precision,
  std::string objective = "stock IA-BCE with separately declared FP32 matcher policy");
 void compare(const std::string& boundary, const torch::Tensor& actual, const torch::Tensor& expected, bool policy_difference = false);
 void inventory(const std::string& boundary, const std::set<std::string>& actual, const std::set<std::string>& expected);
 void fact(const std::string& boundary, const nlohmann::json& value);
private:
 void record(nlohmann::json value);
 std::ofstream report_;
 at::ScalarType precision_;
 bool failed_ = false;
};
[[nodiscard]] TensorMap oracle_output_tensors(const ModelOutputs& outputs);
[[nodiscard]] PreparedTargets oracle_targets(const OracleArchive& archive, const OracleCase& entry, torch::Device device);
[[nodiscard]] std::vector<LayerMaskSamples> oracle_samples(const OracleArchive&, std::size_t layers, torch::Device, const std::string& prefix = "");
[[nodiscard]] DetectionConfig oracle_detection(const NativeRfDetrConfig& config);
void compare_oracle_outputs(OracleComparison&, const OracleArchive&, const ModelOutputs&, const std::vector<torch::Tensor>& encoder_indices, const std::string& prefix = "");
void compare_oracle_vjp(OracleComparison&, const OracleArchive&, NativeRfDetrModel&, const ModelOutputs&, const torch::Tensor& input, bool retain_graph);
}  // namespace mmltk::backend::models::rfdetr::testsupport
