#pragma once
#include <ATen/ATen.h>
#include <cuda.h>
#include <memory>
#include <cstdint>
#include <utility>
#include <vector>
#include "src/common/system/numa_memory.h"
#include "src/backend/models/rfdetr/core/detail/lsap_scratch.h"
namespace mmltk::backend::models::rfdetr {
struct TracedLossOpCache;
struct MatcherStatistics final {
 std::uint64_t cost_submissions = 0, cost_bytes = 0, cost_dependencies = 0;
 std::uint64_t materializations = 0, assignment_bytes = 0, uploads = 0;
 std::uint64_t h2d_submissions = 0, gdr_writes = 0, storage_growth = 0, completion_dependencies = 0;
};
struct MatcherLayerIndices final {
 std::pair<at::Tensor, at::Tensor> source;
 at::Tensor global_targets;
 std::pair<at::Tensor, at::Tensor> cpu_source;
};
// A training lane owns one workspace. Returned tensor storage owns its immutable
// assignment slot independently; no loss helper allocates or uploads indices.
class MatcherWorkspace final {
public:
 explicit MatcherWorkspace(int node, bool h2d = false);
 ~MatcherWorkspace();
 MatcherWorkspace(const MatcherWorkspace&) = delete;
 MatcherWorkspace& operator=(const MatcherWorkspace&) = delete;
 void prepare_cost(at::IntArrayRef shape, const at::Device&);
 [[nodiscard]] at::Tensor device_layer(std::int64_t index) const;
 [[nodiscard]] at::Tensor read_cost();
 [[nodiscard]] at::Tensor cpu_indices(std::int64_t count);
 [[nodiscard]] TracedLossOpCache& loss_cache() noexcept;
 [[nodiscard]] LsapScratch& solver() noexcept { return solver_; }
 [[nodiscard]] std::vector<MatcherLayerIndices> pack(const std::vector<std::vector<std::pair<at::Tensor, at::Tensor>>>&, const std::vector<std::int64_t>& target_offsets, const at::Device&);
 // Called after the lane's complete forward/backward use. CPU ownership still
 // prevents reuse of tensors saved in retained graphs.
 void complete_assignments(CUstream stream);
 void enable_statistics() noexcept;
 [[nodiscard]] MatcherStatistics statistics() const noexcept;

private:
 mmltk::common::system::NumaMemoryResource memory_;
 LsapScratch solver_;
 struct State;
 std::unique_ptr<State> state_;
};
}  // namespace mmltk::backend::models::rfdetr
