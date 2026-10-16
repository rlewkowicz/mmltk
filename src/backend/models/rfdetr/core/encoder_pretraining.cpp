// Patch adaptation follows timm 1.0.27 (Copyright Ross Wightman,
// Apache-2.0); source and model attribution is retained in NOTICE.
#include "encoder_pretraining.h"
#include "model.h"
#include "detail/backbone.h"
#include "src/common/io/file_digest.h"
#include "src/common/io/file_memory.h"
#include <torch/torch.h>
#include <torch/nn/functional.h>
#include <ATen/ops/linalg_pinv.h>
#include <nlohmann/json.hpp>
#include <sys/mman.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
namespace mmltk::backend::models::rfdetr {
namespace {
namespace io = mmltk::common::io;
namespace F = torch::nn::functional;
using Json = nlohmann::json;
constexpr std::size_t kHeaderLimit = 1024U * 1024U, kTensorLimit = 4096U;
void require(bool admitted, const std::string& reason) {
 if (!admitted) throw std::invalid_argument("encoder pretraining: " + reason);
}
void cancelled(std::stop_token stop) {
 if (stop.stop_requested()) throw std::runtime_error("encoder pretraining cancelled");
}
std::uint64_t unsigned_integer(const Json& value) {
 require(value.is_number_unsigned(), "tensor shape/offset is not an unsigned integer");
 return value.get<std::uint64_t>();
}
void require_fp32(const torch::Tensor& value, const std::string& name) {
 require(value.defined() && value.device().is_cpu() && value.scalar_type() == torch::kFloat32, "expected CPU FP32 tensor: " + name);
 require(torch::isfinite(value).all().item<bool>(), "nonfinite tensor: " + name);
}
class AdmittedEncoderFile final {
public:
 AdmittedEncoderFile(const std::filesystem::path& path, const EncoderPretrainingAsset& asset, std::stop_token stop)
     : file_(io::FileHandle::open_readonly(path.string())), snapshot_(io::FileSnapshot::Read(file_.get())) {
  require(snapshot_.bytes == asset.file_bytes, "source byte count differs from catalog");
  void* address = ::mmap(nullptr, static_cast<std::size_t>(snapshot_.bytes), PROT_READ, MAP_PRIVATE, file_.get(), 0);
  if (address == MAP_FAILED) throw io::errno_error("map encoder source", path.string());
  mapping_.adopt(address, static_cast<std::size_t>(snapshot_.bytes));
  io::Sha256Hasher hash;
  constexpr std::size_t chunk = 1024U * 1024U;
  const auto content = bytes();
  for (std::size_t offset = 0; offset < content.size(); offset += chunk) {
   cancelled(stop);
   hash.Update(content.subspan(offset, std::min(chunk, content.size() - offset)));
  }
  require(io::sha256_hex(hash.Finish()) == asset.sha256, "source SHA-256 differs from catalog");
  unchanged();
 }
 [[nodiscard]] std::span<const std::uint8_t> bytes() const { return {static_cast<const std::uint8_t*>(mapping_.address()), mapping_.mapped_bytes()}; }
 void unchanged() const { require(io::FileSnapshot::Read(file_.get()) == snapshot_, "opened source changed during admission"); }
private:
 io::FileHandle file_;
 io::FileSnapshot snapshot_;
 io::MappedByteRegion mapping_;
};
// This is the exact unused portion of the pinned PE classifier. A prefix match
// would hide new or wrongly shaped source state, so every name/shape is explicit.
const std::unordered_map<std::string, std::vector<std::int64_t>>& pe_exclusions() {
 static const std::unordered_map<std::string, std::vector<std::int64_t>> shapes{
  {"attn_pool.kv.bias", {384}}, {"attn_pool.kv.weight", {384, 192}}, {"attn_pool.latent", {1, 1, 192}},
  {"attn_pool.mlp.fc1.bias", {768}}, {"attn_pool.mlp.fc1.weight", {768, 192}}, {"attn_pool.mlp.fc2.bias", {192}}, {"attn_pool.mlp.fc2.weight", {192, 768}},
  {"attn_pool.norm.bias", {192}}, {"attn_pool.norm.weight", {192}}, {"attn_pool.proj.bias", {192}}, {"attn_pool.proj.weight", {192, 192}},
  {"attn_pool.q.bias", {192}}, {"attn_pool.q.weight", {192, 192}}, {"head.bias", {512}}, {"head.weight", {512, 192}}};
 return shapes;
}
}  // namespace
namespace detail {
std::vector<NormalizedModelStateEntry> decode_encoder_safetensors(const std::span<const std::uint8_t> bytes, const std::uint64_t maximum_bytes) {
 static_assert(std::endian::native == std::endian::little);
 require(bytes.size() <= maximum_bytes && bytes.size() >= 10U, "invalid source byte bound");
 std::uint64_t header_bytes = 0;
 for (std::size_t index = 0; index < 8U; ++index) header_bytes |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
 require(header_bytes > 1U && header_bytes <= kHeaderLimit && header_bytes <= bytes.size() - 8U, "invalid safetensors header length");
 require(bytes[8] == '{', "safetensors header must start with an object");
 std::vector<std::set<std::string>> keys;
 const auto header = Json::parse(bytes.begin() + 8, bytes.begin() + 8 + static_cast<std::ptrdiff_t>(header_bytes), [&](int depth, Json::parse_event_t event, Json& value) {
  require(depth <= 6, "safetensors metadata nesting exceeds bound");
  if (event == Json::parse_event_t::object_start) keys.emplace_back();
  else if (event == Json::parse_event_t::key) require(keys.back().insert(value.get<std::string>()).second, "duplicate safetensors key");
  else if (event == Json::parse_event_t::object_end) keys.pop_back();
  return true;
 });
 require(header.is_object() && !header.empty() && header.size() <= kTensorLimit + 1U, "invalid safetensors tensor count");
 const std::size_t payload = 8U + static_cast<std::size_t>(header_bytes), payload_bytes = bytes.size() - payload;
 struct TensorRange { std::string name; std::vector<std::int64_t> shape; std::size_t begin, end; };
 std::vector<TensorRange> ranges;
 ranges.reserve(std::min(header.size(), kTensorLimit));
 for (const auto& [name, record] : header.items()) {
  require(!name.empty() && name.size() <= 512U, "invalid tensor name");
  if (name == "__metadata__") {
   require(record.is_object(), "metadata must be a string dictionary");
   for (const auto& value : record) require(value.is_string(), "metadata must contain strings only");
   continue;
  }
  require(record.is_object() && record.size() == 3U && record.contains("dtype") && record.contains("shape") && record.contains("data_offsets"), "invalid tensor metadata: " + name);
  require(record.at("dtype") == "F32", "source tensor is not FP32: " + name);
  const auto& shape = record.at("shape");
  const auto& offsets = record.at("data_offsets");
  require(shape.is_array() && shape.size() <= 4U && offsets.is_array() && offsets.size() == 2U, "invalid tensor rank/offsets: " + name);
  TensorRange range{name, {}, 0, 0};
  std::uint64_t elements = 1U;
  for (const auto& extent : shape) {
   const auto dimension = unsigned_integer(extent);
   require(dimension > 0 && dimension <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) && elements <= payload_bytes / sizeof(float) / dimension,
    "tensor shape exceeds source payload: " + name);
   elements *= dimension;
   range.shape.push_back(static_cast<std::int64_t>(dimension));
  }
  const auto begin = unsigned_integer(offsets[0]), end = unsigned_integer(offsets[1]);
  require(begin <= end && end <= payload_bytes && end - begin == elements * sizeof(float), "tensor extent differs from shape: " + name);
  range.begin = static_cast<std::size_t>(begin);
  range.end = static_cast<std::size_t>(end);
  ranges.push_back(std::move(range));
 }
 require(!ranges.empty() && ranges.size() <= kTensorLimit, "invalid safetensors tensor count");
 std::ranges::sort(ranges, {}, &TensorRange::begin);
 std::size_t covered = 0;
 for (const auto& range : ranges) {
  require(range.begin == covered, "overlapping or uncovered safetensors payload: " + range.name);
  covered = range.end;
 }
 require(covered == payload_bytes, "trailing safetensors payload");
 std::vector<NormalizedModelStateEntry> result;
 result.reserve(ranges.size());
 for (const auto& range : ranges) {
  auto tensor = torch::empty(range.shape, torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCPU));
  std::memcpy(tensor.data_ptr<float>(), bytes.data() + payload + range.begin, range.end - range.begin);
  require_fp32(tensor, range.name);
  result.push_back({range.name, std::move(tensor)});
 }
 return result;
}
torch::Tensor resample_encoder_patch(const torch::Tensor& source, const std::int64_t patch_size) {
 require_fp32(source, "patch kernel");
 require(source.dim() == 4 && source.size(2) == source.size(3) && source.size(2) > 0 && source.size(2) <= 32 && patch_size > 0 && patch_size <= 32, "invalid patch resampling geometry");
 const auto old = source.size(2);
 if (old == patch_size) return source;
 // timm 1.0.27 resample_patch_embed (not its retained _old implementation):
 // one FP32 resize matrix and torch.linalg.pinv's atol=None/rtol=None defaults.
 const auto basis = torch::eye(old * old, source.options()).reshape({old * old, 1, old, old});
 const auto resized = F::interpolate(basis, F::InterpolateFuncOptions().size(std::vector<std::int64_t>{patch_size, patch_size}).mode(torch::kBicubic).align_corners(false).antialias(true));
 const auto matrix = resized.squeeze(1).permute({1, 2, 0}).reshape({patch_size * patch_size, old * old});
 const auto inverse = at::linalg_pinv(matrix, std::optional<double>{}, std::optional<double>{}, false);
 return torch::matmul(source.reshape({source.size(0), source.size(1), old * old}), inverse).reshape({source.size(0), source.size(1), patch_size, patch_size});
}
torch::Tensor resample_encoder_positions(const torch::Tensor& source, const std::int64_t grid) {
 require_fp32(source, "positions");
 require(source.dim() == 3 && source.size(0) == 1 && source.size(1) > 1 && grid > 0 && grid <= 128, "invalid position resampling geometry");
 const auto old = static_cast<std::int64_t>(std::llround(std::sqrt(static_cast<double>(source.size(1) - 1))));
 require(old * old + 1 == source.size(1), "source position grid is not square");
 if (old == grid) return source;
 const auto spatial = source.narrow(1, 1, old * old).reshape({1, old, old, source.size(2)}).permute({0, 3, 1, 2});
 const auto resized = F::interpolate(spatial, F::InterpolateFuncOptions().size(std::vector<std::int64_t>{grid, grid}).mode(torch::kBicubic).align_corners(false).antialias(true));
 return torch::cat({source.narrow(1, 0, 1), resized.permute({0, 2, 3, 1}).reshape({1, grid * grid, source.size(2)})}, 1);
}
NormalizedModelStateCandidate stage_encoder_pretraining(BackboneEncoder& encoder, const NativeRfDetrConfig& config, const std::vector<NormalizedModelStateEntry>& state) {
 const auto* asset = find_encoder_pretraining_asset(config.backbone.kind);
 require(asset != nullptr, "unsupported encoder family");
 const auto& geometry = asset->geometry;
 require(config.backbone.dimension == geometry.dimension && config.backbone.feedforward_dimension == geometry.feedforward_dimension && config.backbone.heads == geometry.heads &&
  config.backbone.layers == geometry.layers, "encoder topology differs from source family");
 require(state.size() <= kTensorLimit, "source tensor count exceeds bound");
 const bool pe = config.backbone.kind == BackboneKind::PeCoreTiny;
 const std::string prefix = pe ? "model." : "encoder.";
 const std::string patch = pe ? "patch_embed.proj.weight" : "embeddings.patch_embeddings.projection.weight";
 const std::string positions = pe ? "pos_embed" : "embeddings.position_embeddings";
 auto parameters = encoder.named_parameters(true), buffers = encoder.named_buffers(true);
 NormalizedModelStateCandidate candidate;
 candidate.destinations.reserve(parameters.size() + buffers.size());
 candidate.tensors.reserve(parameters.size() + buffers.size());
 std::unordered_set<std::string> supplied;
 for (const auto& entry : state) {
  require(supplied.insert(entry.name).second, "duplicate source tensor: " + entry.name);
  require_fp32(entry.tensor, entry.name);
  const auto name = prefix + entry.name;
  auto* destination = parameters.find(name);
  if (!destination) destination = buffers.find(name);
  if (!destination) {
   const auto excluded = pe_exclusions().find(entry.name);
   if (pe && excluded != pe_exclusions().end()) require(entry.tensor.sizes().vec() == excluded->second, "excluded PE tensor shape differs: " + entry.name);
   else candidate.summary.unexpected_names.push_back(entry.name);
   continue;
  }
  require(destination->device().is_cpu() && destination->scalar_type() == torch::kFloat32, "initialization requires a fresh CPU FP32 encoder");
  auto expected = destination->sizes().vec();
  if (entry.name == patch) expected = {geometry.dimension, geometry.channels, geometry.patch_size, geometry.patch_size};
  if (entry.name == positions) expected = {1, geometry.position_grid * geometry.position_grid + 1, geometry.dimension};
  if (entry.tensor.sizes().vec() != expected) {
   candidate.summary.incompatible_names.push_back(name);
   continue;
  }
  auto tensor = entry.name == patch ? resample_encoder_patch(entry.tensor, config.patch_size)
   : entry.name == positions ? resample_encoder_positions(entry.tensor, config.positional_encoding_size) : entry.tensor;
  require_fp32(tensor, name);
  (void)stage_model_state_tensor(candidate, name, tensor, *destination);
 }
 const auto missing = [&](const auto& values) {
  for (const auto& value : values) {
   require(value.key().starts_with(prefix), "encoder owner has unexpected relative names");
   if (!supplied.contains(value.key().substr(prefix.size()))) candidate.summary.missing_names.push_back(value.key());
  }
 };
 missing(parameters);
 missing(buffers);
 if (pe) for (const auto& [name, shape] : pe_exclusions()) if (!supplied.contains(name)) candidate.summary.missing_names.push_back(name);
 if (!candidate.summary.missing_names.empty() || !candidate.summary.unexpected_names.empty() || !candidate.summary.incompatible_names.empty())
  throw ModelStateAdmissionError("encoder pretraining state does not exactly cover the source and native encoder", std::move(candidate.summary));
 return candidate;
}
}  // namespace detail
EncoderPretrainingResult initialize_encoder_pretraining(NativeRfDetrModel& model, const std::filesystem::path& path, std::stop_token stop) {
 cancelled(stop);
 const auto* asset = find_encoder_pretraining_asset(model.config().backbone.kind);
 require(asset != nullptr, "unsupported encoder family");
 const auto admitted = std::make_shared<AdmittedEncoderFile>(path, *asset, stop);
 const auto source = detail::decode_encoder_safetensors(admitted->bytes(), asset->file_bytes);
 auto candidate = model.stage_encoder_pretraining(source);
 EncoderPretrainingResult result{encoder_pretraining_provenance(*asset), candidate.summary};
 candidate.validate_source = [admitted] { admitted->unchanged(); };
 cancelled(stop);
 model.commit_normalized_state(std::move(candidate));
 return result;
}
}  // namespace mmltk::backend::models::rfdetr
