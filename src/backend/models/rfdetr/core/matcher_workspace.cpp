#include "detail/matcher_workspace.h"
#include "detail/traced_loss_cache.h"
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <cuda_runtime_api.h>
#include <algorithm>
#include <limits>
#include <meta>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <utility>
#include "src/backend/ml/cuda/numa_host_tensor.h"
#include "src/frameworks/gpu/gdr_mapped_buffer.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "src/common/io/scoped_fd.h"
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::models::rfdetr {
namespace {
void check(CUresult status, const char* operation) {
 if (status != CUDA_SUCCESS) throw std::runtime_error(operation);
}
struct MatcherContext final {
 explicit MatcherContext(int index) : device(index) {
  check(cuCtxGetCurrent(&context), "resolve actual matcher CUDA context");
  if (!context) throw std::invalid_argument("matcher requires a current owning CUDA context");
  CUcontext primary{};
  check(cuDevicePrimaryCtxRetain(&primary, device), "retain matcher CUDA primary context");
  primary_owned = primary == context;
  if (!primary_owned) check(cuDevicePrimaryCtxRelease(device), "release unused matcher primary context");
 }
 ~MatcherContext() {
  if (primary_owned) (void)cuDevicePrimaryCtxRelease(device);
 }
 int device;
 CUcontext context{};
 bool primary_owned = false;
};
template <class Release>
[[nodiscard]] bool release_matcher_resources(const std::shared_ptr<MatcherContext>& context, Release&& release) noexcept {
 if (!context) return true;
 if (cuCtxPushCurrent(context->context) != CUDA_SUCCESS) return false;
 bool released = false;
 try {
  std::forward<Release>(release)();
  released = true;
 } catch (...) {}
 CUcontext prior{};
 return cuCtxPopCurrent(&prior) == CUDA_SUCCESS && released;
}
struct AssignmentResources final {
 std::shared_ptr<MatcherContext> context;
 std::unique_ptr<mmltk::frameworks::gpu::GdrMappedBuffer> mapped;
 std::unique_ptr<mmltk::backend::ml::cuda::NumaHostTensor> host;
 at::Tensor host_values;
 at::Tensor device_values;
 std::optional<mmltk::frameworks::gpu::GdrMappedBuffer::ReadLease> lease;
 CUevent complete{};
 bool recorded = false;
 bool release() noexcept {
  return release_matcher_resources(context, [this] {
   if (!recorded)
    check(cuCtxSynchronize(), "settle assignment retirement");
   else if (complete)
    check(cuEventSynchronize(complete), "complete assignment retirement");
   lease.reset();
   if (mapped) mapped->close();
   host_values = at::Tensor{};
   if (host) check(host->ReleaseSettled(), "release assignment host storage");
   device_values = at::Tensor{};
   if (complete) {
    check(cuEventDestroy(complete), "release assignment event");
    complete = nullptr;
   }
  });
 }
};
struct AssignmentSlot final {
 mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement{1};
 mmltk::frameworks::gpu::TerminalCudaRetirementLease reservation = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement);
 std::shared_ptr<AssignmentResources> physical = std::make_shared<AssignmentResources>();
 // Counts tensor-storage owners, independent of the workspace's slot owner.
 std::shared_ptr<int> use = std::make_shared<int>(0);
 ~AssignmentSlot() {
  if (!physical->release()) std::move(reservation).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(physical)), cudaErrorUnknown);
 }
 void settle() {
  auto& p = *physical;
  if (p.recorded && p.complete) check(cuEventSynchronize(p.complete), "settle matcher assignment consumers");
  if (!p.recorded && (p.device_values.defined() || p.lease)) check(cuCtxSynchronize(), "settle unrecorded matcher consumers");
  p.lease.reset();
  p.recorded = false;
 }
};
struct MatcherCostResources final {
 std::shared_ptr<MatcherContext> context;
 std::unique_ptr<mmltk::backend::ml::cuda::NumaHostTensor> host;
 at::Tensor device_backing, device_cost, cpu_cost, output_prefixes;
 std::vector<std::int64_t> queries, target_prefixes, layer_prefixes;
 CUevent complete{};
 bool pending = false;
 bool release() noexcept {
  return release_matcher_resources(context, [this] {
   if (pending) check(cuCtxSynchronize(), "settle unfinished matcher cost generation");
   cpu_cost = at::Tensor{};
   if (host) check(host->ReleaseSettled(), "release matcher cost pages");
   output_prefixes = at::Tensor{};
   device_cost = at::Tensor{};
   device_backing = at::Tensor{};
   if (complete) {
    check(cuEventDestroy(complete), "release matcher cost event");
    complete = nullptr;
   }
  });
 }
};
template <auto Member>
[[nodiscard]] bool append_matcher_statistic(char* const record, const std::size_t capacity, int& length, const MatcherStatistics& statistics) noexcept {
 constexpr auto name = std::meta::identifier_of(Member);
 const auto remaining = capacity - static_cast<std::size_t>(length);
 const int appended = std::snprintf(record + length, remaining, ",\"%.*s\":%llu", static_cast<int>(name.size()), name.data(), static_cast<unsigned long long>(statistics.[:Member:]));
 if (appended < 0 || static_cast<std::size_t>(appended) >= remaining) return false;
 length += appended;
 return true;
}
inline constexpr auto kMatcherStatisticMembers = std::define_static_array(std::meta::nonstatic_data_members_of(^^MatcherStatistics, std::meta::access_context::current()));
template <std::size_t... Indices>
[[nodiscard]] bool append_matcher_statistics(char* const record, const std::size_t capacity, int& length, const MatcherStatistics& statistics, std::index_sequence<Indices...>) noexcept {
 return (append_matcher_statistic<kMatcherStatisticMembers[Indices]>(record, capacity, length, statistics) && ...);
}
}  // namespace
struct MatcherWorkspace::State {
 explicit State(bool upload_h2d) : h2d(upload_h2d) {
  if (const auto* path = std::getenv("MMLTK_MATCHER_TRACE_FILE"); path && *path) {
   trace.reset(::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
   observed = trace.get() >= 0;
  }
 }
 bool h2d;
 bool observed = false;
 MatcherStatistics statistics;
 mmltk::common::io::ScopedFd trace;
 void count(std::uint64_t MatcherStatistics::* member, std::uint64_t amount = 1) noexcept {
  if (observed) statistics.*member += amount;
 }
 void log(const char* event) const noexcept {
  if (trace.get() < 0) return;
  char record[768];
  int length = std::snprintf(record, sizeof(record), "{\"event\":\"%s\",\"device\":%d,\"transport\":\"%s\"", event, context ? context->device : -1, h2d ? "h2d" : "gdr");
  if (length < 0 || static_cast<std::size_t>(length) >= sizeof(record)) return;
  if (!append_matcher_statistics(record, sizeof(record), length, statistics, std::make_index_sequence<kMatcherStatisticMembers.size()>{})) return;
  if (static_cast<std::size_t>(length) + 2 > sizeof(record)) return;
  record[length++] = '}';
  record[length++] = '\n';
  const auto written = ::write(trace.get(), record, static_cast<std::size_t>(length));
  (void)written;
 }
 std::shared_ptr<MatcherContext> context;
 TracedLossOpCache loss_cache;
 mmltk::frameworks::gpu::TerminalCudaRetirementOwner retirement{1};
 mmltk::frameworks::gpu::TerminalCudaRetirementLease reservation = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement);
 std::shared_ptr<MatcherCostResources> cost = std::make_shared<MatcherCostResources>();
 std::vector<std::shared_ptr<AssignmentSlot>> assignments;
 std::vector<std::shared_ptr<mmltk::common::system::NumaMemory>> cpu_results;
 ~State() {
  assignments.clear();
  if (!cost->release()) std::move(reservation).Install(mmltk::frameworks::gpu::TerminalCudaCustody::Share(std::move(cost)), cudaErrorUnknown);
 }
 void bind(const at::Device& device) {
  if (!device.is_cuda()) return;
  if (context && context->device != device.index()) throw std::invalid_argument("matcher workspace cannot change owning CUDA device");
  if (context) {
   CUcontext active{};
   check(cuCtxGetCurrent(&active), "resolve matcher caller context");
   if (active != context->context) throw std::invalid_argument("matcher workspace cannot change owning CUDA context");
  }
  if (!context) {
   auto candidate_context = std::make_shared<MatcherContext>(device.index());
   auto candidate_host = std::make_unique<mmltk::backend::ml::cuda::NumaHostTensor>(device.index());
   CUevent candidate_event{};
   check(cuEventCreate(&candidate_event, CU_EVENT_DISABLE_TIMING), "create matcher cost completion");
   context = std::move(candidate_context);
   cost->context = context;
   cost->host = std::move(candidate_host);
   cost->complete = candidate_event;
  }
 }
};
MatcherWorkspace::MatcherWorkspace(int node, bool h2d) : memory_(node), solver_(&memory_), state_(std::make_unique<State>(h2d)) {}
MatcherWorkspace::~MatcherWorkspace() = default;
TracedLossOpCache& MatcherWorkspace::loss_cache() noexcept { return state_->loss_cache; }
void MatcherWorkspace::enable_statistics() noexcept { state_->observed = true; }
MatcherStatistics MatcherWorkspace::statistics() const noexcept { return state_->statistics; }
at::Tensor MatcherWorkspace::cpu_indices(std::int64_t count) {
 if (count < 0 || count > std::numeric_limits<std::int64_t>::max() / 16) throw std::invalid_argument("matcher CPU assignment extent overflows");
 std::shared_ptr<mmltk::common::system::NumaMemory> storage;
 const auto reusable = std::ranges::find_if(state_->cpu_results, [](const auto& slot) { return slot.use_count() == 1; });
 if (reusable == state_->cpu_results.end()) {
  storage = std::make_shared<mmltk::common::system::NumaMemory>(memory_.node());
  state_->cpu_results.push_back(storage);
 } else {
  storage = *reusable;
 }
 storage->ensure_bytes(std::max<std::size_t>(2 * count * sizeof(std::int64_t), 1));
 return at::from_blob(storage->data(), {2, count}, [storage](void*) {}, at::TensorOptions().dtype(at::kLong));
}
void MatcherWorkspace::prepare_cost(at::IntArrayRef queries, at::IntArrayRef counts, const at::Device& device) {
 if (!device.is_cuda() || !device.has_index()) throw std::invalid_argument("matcher cost storage requires an explicit CUDA device");
 constexpr std::int64_t limit = std::numeric_limits<std::int64_t>::max() / sizeof(float);
 auto product = [](std::int64_t lhs, std::int64_t rhs) {
  if (lhs < 0 || rhs < 0 || (rhs && lhs > limit / rhs)) throw std::invalid_argument("matcher cost shape overflows");
  return lhs * rhs;
 };
 std::int64_t targets = 0, elements = 0, max_targets = 0, max_queries = 0;
 for (auto count : counts) {
  if (count < 0 || count > limit - targets) throw std::invalid_argument("matcher target prefix overflows");
  targets += count;
  max_targets = std::max(max_targets, count);
 }
 for (auto count : queries) {
  const auto extent = product(count, targets);
  if (extent > limit - elements) throw std::invalid_argument("matcher layer prefix overflows");
  elements += extent;
  max_queries = std::max(max_queries, count);
 }
 // Retain the former padded allocation admission even though storage is compact.
 (void)product(product(product(queries.size(), counts.size()), max_queries), max_targets);
 c10::cuda::CUDAGuard guard(device);
 state_->bind(device);
 auto& cost = *state_->cost;
 if (cost.pending) {
  check(cuCtxSynchronize(), "settle unfinished matcher cost generation before reuse");
  cost.pending = false;
 }
 // Reserve before replacing any active metadata; settled reuse keeps capacity.
 cost.queries.reserve(queries.size());
 cost.target_prefixes.reserve(counts.size() + 1);
 cost.layer_prefixes.reserve(queries.size() + 1);
 if (!cost.device_backing.defined() || cost.device_backing.numel() < elements) {
  cost.device_backing = at::empty({elements}, at::TensorOptions().dtype(at::kFloat).device(device));
  mmltk::common::logging::profile_add_value("rfdetr.matcher.cost_storage_growth", 1);
 }
 cost.device_cost = cost.device_backing.narrow(0, 0, elements);
 cost.cpu_cost = at::Tensor{};
 cost.cpu_cost = cost.host->view({elements}, at::kFloat);
 cost.output_prefixes = at::Tensor{};
 cost.queries.assign(queries.begin(), queries.end());
 cost.target_prefixes.clear();
 cost.target_prefixes.push_back(0);
 for (auto count : counts) cost.target_prefixes.push_back(cost.target_prefixes.back() + count);
 cost.layer_prefixes.clear();
 cost.layer_prefixes.push_back(0);
 for (auto count : queries) cost.layer_prefixes.push_back(cost.layer_prefixes.back() + count * targets);
 cost.pending = true;
}
at::Tensor MatcherWorkspace::device_layer(std::int64_t index) const {
 const auto& cost = *state_->cost;
 const auto begin = cost.layer_prefixes.at(index);
 return cost.device_cost.narrow(0, begin, cost.layer_prefixes.at(index + 1) - begin);
}
at::Tensor MatcherWorkspace::cpu_matrix(std::int64_t layer, std::int64_t image) const {
 const auto& cost = *state_->cost;
 if (cost.pending) throw std::logic_error("matcher costs have not settled");
 const auto queries = cost.queries.at(layer);
 const auto prefix = cost.target_prefixes.at(image);
 const auto count = cost.target_prefixes.at(image + 1) - prefix;
 return cost.cpu_cost.narrow(0, cost.layer_prefixes.at(layer) + queries * prefix, queries * count).view({queries, count});
}
at::Tensor MatcherWorkspace::output_offsets(at::IntArrayRef lookup_offsets, const at::Tensor& device_offsets) {
 auto& cost = *state_->cost;
 const auto batch = cost.target_prefixes.size() - 1;
 if (lookup_offsets.size() != batch) throw std::invalid_argument("matcher output metadata does not match batch size");
 if (std::equal(lookup_offsets.begin(), lookup_offsets.end(), cost.target_prefixes.begin())) {
  cost.output_prefixes = device_offsets;
 } else {
  cost.output_prefixes = at::tensor(at::IntArrayRef(cost.target_prefixes.data(), batch), device_offsets.options());
 }
 return cost.output_prefixes;
}
at::Tensor MatcherWorkspace::read_cost() {
 if (!state_->context || !state_->cost->device_cost.defined()) throw std::logic_error("matcher costs have not been prepared");
 c10::cuda::CUDAGuard guard(static_cast<c10::DeviceIndex>(state_->context->device));
 const auto device_index = static_cast<c10::DeviceIndex>(state_->context->device);
 state_->bind(at::Device(at::kCUDA, device_index));
 const auto stream = c10::cuda::getCurrentCUDAStream(device_index);
 const auto bytes = state_->cost->device_cost.nbytes();
 check(cuMemcpyDtoHAsync(state_->cost->cpu_cost.data_ptr(), reinterpret_cast<CUdeviceptr>(state_->cost->device_cost.data_ptr()), bytes, stream.stream()), "copy active matcher costs");
 check(cuEventRecord(state_->cost->complete, stream.stream()), "record matcher cost completion");
 check(cuEventSynchronize(state_->cost->complete), "complete matcher cost copy");
 state_->cost->pending = false;
 state_->count(&MatcherStatistics::cost_submissions);
 state_->count(&MatcherStatistics::cost_bytes, bytes);
 state_->count(&MatcherStatistics::cost_dependencies);
 state_->log("cost_complete");
 mmltk::common::logging::profile_add_value("rfdetr.matcher.cost_copy_syncs", 1);
 mmltk::common::logging::profile_add_value("rfdetr.matcher.cost_d2h_bytes", bytes);
 return state_->cost->cpu_cost;
}
std::vector<MatcherLayerIndices> MatcherWorkspace::pack(
 const std::vector<std::vector<std::pair<at::Tensor, at::Tensor>>>& indices, const std::vector<std::int64_t>& target_offsets, const at::Device& device) {
 if (!device.is_cpu() && !device.is_cuda()) throw std::invalid_argument("matcher assignments require a CPU or CUDA device");
 std::vector<std::int64_t> offsets{0};
 for (const auto& layer : indices) {
  if (layer.size() != target_offsets.size()) throw std::invalid_argument("matcher assignment target offsets do not match the batch");
  auto count = offsets.back();
  for (const auto& pair : layer) {
   if (!pair.first.device().is_cpu() || !pair.second.device().is_cpu() || pair.first.scalar_type() != at::kLong || pair.second.scalar_type() != at::kLong || pair.first.dim() != 1 ||
       pair.second.dim() != 1 || !pair.first.is_contiguous() || !pair.second.is_contiguous() || pair.first.numel() != pair.second.numel() ||
       pair.first.numel() > std::numeric_limits<std::int64_t>::max() / 24 - count)
    throw std::invalid_argument("matcher assignment indices require equal contiguous CPU int64 vectors");
   count += pair.first.numel();
  }
  offsets.push_back(count);
 }
 const auto count = offsets.back();
 at::Tensor packed;
 std::shared_ptr<AssignmentSlot> slot;
 if (count == 0) {
  packed = at::empty({3, 0}, at::TensorOptions().dtype(at::kLong).device(device));
 } else if (device.is_cuda()) {
  c10::cuda::CUDAGuard guard(device);
  state_->bind(device);
  const auto reusable = std::ranges::find_if(state_->assignments, [](const auto& item) { return item->use.use_count() == 1; });
  if (reusable != state_->assignments.end()) {
   slot = *reusable;
   slot->settle();
  } else {
   slot = std::make_shared<AssignmentSlot>();
   slot->physical->context = state_->context;
   slot->physical->host = std::make_unique<mmltk::backend::ml::cuda::NumaHostTensor>(device.index());
   if (state_->h2d) check(cuEventCreate(&slot->physical->complete, CU_EVENT_DISABLE_TIMING), "create assignment completion");
   if (!state_->h2d) slot->physical->mapped = std::make_unique<mmltk::frameworks::gpu::GdrMappedBuffer>(state_->context->context);
   state_->assignments.push_back(slot);
   mmltk::common::logging::profile_add_value("rfdetr.matcher.assignment_slots", 1);
  }
  slot->physical->host_values = at::Tensor{};
  slot->physical->host_values = slot->physical->host->view({3, count}, at::kLong);
  packed = slot->physical->host_values;
 } else {
  auto storage = std::make_shared<mmltk::common::system::NumaMemory>(memory_.node());
  storage->ensure_bytes(std::max<std::size_t>(3 * count * sizeof(std::int64_t), 1));
  packed = at::from_blob(storage->data(), {3, count}, [storage](void*) {}, at::TensorOptions().dtype(at::kLong));
 }
 auto* data = packed.data_ptr<std::int64_t>();
 std::int64_t cursor = 0;
 for (const auto& layer : indices) {
  for (std::size_t batch = 0; batch < layer.size(); ++batch) {
   const auto* sources = layer[batch].first.data_ptr<std::int64_t>();
   const auto* targets = layer[batch].second.data_ptr<std::int64_t>();
   for (std::int64_t i = 0; i < layer[batch].first.numel(); ++i, ++cursor) {
    data[cursor] = static_cast<std::int64_t>(batch);
    data[count + cursor] = sources[i];
    data[2 * count + cursor] = target_offsets[batch] + targets[i];
   }
  }
 }
 mmltk::common::logging::profile_add_value("rfdetr.matcher.index_materializations", 1);
 mmltk::common::logging::profile_add_value("rfdetr.matcher.assignment_bytes", packed.nbytes());
 state_->count(&MatcherStatistics::materializations);
 state_->count(&MatcherStatistics::assignment_bytes, packed.nbytes());
 auto cpu_packed = slot ? at::from_blob(packed.data_ptr(), {3, count}, [slot, use = slot->use](void*) {}, packed.options()) : packed;
 if (slot && count) {
  c10::cuda::CUDAGuard guard(device);
  void* destination{};
  if (slot->physical->mapped) {
   const auto capacity = slot->physical->mapped->capacity_bytes();
   slot->physical->mapped->ensure_bytes(packed.nbytes());
   if (capacity != slot->physical->mapped->capacity_bytes()) {
    state_->count(&MatcherStatistics::storage_growth);
    mmltk::common::logging::profile_add_value("rfdetr.matcher.assignment_storage_growth", 1);
   }
   if (!slot->physical->mapped->write(0, {reinterpret_cast<const std::byte*>(data), packed.nbytes()})) throw std::runtime_error("matcher assignment upload cancelled");
   slot->physical->lease.emplace(slot->physical->mapped->borrow());
   destination = reinterpret_cast<void*>(slot->physical->lease->device_data());
   state_->count(&MatcherStatistics::gdr_writes);
  } else {
   if (!slot->physical->device_values.defined() || slot->physical->device_values.numel() < 3 * count) {
    slot->physical->device_values = at::empty({3 * count}, packed.options().device(device));
    state_->count(&MatcherStatistics::storage_growth);
    mmltk::common::logging::profile_add_value("rfdetr.matcher.assignment_storage_growth", 1);
   }
   destination = slot->physical->device_values.data_ptr();
   check(cuMemcpyHtoDAsync(reinterpret_cast<CUdeviceptr>(destination), data, packed.nbytes(), c10::cuda::getCurrentCUDAStream(device.index()).stream()), "upload packed assignments");
   mmltk::common::logging::profile_add_value("rfdetr.matcher.assignment_h2d_submissions", 1);
   state_->count(&MatcherStatistics::h2d_submissions);
  }
  packed = at::from_blob(destination, {3, count}, [slot, use = slot->use](void*) {}, packed.options().device(device));
  mmltk::common::logging::profile_add_value("rfdetr.matcher.assignment_uploads", 1);
  state_->count(&MatcherStatistics::uploads);
 } else if (slot) {
  packed = at::empty({3, 0}, packed.options().device(device));
 }
 std::vector<MatcherLayerIndices> result;
 result.reserve(indices.size());
 for (std::size_t layer = 0; layer < indices.size(); ++layer) {
  const auto begin = offsets[layer];
  const auto size = offsets[layer + 1] - begin;
  result.push_back({{packed.select(0, 0).narrow(0, begin, size), packed.select(0, 1).narrow(0, begin, size)}, packed.select(0, 2).narrow(0, begin, size),
   {cpu_packed.select(0, 0).narrow(0, begin, size), cpu_packed.select(0, 1).narrow(0, begin, size)}});
 }
 state_->log("assignment_complete");
 return result;
}
void MatcherWorkspace::complete_assignments(CUstream stream) {
 if (!state_->context) return;
 c10::cuda::CUDAGuard guard(static_cast<c10::DeviceIndex>(state_->context->device));
 state_->bind(at::Device(at::kCUDA, static_cast<c10::DeviceIndex>(state_->context->device)));
 for (auto& slot : state_->assignments) {
  if (slot->use.use_count() != 1 || slot->physical->recorded || (!slot->physical->lease && !slot->physical->device_values.defined())) continue;
  if (slot->physical->lease)
   slot->physical->lease->record_consumed(stream);
  else
   check(cuEventRecord(slot->physical->complete, stream), "record assignment last consumer");
  slot->physical->recorded = true;
  state_->count(&MatcherStatistics::completion_dependencies);
  mmltk::common::logging::profile_add_value("rfdetr.matcher.assignment_completion_dependencies", 1);
 }
 state_->log("consumers_submitted");
}
}  // namespace mmltk::backend::models::rfdetr
