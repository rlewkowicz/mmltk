module;
// Bounded independent RF-DETR evidence. Execute only through ./mmltk.
//
// Native counterpart to generate_rfdetr_upstream_oracle.py. The pinned RF-DETR
// Python package and donor PyTorch/torchvision remain the numerical authority;
// this file never reads, imports, launches, or embeds the original producer.
// All scenario control, dependency/source checks, observation bookkeeping,
// inventories, bounds, hashing, and publication are owned here. pybind11 calls
// the genuine upstream APIs and installs C++ observation callbacks.
//
// Build dependencies (deliberately no build/wrapper changes in this port):
//   * The donor CPython development headers and matching embedding library.
//   * pybind11/embed.h and pybind11/stl.h, available in the donor Torch include
//     directory or a matching pybind11 development installation.
//   * third_party/json/include, OpenSSL development headers, and libcrypto.
// Link against CPython's embed library (including its platform link flags) and
// libcrypto. Build/run inside the existing pinned oracle container, retaining
// its PYTHONPATH overlay, offline settings, CUDA payload, and requirements.
// producer_sha256 identifies the running native executable, not the old script.
// Version 2 uses authenticated indexes and bounded physical tensor shards.
#include <pybind11/embed.h>
#include <pybind11/stl.h>
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include "native_catalog.h"
export module mmltk.tools.generate_rfdetr_upstream_oracle;
import std;
import mmltk.tools.cpp_digest;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
namespace {
namespace py = pybind11;
namespace fs = std::filesystem;
namespace support = mmltk::tools;
using Json = nlohmann::json;  // Sorted object keys, as json.dumps(sort_keys=True).
using namespace pybind11::literals;
constexpr std::string_view Format = "mmltk.rfdetr.upstream-oracle.v2";
const fs::path OracleRoot = "/opt/rfdetr-oracle";
constexpr std::uint64_t MaxManifest = 32ULL * 1024 * 1024;
constexpr std::uint64_t MaxArchive = 4ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t ShardTarget = 256ULL * 1024 * 1024;
constexpr std::uint64_t MaxStepStorage = 64ULL * 1024 * 1024 * 1024;
constexpr std::size_t MaxShards = 256, MaxTensors = 20000;
constexpr auto KeypointTopologyFields = std::to_array<std::pair<const char*, const char*>>(
 {{"keypoint_dimension_downscale", "grouppose_keypoint_dim_downscale"}, {"keypoint_cross_attention", "keypoint_cross_attn"}, {"inter_instance_keypoint_attention", "inter_instance_kp_attn"}});
py::module_ module(const char* name) { return py::module_::import(name); }
py::object object(py::handle value) { return py::reinterpret_borrow<py::object>(value); }
bool truth(py::handle value) {
 const int result = PyObject_IsTrue(value.ptr());
 if (result < 0) throw py::error_already_set();
 return result != 0;
}
std::string string(py::handle value) { return py::str(value).cast<std::string>(); }
std::int64_t integer(py::handle value) {
 // Tensor scalar conversion follows Python int(), including its device sync.
 return module("builtins").attr("int")(value).cast<std::int64_t>();
}
py::object owned_result(PyObject* result) {
 if (!result) throw py::error_already_set();
 return py::reinterpret_steal<py::object>(result);
}
py::object invoke(py::handle callable, py::handle args, py::handle kwargs) { return owned_result(PyObject_Call(callable.ptr(), args.ptr(), kwargs.is_none() ? nullptr : kwargs.ptr())); }
py::object item(py::handle value, py::handle index) { return owned_result(PyObject_GetItem(value.ptr(), index.ptr())); }
py::object index(py::handle value, py::ssize_t offset) { return item(value, py::int_(offset)); }
py::object add(py::handle left, py::handle right) { return owned_result(PyNumber_Add(left.ptr(), right.ptr())); }
Json sequence_json(py::handle value);
Json to_json(py::handle value) {
 if (value.is_none()) return nullptr;
 if (PyBool_Check(value.ptr())) return value.is(py::bool_(true));
 if (PyLong_Check(value.ptr())) return py::cast<std::int64_t>(value);
 if (PyFloat_Check(value.ptr())) {
  const double number = py::cast<double>(value);
  if (!std::isfinite(number)) throw std::runtime_error("out of range float values are not JSON compliant");
  return number;
 }
 if (PyUnicode_Check(value.ptr())) return py::cast<std::string>(value);
 if (PyDict_Check(value.ptr())) {
  Json result = Json::object();
  for (const auto& pair : py::reinterpret_borrow<py::dict>(value)) {
   if (!PyUnicode_Check(pair.first.ptr())) throw std::runtime_error("oracle JSON key is not a string");
   result[py::cast<std::string>(pair.first)] = to_json(pair.second);
  }
  return result;
 }
 if (PyList_Check(value.ptr()) || PyTuple_Check(value.ptr())) return sequence_json(value);
 throw std::runtime_error("non-JSON oracle value: " + string(py::type::of(value)));
}
Json sequence_json(py::handle value) {
 Json result = Json::array();
 for (py::handle element : py::reinterpret_borrow<py::iterable>(value)) result.push_back(to_json(element));
 return result;
}
Json read_json(const fs::path& path) {
 if (!fs::is_regular_file(path) || fs::is_symlink(path) || fs::file_size(path) > MaxManifest) throw std::runtime_error("invalid or oversized evidence JSON");
 std::vector<std::set<std::string>> keys;
 return Json::parse(support::read_text(path), [&](int, Json::parse_event_t event, Json& value) {
  if (event == Json::parse_event_t::object_start)
   keys.emplace_back();
  else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
   throw std::runtime_error("duplicate evidence JSON key");
  else if (event == Json::parse_event_t::object_end)
   keys.pop_back();
  return true;
 });
}
const Json& source_pins() {
 static const auto pins = read_json(OracleRoot / "sources.json");
 return pins;
}
std::string core_revision() { return source_pins().at("core").at("revision").get<std::string>(); }
void check_signals() {
 if (PyErr_CheckSignals() < 0) throw py::error_already_set();
}
std::string digest(const fs::path& path) {
 support::FileHasher hasher(1024 * 1024);
 return hasher(path, check_signals);
}
void json_write(const fs::path& path, const Json& value) {
 // to_json rejects nonfinite external scalars; native manifest scalars are
 // validated here too, because nlohmann would otherwise serialize NaN as null.
 const auto finite = [](const auto& self, const Json& node) -> void {
  if (node.is_number_float() && !std::isfinite(node.get<double>())) throw std::runtime_error("out of range float values are not JSON compliant");
  if (node.is_structured())
   for (const auto& child : node) self(self, child);
 };
 finite(finite, value);
 const auto data = value.dump(2, ' ', true);
 if (data.size() > MaxManifest) throw std::runtime_error("oracle manifest exceeds its declared bound");
 const fs::path temporary = path.string() + ".partial";
 support::FileDescriptor file(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0666));
 if (file.get() < 0) throw std::system_error(errno, std::generic_category(), "open " + temporary.string());
 support::write_all(file.get(), data, "write oracle manifest");
 if (::fsync(file.get()) != 0) throw std::system_error(errno, std::generic_category(), "fsync oracle manifest");
 file.reset();
 fs::rename(temporary, path);
}
Json payload() {
 const auto torch = module("torch");
 const auto torchvision = module("torchvision");
 const auto metadata = module("importlib.metadata");
 std::set<std::string> names{"torch", "torchvision", "triton"};
 for (py::handle distribution : metadata.attr("distributions")()) {
  const py::object name = object(distribution).attr("metadata")["Name"];
  if (truth(object(name).attr("lower")().attr("startswith")("nvidia-"))) names.insert(string(name));
 }
 Json distributions = Json::object();
 for (const auto& name : names) distributions[name] = string(metadata.attr("version")(name));
 return {
  {"distributions", distributions}, {"torch", to_json(torch.attr("__version__"))}, {"torchvision", to_json(torchvision.attr("__version__"))}, {"cuda", to_json(torch.attr("version").attr("cuda"))}
 };
}
Json check_environment() {
 auto current = payload();
 const auto expected = read_json(OracleRoot / "payload.json");
 if (current != expected) throw std::runtime_error("authoritative NGC payload replaced: " + current.dump() + " != " + expected.dump());
 const auto manifest = read_json(OracleRoot / "nvidia-payload.json");
 if (!current.at("torch").get<std::string>().starts_with(manifest.at("versions").at("torch").get<std::string>())) throw std::runtime_error("oracle Torch does not match docker/nvidia-payload.json");
 const auto metadata = module("importlib.metadata");
 const auto requirement_type = module("packaging.requirements").attr("Requirement");
 const auto canonicalize = module("packaging.utils").attr("canonicalize_name");
 const auto canonical = [&](py::handle name) { return string(canonicalize(name)); };
 std::vector<std::pair<std::string, std::string>> pending;
 for (const auto& line : support::lines(support::read_text(OracleRoot / "requirements.txt"))) {
  if (line.empty() || line.starts_with('#')) continue;
  const auto separator = line.find("==");
  if (separator == std::string::npos || line.find("==", separator + 2) != std::string::npos) throw std::runtime_error("oracle dependency must have exactly one == pin: " + line);
  const auto name = line.substr(0, separator);
  const auto version = line.substr(separator + 2);
  if (string(metadata.attr("version")(name)) != version) throw std::runtime_error("incompatible oracle dependency " + name + ": expected " + version);
  pending.emplace_back(canonical(py::str(name)), "");
 }
 std::set<std::string> authoritative;
 for (const auto& [name, version] : current.at("distributions").items()) authoritative.insert(canonical(py::str(name)));
 pending.emplace_back("fsspec", "http");
 pending.emplace_back("torchmetrics", "detection");
 pending.emplace_back("jsonargparse", "signatures");
 std::set<std::pair<std::string, std::string>> seen;
 while (!pending.empty()) {
  auto [name, extra] = std::move(pending.back());
  pending.pop_back();
  if (authoritative.contains(name) || !seen.emplace(name, extra).second) continue;
  const auto declarations = metadata.attr("requires")(name);
  if (declarations.is_none()) continue;
  for (py::handle declaration : declarations) {
   const auto requirement = requirement_type(declaration);
   const auto marker = requirement.attr("marker");
   if (truth(marker) && !truth(marker.attr("evaluate")(py::dict("extra"_a = extra)))) continue;
   const auto version = metadata.attr("version")(requirement.attr("name"));
   if (!truth(requirement.attr("specifier").attr("contains")(version, "prereleases"_a = true)))
    throw std::runtime_error("incompatible dependency closure: " + name + "[" + extra + "] requires " + string(requirement) + ", installed " + string(version));
   const auto dependency = canonical(requirement.attr("name"));
   pending.emplace_back(dependency, "");
   for (py::handle selected : requirement.attr("extras")) pending.emplace_back(dependency, string(selected));
  }
 }
 for (const char* name : {"transformers", "pytorch_lightning", "torchmetrics", "torch_linear_assignment"}) module(name);  // Actual eager numerical and Lightning import closure.
 current["dependency_closure"] = Json::object();
 for (const auto& [name, extra] : seen) current["dependency_closure"][name] = string(metadata.attr("version")(name));
 return current;
}
Json precision_capability() {
 const auto cuda = module("torch").attr("cuda");
 if (!truth(cuda.attr("is_available")())) throw std::runtime_error("required CUDA device unavailable");
 return {{"fp32", true}, {"fp16", true}, {"bf16", truth(cuda.attr("is_bf16_supported")("including_emulation"_a = false))}, {"device_capability", to_json(cuda.attr("get_device_capability")(0))}};
}
Json verify_source(const fs::path& source, const std::string& kind = "core") {
 const auto& pin = source_pins().at(kind);
 const auto git = [&](std::initializer_list<std::string> arguments) {
  std::vector<std::string> command{"git", "-c", "safe.directory=" + source.string(), "-C", source.string()};
  command.insert(command.end(), arguments.begin(), arguments.end());
  support::RunOptions options;
  options.capture_stdout = true;
  options.env = {{"GIT_DIR", source.string() + "-git"}, {"GIT_COMMON_DIR", source.string() + "-common"}, {"GIT_WORK_TREE", source.string()}};
  const auto result = support::run(command, options);
  check_signals();
  if (result.exit_code) throw std::runtime_error("upstream git validation failed with status " + std::to_string(result.exit_code));
  return support::trim(result.out);
 };
 if (git({"rev-parse", "HEAD"}) != pin.at("revision").get<std::string>() || git({"rev-parse", "HEAD^{tree}"}) != pin.at("tree").get<std::string>() ||
     !git({"status", "--porcelain", "--untracked-files=all"}).empty())
  throw std::runtime_error("upstream must be the clean pinned " + kind + " source");
 if (!git({"diff", "--name-only", "HEAD"}).empty()) throw std::runtime_error("upstream source changed");
 std::vector<fs::path> files{source / "README.md", source / "LICENSE"};
 const auto package = source / "src" / pin.at("package").get<std::string>();
 if (fs::exists(package)) {
  for (const auto& entry : fs::recursive_directory_iterator(package))
   if (entry.is_regular_file()) files.push_back(entry.path());
 }
 std::sort(files.begin(), files.end());
 Json result = Json::object();
 for (const auto& path : files) result[path.lexically_relative(source).generic_string()] = digest(path);
 return result;
}
// Context managers and monkey-patches always restore on C++/Python exceptions.
// The torch contexts used here do not suppress exceptions or inspect exc_info.
class PythonContext {
public:
 explicit PythonContext(py::object context) : context_(std::move(context)) { context_.attr("__enter__")(); }
 ~PythonContext() { close_noexcept(); }
 PythonContext(const PythonContext&) = delete;
 PythonContext& operator=(const PythonContext&) = delete;
 void close() {
  if (!context_) return;
  auto context = std::exchange(context_, py::object());
  context.attr("__exit__")(py::none(), py::none(), py::none());
 }

private:
 void close_noexcept() noexcept {
  try {
   close();
  } catch (py::error_already_set& error) { error.discard_as_unraisable("oracle context restoration"); } catch (const std::exception& error) {
   std::cerr << "oracle context restoration failed: " << error.what() << '\n';
  }
 }
 py::object context_;
};
class AttributePatches {
public:
 ~AttributePatches() { restore_noexcept(); }
 AttributePatches() = default;
 AttributePatches(const AttributePatches&) = delete;
 AttributePatches& operator=(const AttributePatches&) = delete;
 void set(py::object owner, const char* name, py::object replacement) {
  originals_.push_back({owner, name, owner.attr(name)});
  owner.attr(name) = std::move(replacement);
 }
 void restore() {
  std::exception_ptr first;
  while (!originals_.empty()) {
   auto patch = std::move(originals_.back());
   originals_.pop_back();
   try {
    patch.owner.attr(patch.name.c_str()) = patch.original;
   } catch (...) {
    if (!first) first = std::current_exception();
   }
  }
  if (first) std::rethrow_exception(first);
 }

private:
 void restore_noexcept() noexcept {
  try {
   restore();
  } catch (py::error_already_set& error) { error.discard_as_unraisable("oracle attribute restoration"); } catch (const std::exception& error) {
   std::cerr << "oracle attribute restoration failed: " << error.what() << '\n';
  }
 }
 struct Patch {
  py::object owner;
  std::string name;
  py::object original;
 };
 std::vector<Patch> originals_;
};
Json file_record(const fs::path& path) { return {{"file", path.filename().string()}, {"sha256", digest(path)}, {"bytes", fs::file_size(path)}}; }
fs::path admitted_file(const fs::path& directory, const std::string& name) {
 if (fs::is_symlink(directory) || name.empty() || name.size() > 256 || fs::path(name).filename() != name || name == "." || name == ".." || fs::is_symlink(directory / name))
  throw std::runtime_error("invalid evidence filename");
 return directory / name;
}
struct EvidenceFile {
 fs::path path;
 std::uint64_t bytes;
};
EvidenceFile admitted_file_record(const fs::path& directory, const Json& reference) {
 return {admitted_file(directory, reference.at("file").get<std::string>()), reference.at("bytes").get<std::uint64_t>()};
}
Json authenticated_json(const fs::path& directory, const Json& reference) {
 const auto [path, bytes] = admitted_file_record(directory, reference);
 if (!bytes || bytes > MaxManifest || fs::file_size(path) != bytes || digest(path) != reference.at("sha256").get<std::string>()) throw std::runtime_error("completed evidence index changed");
 return read_json(path);
}
void publish_file(const fs::path& destination, const std::function<void(const fs::path&)>& write) {
 const fs::path temporary = destination.string() + ".partial";
 fs::remove(temporary);
 write(temporary);
 support::FileDescriptor file(::open(temporary.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
 if (file.get() < 0 || ::fsync(file.get()) != 0) throw std::system_error(errno, std::generic_category(), "sync evidence product");
 file.reset();
 fs::rename(temporary, destination);
}
void verify_archive(const fs::path& directory, const Json& reference) {
 const auto index = authenticated_json(directory, reference);
 if (index.at("format") != "mmltk.rfdetr.step.v2" || index.at("shards").empty() || index.at("shards").size() > MaxShards || index.at("tensor_count").get<std::size_t>() > MaxTensors)
  throw std::runtime_error("invalid completed step index");
 std::uint64_t total = 0, metadata = 0;
 std::set<std::string> files, names;
 for (const auto& ref : index.at("shards")) {
  if (!files.insert(ref.at("file").get<std::string>()).second) throw std::runtime_error("duplicate completed shard index");
  const auto index_bytes = ref.at("bytes").get<std::uint64_t>();
  if (index_bytes > MaxManifest - metadata) throw std::runtime_error("unbounded completed step metadata");
  metadata += index_bytes;
  const auto shard = authenticated_json(directory, ref);
  for (const auto& [name, spec] : shard.at("tensors").items())
   if (!names.insert(name).second || names.size() > MaxTensors) throw std::runtime_error("duplicate completed tensor");
  const auto [path, bytes] = admitted_file_record(directory, shard);
  if (!files.insert(path.filename().string()).second || !bytes || bytes > MaxArchive || bytes > MaxStepStorage - total || fs::file_size(path) != bytes ||
      digest(path) != shard.at("sha256").get<std::string>())
   throw std::runtime_error("completed tensor shard changed");
  total += bytes;
 }
 if (total != index.at("bytes") || names.size() != index.at("tensor_count")) throw std::runtime_error("completed step storage differs");
}
class Archive {
public:
 Archive(fs::path directory, std::string name) : directory_(std::move(directory)), name_(std::move(name)) {}
 void add(const std::string& name, py::handle value) {
  const auto torch = module("torch");
  if (name.empty() || name.size() > 512 || name.starts_with("finite/") || !names_.insert(name).second || names_.size() > MaxTensors)
   throw std::runtime_error("invalid or duplicate oracle tensor " + name);
  if (value.is_none()) {
   inventory_[name] = {{"defined", false}};
   return;
  }
  if (!py::isinstance(value, torch.attr("Tensor"))) throw py::type_error(name);
  const bool floating = truth(object(value).attr("is_floating_point")());
  const auto elements = static_cast<std::uint64_t>(integer(object(value).attr("numel")()));
  const auto width = static_cast<std::uint64_t>(integer(object(value).attr("element_size")())) + floating;
  if (!width || elements > (MaxArchive - MaxManifest) / width) throw std::runtime_error("unbounded logical tensor");
  const auto bytes = elements * width;
  if (!inventory_.empty() && bytes > ShardTarget - std::min(pending_bytes_, ShardTarget)) flush();
  const auto tensor = object(value).attr("detach")().attr("as_subclass")(torch.attr("Tensor")).attr("cpu")().attr("contiguous")().attr("clone")();
  const auto finite = floating ? torch.attr("isfinite")(tensor) : torch.attr("ones_like")(tensor, "dtype"_a = torch.attr("bool"));
  tensors_[py::str(name)] = tensor;
  if (floating) tensors_[py::str("finite/" + name)] = finite;
  inventory_[name] = {
   {"defined", true}, {"shape", sequence_json(object(value).attr("shape"))}, {"dtype", string(object(value).attr("dtype"))}, {"device", string(object(value).attr("device"))},
   {"stride", sequence_json(object(value).attr("stride")())}, {"finite_count", integer(finite.attr("sum")())}, {"elements", elements}
  };
  pending_bytes_ += bytes;
 }
 Json write() {
  flush();
  const auto path = directory_ / (name_ + ".json");
  json_write(path, {{"format", "mmltk.rfdetr.step.v2"}, {"shards", shards_}, {"tensor_count", names_.size()}, {"bytes", total_bytes_}});
  return file_record(path);
 }

private:
 void flush() {
  if (inventory_.empty()) return;
  if (shards_.size() >= MaxShards) throw std::runtime_error("too many physical shards");
  const auto path = directory_ / (name_ + "-shard" + std::to_string(shards_.size()) + ".pt");
  publish_file(path, [&](const auto& temporary) { module("torch").attr("save")(tensors_, temporary.string()); });
  const auto bytes = fs::file_size(path);
  if (!bytes || bytes > MaxArchive || bytes > MaxStepStorage - total_bytes_) throw std::runtime_error("unbounded serialized step storage");
  auto record = file_record(path);
  record["tensors"] = std::move(inventory_);
  const fs::path metadata = path.string() + ".json";
  json_write(metadata, record);
  const auto index_bytes = fs::file_size(metadata);
  if (index_bytes > MaxManifest - metadata_bytes_) throw std::runtime_error("unbounded step metadata");
  metadata_bytes_ += index_bytes;
  shards_.push_back(file_record(metadata));
  total_bytes_ += bytes;
  tensors_ = py::dict();
  inventory_ = Json::object();
  pending_bytes_ = 0;
 }
 fs::path directory_;
 std::string name_;
 py::dict tensors_;
 Json inventory_ = Json::object(), shards_ = Json::array();
 std::set<std::string> names_;
 std::uint64_t pending_bytes_ = 0, total_bytes_ = 0, metadata_bytes_ = 0;
};
using NamedTensors = std::vector<std::pair<std::string, py::object>>;
NamedTensors output_tensors(py::dict outputs) {
 std::vector<std::pair<std::string, py::dict>> layers{{"main", outputs}};
 if (outputs.contains("aux_outputs")) {
  std::size_t layer = 0;
  for (py::handle value : outputs["aux_outputs"]) layers.emplace_back("aux." + std::to_string(layer++), py::cast<py::dict>(value));
 }
 if (outputs.contains("enc_outputs")) layers.emplace_back("enc", py::cast<py::dict>(outputs["enc_outputs"]));
 static const std::set<std::string> Allowed{"pred_logits", "pred_boxes", "pred_masks", "pred_keypoints", "aux_outputs", "enc_outputs"};
 NamedTensors result;
 for (const auto& [label, layer] : layers) {
  for (const auto& pair : layer)
   if (!Allowed.contains(string(pair.first))) throw std::runtime_error("unexpected upstream output inventory in " + label + ": " + string(layer));
  for (const char* key : {"pred_logits", "pred_boxes", "pred_masks", "pred_keypoints"}) {
   if (!layer.contains(key)) continue;
   const auto value = object(layer[key]);
   if (py::isinstance<py::dict>(value)) {
    for (const auto& pair : value.cast<py::dict>()) result.emplace_back(label + "/" + key + "/" + string(pair.first), object(pair.second));
   } else
    result.emplace_back(label + "/" + key, value);
  }
 }
 return result;
}
py::object current_python_frame() {
 // A C++ callback adds no Python frame. PyEval_GetFrame therefore is the
 // original Python wrapper's sys._getframe(1), preserving caller filters.
 PyFrameObject* frame = PyEval_GetFrame();
 return frame ? object(py::handle(reinterpret_cast<PyObject*>(frame))) : py::none();
}
bool frame_from(py::handle frame, std::string_view suffix) { return !frame.is_none() && string(object(frame).attr("f_code").attr("co_filename")).ends_with(suffix); }
py::object detached_clone(py::handle value) { return object(value).attr("detach")().attr("clone")(); }
class ForwardOperators {
public:
 struct State {
  NamedTensors outputs;
  NamedTensors gradients;
  std::vector<std::unique_ptr<PythonContext>> gradient_hooks;
  std::map<std::string, std::size_t> counts;
  bool active = true;
  bool backward = false;
 };
 explicit ForwardOperators(bool backward) : state_(std::make_shared<State>()) {
  state_->backward = backward;
  const auto functional = module("torch.nn.functional");
  for (const char* name : {"conv2d", "layer_norm", "linear", "gelu", "scaled_dot_product_attention"}) {
   const py::object original = functional.attr(name);
   auto callback = py::cpp_function([state = state_, name = std::string(name), original](py::args args, py::kwargs kwargs) {
    auto output = invoke(original, args, kwargs);
    if (state->active || state->backward) {
     if (state->outputs.size() >= 256) throw std::runtime_error("forward operator observation exceeds its bound");
     auto prefix = "forward-operator/" + name;
     if (!state->active) {
      prefix = "decoder-" + prefix + "/";
      for (py::handle dimension : output.attr("shape")) prefix += std::to_string(integer(dimension)) + "x";
     }
     const auto ordinal = state->counts[prefix]++;
     auto key = prefix + "/" + std::to_string(ordinal);
     state->outputs.emplace_back(key, detached_clone(output));
     if (state->backward) {
      key.replace(key.find("forward"), 7, "backward");
      const auto slot = state->gradients.size();
      state->gradients.emplace_back(key, py::none());
      if (truth(output.attr("requires_grad")))
       state->gradient_hooks.push_back(std::make_unique<PythonContext>(output.attr("register_hook")(py::cpp_function([state, slot](py::object gradient) {
        state->gradients[slot].second = gradient.attr("detach")();
        return gradient;
       }))));
     }
    }
    return output;
   });
   patches_.set(functional, name, std::move(callback));
  }
 }
 void close() { patches_.restore(); }
 void save(Archive& archive) const {
  for (const auto& [name, value] : state_->outputs) archive.add(name, value);
 }
 void save_gradients(Archive& archive) {
  state_->gradient_hooks.clear();
  for (const auto& [name, value] : state_->gradients) archive.add(name, value);
 }
 const std::shared_ptr<State>& state() const { return state_; }

private:
 std::shared_ptr<State> state_;
 AttributePatches patches_;
};
class Observe {
public:
 explicit Observe(ForwardOperators* operators = nullptr) : state_(std::make_shared<State>()), sys_(module("sys")), previous_profile_(sys_.attr("getprofile")()) {
  const auto torch = module("torch");
  const py::object original_rand = torch.attr("rand");
  patches_.set(torch, "rand", py::cpp_function([state = state_, original_rand](py::args args, py::kwargs kwargs) {
   auto result = invoke(original_rand, args, kwargs);
   bool uncertain = false;
   bool matcher = false;
   for (auto frame = current_python_frame(); !frame.is_none(); frame = frame.attr("f_back")) {
    uncertain = uncertain || string(frame.attr("f_code").attr("co_name")) == "get_uncertain_point_coords_with_randomness";
    matcher = matcher || frame_from(frame, "/rfdetr/models/matcher.py");
   }
   if (uncertain)
    state->uncertain.push_back(detached_clone(result));
   else if (matcher)
    state->matcher.push_back(detached_clone(result));
   return result;
  }));
  const py::object original_topk = torch.attr("topk");
  const auto operator_state = operators ? operators->state() : std::shared_ptr<ForwardOperators::State>();
  patches_.set(torch, "topk", py::cpp_function([state = state_, original_topk, operator_state](py::args args, py::kwargs kwargs) {
   auto result = invoke(original_topk, args, kwargs);
   if (frame_from(current_python_frame(), "/rfdetr/models/transformer.py")) {
    if (operator_state) operator_state->active = false;
    state->encoder.push_back(detached_clone(result.attr("indices")));
   }
   return result;
  }));
  auto profile = py::cpp_function([state = state_, previous = previous_profile_](py::object frame, py::str event, py::object result) {
   if (string(event) == "return" && frame_from(frame, "/rfdetr/models/criterion.py")) {
    const auto function = string(frame.attr("f_code").attr("co_name"));
    const py::dict locals = frame.attr("f_locals");
    if (function == "forward" && locals.contains("all_indices")) state->indices = object(locals["all_indices"]);
    if (function == "loss_masks" && locals.contains("point_logits")) {
     NamedTensors direct;
     for (const char* name : {"point_coords", "point_logits", "point_labels"}) direct.emplace_back(name, detached_clone(locals[name]));
     state->direct.push_back(std::move(direct));
    }
   }
   if (!previous.is_none()) previous(frame, event, result);
  });
  try {
   sys_.attr("setprofile")(profile);
   profiling_ = true;
  } catch (...) {
   sys_.attr("setprofile")(previous_profile_);
   throw;
  }
 }
 ~Observe() {
  try {
   close();
  } catch (py::error_already_set& error) { error.discard_as_unraisable("oracle observation restoration"); } catch (const std::exception& error) {
   std::cerr << "oracle observation restoration failed: " << error.what() << '\n';
  }
 }
 Observe(const Observe&) = delete;
 Observe& operator=(const Observe&) = delete;
 void close() {
  // Match the original exit ordering: restore torch before the profiler.
  std::exception_ptr first;
  try {
   patches_.restore();
  } catch (...) { first = std::current_exception(); }
  if (profiling_) {
   profiling_ = false;
   try {
    sys_.attr("setprofile")(previous_profile_);
   } catch (...) {
    if (!first) first = std::current_exception();
   }
  }
  if (first) std::rethrow_exception(first);
 }
 void save(Archive& archive, const std::string& prefix = {}) const {
  for (std::size_t group = 0; group != state_->encoder.size(); ++group) archive.add(prefix + "encoder/" + std::to_string(group), state_->encoder[group]);
  std::size_t layer = 0;
  for (py::handle matches : state_->indices) {
   std::size_t image = 0;
   for (py::handle pair : py::reinterpret_borrow<py::iterable>(matches)) {
    const py::sequence indices = py::reinterpret_borrow<py::sequence>(pair);
    if (indices.size() != 2) throw std::runtime_error("upstream assignment pair has changed");
    const auto base = prefix + "assignment/" + std::to_string(layer) + "/" + std::to_string(image++);
    archive.add(base + "/query", indices[0]);
    archive.add(base + "/target", indices[1]);
   }
   ++layer;
  }
  if (!state_->matcher.empty()) {
   if (state_->matcher.size() != layer || state_->uncertain.size() != 2 * layer || state_->direct.size() != layer)
    throw std::runtime_error("upstream mask sampling inventory changed; cannot map explicit native samples");
   for (std::size_t i = 0; i != layer; ++i) {
    const auto suffix = std::to_string(i);
    archive.add(prefix + "samples/" + suffix + "/matcher", state_->matcher[i]);
    archive.add(prefix + "samples/" + suffix + "/candidates", state_->uncertain[i * 2]);
    archive.add(prefix + "samples/" + suffix + "/remainder", state_->uncertain[i * 2 + 1]);
    for (const auto& [key, value] : state_->direct[i]) archive.add(prefix + "direct/" + suffix + "/" + key, value);
   }
  }
 }

private:
 struct State {
  py::object indices = py::list();
  std::vector<py::object> encoder;
  std::vector<py::object> matcher;
  std::vector<py::object> uncertain;
  std::vector<NamedTensors> direct;
 };
 std::shared_ptr<State> state_;
 py::module_ sys_;
 py::object previous_profile_;
 bool profiling_ = false;
 AttributePatches patches_;
};
py::object normalized_input(int batch, int resolution, int offset = 0) {
 const auto torch = module("torch");
 const auto count = static_cast<std::int64_t>(batch) * 3 * resolution * resolution;
 auto pixels = torch.attr("arange")(count, "dtype"_a = torch.attr("int64")).attr("reshape")(batch, 3, resolution, resolution);
 pixels = add(pixels.attr("__mul__")(37), pixels.attr("__floordiv__")(resolution).attr("__mul__")(17));
 pixels = pixels.attr("__add__")(offset).attr("__mod__")(256).attr("float")().attr("__truediv__")(255);
 const auto mean = torch.attr("tensor")(py::make_tuple(.485, .456, .406)).attr("reshape")(1, 3, 1, 1);
 const auto deviation = torch.attr("tensor")(py::make_tuple(.229, .224, .225)).attr("reshape")(1, 3, 1, 1);
 return pixels.attr("__sub__")(mean).attr("__truediv__")(deviation);
}
py::list targets_for(int batch, int resolution, bool segmentation, const std::vector<int>& joint_counts = {}) {
 const auto torch = module("torch");
 py::list targets;
 for (int image = 0; image != batch; ++image) {
  const int count = image == 0 ? 2 : 0;
  py::list labels;
  py::list boxes;
  if (count) {
   const auto active = std::ranges::find_if(joint_counts, [](int count) { return count > 0; });
   labels.append(joint_counts.empty() ? 1 : std::distance(joint_counts.begin(), active));
   labels.append(joint_counts.empty() ? 3 : std::distance(joint_counts.begin(), active));
   boxes.append(py::make_tuple(.31, .34, .22, .28));
   boxes.append(py::make_tuple(.72, .65, .19, .24));
  }
  py::dict target;
  target["labels"] = torch.attr("tensor")(labels, "dtype"_a = torch.attr("int64"), "device"_a = "cuda");
  target["boxes"] = torch.attr("tensor")(boxes, "dtype"_a = torch.attr("float32"), "device"_a = "cuda").attr("reshape")(count, 4);
  if (!joint_counts.empty()) {
   const int joints = *std::ranges::max_element(joint_counts);
   py::list rows;
   for (int row = 0; row < count; ++row) {
    py::list points;
    for (int joint = 0; joint < joints; ++joint) points.append(py::make_tuple(.2 + .01 * joint + .3 * row, .3 + .008 * joint, joint % 3));
    rows.append(points);
   }
   target["keypoints"] = torch.attr("tensor")(rows, "dtype"_a = torch.attr("float32"), "device"_a = "cuda").attr("reshape")(count, joints, 3);
  }
  if (segmentation) {
   const auto masks = torch.attr("zeros")(count, resolution, resolution, "dtype"_a = torch.attr("bool"), "device"_a = "cuda");
   for (int row = 0; row != count; ++row) {
    const auto lo = resolution * (row + 1) / 5;
    const auto hi = resolution * (row + 2) / 5;
    const auto rows = py::slice(py::int_(lo), py::int_(hi), py::none());
    const auto columns = py::slice(py::int_(lo), py::int_(hi + 3), py::none());
    masks.attr("__setitem__")(py::make_tuple(row, rows, columns), true);
   }
   target["masks"] = masks;
  }
  targets.append(target);
 }
 return targets;
}
void save_targets(Archive& archive, const py::list& targets) {
 std::size_t image = 0;
 for (py::handle value : targets) {
  for (const auto& [name, tensor] : py::reinterpret_borrow<py::dict>(value)) archive.add("target/" + std::to_string(image) + "/" + string(name), tensor);
  ++image;
 }
}
py::object float_operands(py::handle value) {
 if (py::isinstance(value, module("torch").attr("Tensor"))) return truth(object(value).attr("is_floating_point")()) ? object(value).attr("float")() : object(value);
 if (py::isinstance<py::dict>(value)) {
  py::dict result;
  for (const auto& pair : py::reinterpret_borrow<py::dict>(value)) result[pair.first] = float_operands(pair.second);
  return result;
 }
 if (py::isinstance<py::list>(value)) {
  py::list result;
  for (py::handle element : py::reinterpret_borrow<py::list>(value)) result.append(float_operands(element));
  return result;
 }
 return object(value);
}
class Fp32MatcherCosts {
public:
 Fp32MatcherCosts() {
  const auto torch = module("torch");
  const auto matcher = module("rfdetr.models.matcher");
  install(matcher.attr("HungarianMatcher"), "_focal_classification_cost", true, false);
  for (const char* name : {"box_cxcywh_to_xyxy", "batch_sigmoid_ce_loss", "batch_dice_loss"}) install(matcher, name, false, false);
  install(torch, "cdist", false, true);
 }
 void close() { patches_.restore(); }

private:
 void install(py::object owner, const char* name, bool method, bool caller_only) {
  const py::object original = owner.attr(name);
  py::object callback = py::cpp_function([original, caller_only](py::args args, py::kwargs kwargs) {
   if (caller_only && !frame_from(current_python_frame(), "/rfdetr/models/matcher.py")) return invoke(original, args, kwargs);
   PythonContext autocast(module("torch").attr("autocast")("cuda", "enabled"_a = false));
   py::tuple positional(args.size());
   for (std::size_t i = 0; i != args.size(); ++i) positional[i] = float_operands(args[i]);
   py::dict keyword;
   for (const auto& pair : kwargs) keyword[pair.first] = float_operands(pair.second);
   auto result = invoke(original, positional, keyword);
   autocast.close();
   return result;
  });
  if (method) {
   // A builtin C callback has no function descriptor. The instance
   // method descriptor supplies self exactly as the replaced Python
   // method did; its equations still execute in the pinned source.
   auto* descriptor = PyInstanceMethod_New(callback.ptr());
   if (!descriptor) throw py::error_already_set();
   callback = py::reinterpret_steal<py::object>(descriptor);
  }
  patches_.set(std::move(owner), name, std::move(callback));
 }
 AttributePatches patches_;
};
py::object save_losses(Archive& archive, const std::string& prefix, py::dict losses, py::dict weights) {
 py::list terms;
 for (const auto& pair : losses) {
  if (weights.contains(pair.first)) terms.append(object(pair.second).attr("__mul__")(weights[pair.first]));
 }
 // RFDETRModelModule.training_step's actual weighted reduction order.
 const auto total = module("torch").attr("stack")(terms).attr("sum")();
 for (const auto& pair : losses) archive.add(prefix + (weights.contains(pair.first) ? "loss/" : "statistic/") + string(pair.first), pair.second);
 archive.add(prefix + "loss/total", total);
 return total;
}
py::list parameter_values(const NamedTensors& parameters) {
 py::list values;
 for (const auto& [name, value] : parameters) values.append(value);
 return values;
}
void save_gradients(Archive& archive, const std::string& prefix, const NamedTensors& names, py::sequence gradients) {
 if (names.size() != static_cast<std::size_t>(gradients.size())) throw std::runtime_error("upstream gradient inventory length changed");
 for (std::size_t i = 0; i != names.size(); ++i) archive.add(prefix + names[i].first, gradients[i]);
}
void save_parameter_gradients(Archive& archive, const std::string& prefix, const NamedTensors& parameters) {
 for (const auto& [name, parameter] : parameters) archive.add(prefix + name, parameter.attr("grad"));
}
void save_optimizer_state(Archive& archive, const std::string& prefix, py::object optimizer, const NamedTensors& parameters) {
 for (const auto& [name, parameter] : parameters) {
  archive.add(prefix + "updated/" + name, parameter);
  const auto state = optimizer.attr("state").attr("get")(parameter, py::dict());
  for (const char* field : {"step", "exp_avg", "exp_avg_sq"}) archive.add(prefix + "optimizer/" + name + "/" + field, state.attr("get")(field));
 }
}
py::object clip_gradients(const NamedTensors& parameters, py::handle max_norm) {
 return module("torch.nn.utils").attr("clip_grad_norm_")(parameter_values(parameters), max_norm, "foreach"_a = false, "error_if_nonfinite"_a = true);
}
void stock_update_evidence(Archive& archive, py::object optimizer, const NamedTensors& names, py::sequence gradients, py::handle max_norm) {
 std::unordered_map<PyObject*, std::size_t> index_by_id;
 NamedTensors shadow;
 shadow.reserve(names.size());
 for (const auto& [name, value] : names) {
  index_by_id.emplace(value.ptr(), shadow.size());
  shadow.emplace_back(name, detached_clone(value).attr("requires_grad_")(true));
 }
 py::list groups;
 for (py::handle group_handle : optimizer.attr("param_groups")) {
  const auto group = py::reinterpret_borrow<py::dict>(group_handle);
  py::dict copy;
  for (const auto& pair : group)
   if (string(pair.first) != "params") copy[pair.first] = pair.second;
  py::list parameters;
  for (py::handle parameter : group["params"]) parameters.append(shadow.at(index_by_id.at(parameter.ptr())).second);
  copy["params"] = parameters;
  groups.append(copy);
 }
 const auto stock_optimizer = module("torch.optim").attr("AdamW")(groups, "foreach"_a = false, "fused"_a = false);
 // Branch the real optimizer at this trajectory state. PyTorch owns both
 // state restoration and the stock update; the retained branch never mutates
 // the controlled model or optimizer used by the next update.
 stock_optimizer.attr("load_state_dict")(module("copy").attr("deepcopy")(optimizer.attr("state_dict")()));
 if (shadow.size() != static_cast<std::size_t>(gradients.size())) throw std::runtime_error("upstream stock gradient inventory length changed");
 for (std::size_t i = 0; i != shadow.size(); ++i) shadow[i].second.attr("grad") = gradients[i].is_none() ? py::none() : detached_clone(gradients[i]);
 archive.add("stock/clip_norm", clip_gradients(shadow, max_norm));
 save_parameter_gradients(archive, "stock/clipped/", shadow);
 stock_optimizer.attr("step")();
 save_optimizer_state(archive, "stock/", stock_optimizer, shadow);
}
Json optimizer_groups(py::object optimizer, const NamedTensors& names) {
 std::unordered_map<PyObject*, std::string> name_by_id;
 for (const auto& [name, value] : names) name_by_id.emplace(value.ptr(), name);
 Json groups = Json::array();
 for (py::handle group_handle : optimizer.attr("param_groups")) {
  const auto group = py::reinterpret_borrow<py::dict>(group_handle);
  Json entry = Json::object();
  entry["names"] = Json::array();
  for (py::handle parameter : group["params"]) entry["names"].push_back(name_by_id.at(parameter.ptr()));
  for (const char* key : {"lr", "weight_decay", "betas", "eps", "amsgrad"}) entry[key] = to_json(group[key]);
  groups.push_back(std::move(entry));
 }
 return groups;
}
Json save_export_reference(Archive& archive, py::object model, py::object configuration, py::object input, bool diagnostic, const fs::path& directory) {
 const auto torch = module("torch");
 // Execute the pinned export preparation and export forward on an owned copy.
 // Its normal CPU tracing policy is distinct from the eager CUDA reference.
 PythonContext random_state(torch.attr("random").attr("fork_rng")("devices"_a = py::list(py::make_tuple(torch.attr("cuda").attr("current_device")()))));
 PythonContext no_grad(torch.attr("no_grad")());
 const auto snapshot = module("copy").attr("deepcopy")(model);
 const auto shape = py::make_tuple(configuration.attr("resolution"), configuration.attr("resolution"));
 const auto graph =
  module("rfdetr.export.prepare").attr("prepare_export_graph")(snapshot, configuration, "shape"_a = shape, "device"_a = input.attr("device"), "batch_size"_a = integer(input.attr("size")(0)));
 const auto export_model = graph.attr("model");
 module("rfdetr.export._backend").attr("_switch_to_export_mode")(export_model);
 const bool masks = truth(configuration.attr("segmentation_head")), joints = truth(configuration.attr("use_grouppose_keypoints"));
 std::vector<std::string> roles{"pred_boxes", "pred_logits"};
 if (masks || joints) roles.emplace_back(masks ? "pred_masks" : "pred_keypoints");
 const auto capture = [&](const std::string& prefix, py::object pixels) {
  Observe observation;
  const auto result = export_model(pixels);
  observation.close();
  observation.save(archive, prefix);
  if (py::len(result) != roles.size()) throw std::runtime_error("upstream export output inventory changed");
  // The external export tuple is (boxes, logits[, masks or keypoints]).
  for (std::size_t role = 0; role < roles.size(); ++role) archive.add(prefix + "output/main/" + roles[role], index(result, role));
 };
 if (diagnostic) capture("export/", input.attr("detach")().attr("cpu")());
 export_model.attr("cuda")();
 if (diagnostic) capture("export-cuda/", input.attr("detach")());
 // Native export uses portable math attention. Preserve the ordinary case's
 // policy and training trajectory while evaluating its owned export copy under
 // the same backend; changed proposal order otherwise changes decoder slots.
 const auto attention = module("torch.nn.attention");
 PythonContext math(attention.attr("sdpa_kernel")(attention.attr("SDPBackend").attr("MATH")));
 capture("export-math/", input.attr("detach")());
 if (!diagnostic) return nullptr;
 export_model.attr("cpu")();
 const auto onnx = module("rfdetr.export._onnx.exporter");
 const auto options = onnx.attr("OnnxConfig")("output_dir"_a = directory.string(), "output_name"_a = "upstream", "opset_version"_a = 17);
 const fs::path artifact = string(onnx.attr("OnnxExporter")(options)(graph));
 if (fs::canonical(artifact.parent_path()) != fs::canonical(directory) || fs::is_symlink(artifact)) throw std::runtime_error("upstream exporter returned an unowned artifact");
 auto record = file_record(artifact);
 record["outputs"] = Json::object();
 const auto output_names = graph.attr("output_names");
 if (py::len(output_names) != roles.size()) throw std::runtime_error("upstream export graph output names changed");
 for (std::size_t role = 0; role < roles.size(); ++role) record["outputs"][roles[role]] = string(index(output_names, role));
 record["exporter"] = "rfdetr.export._onnx.exporter.OnnxExporter";
 record["onnx_version"] = string(module("onnx").attr("__version__"));
 return record;
}
Json run_case(const fs::path& directory, py::object model, py::dict initial, py::object mc, py::object tc, py::object defaults, const std::string& preset, const std::string& dtype_name, int batch,
 bool training, int steps, const std::string& protocol, bool backward_operators, bool export_reference) {
 const auto torch = module("torch");
 const auto build_criterion = module("rfdetr.models.lwdetr").attr("build_criterion_from_config");
 const auto get_param_dict = module("rfdetr.training.param_groups").attr("get_param_dict");
 const auto make_namespace = module("rfdetr._namespace").attr("_namespace_from_configs");
 const auto nested_tensor = module("rfdetr.utilities.tensors").attr("NestedTensor");
 const bool fp32 = dtype_name == "fp32";
 const py::object dtype = torch.attr(fp32 ? "float32" : dtype_name == "fp16" ? "float16" : "bfloat16");
 model.attr("load_state_dict")(initial, "strict"_a = true);
 model.attr("train")(training);
 const auto criterion = index(build_criterion(mc, tc, defaults), 0);
 criterion.attr("train")(training);
 const auto controlled = index(build_criterion(mc, tc, defaults), 0);
 controlled.attr("train")(training);
 NamedTensors names;
 NamedTensors trainable;
 for (py::handle parameter : model.attr("named_parameters")()) {
  const auto name = string(index(parameter, 0));
  const auto value = index(parameter, 1);
  names.emplace_back(name, value);
  if (truth(value.attr("requires_grad"))) trainable.emplace_back(name, value);
 }
 const auto optimizer =
  module("torch.optim")
   .attr("AdamW")(get_param_dict(make_namespace(mc, tc, defaults), model), "lr"_a = tc.attr("lr"), "weight_decay"_a = tc.attr("weight_decay"), "foreach"_a = false, "fused"_a = false);
 const auto groups = optimizer_groups(optimizer, names);
 const auto identity = preset + "-" + protocol + "-" + dtype_name + "-b" + std::to_string(batch) + (training ? "-train" : "-eval");
 const int resolution = py::cast<int>(mc.attr("resolution"));
 Json records = Json::array();
 Json execution_policy;
 Json upstream_export;
 for (int step = 0; step != steps; ++step) {
  Archive archive(directory, identity + "-step" + std::to_string(step));
  torch.attr("manual_seed")(731 + step);
  const auto images = normalized_input(batch, resolution, step * 11).attr("cuda")().attr("requires_grad_")(true);
  const auto mask = torch.attr("zeros")(batch, resolution, resolution, "dtype"_a = torch.attr("bool"), "device"_a = "cuda");
  const auto targets = targets_for(batch, resolution, truth(mc.attr("segmentation_head")), py::cast<std::vector<int>>(mc.attr("num_keypoints_per_class")));
  save_targets(archive, targets);
  archive.add("input", images);
  archive.add("input_mask", mask);
  optimizer.attr("zero_grad")("set_to_none"_a = true);
  std::unique_ptr<ForwardOperators> operators;
  if (step == 0) operators = std::make_unique<ForwardOperators>(backward_operators);
  Observe forward_observation(operators.get());
  py::dict outputs;
  {
   std::vector<std::unique_ptr<PythonContext>> keypoint_observers;
   if (truth(mc.attr("use_grouppose_keypoints"))) {
    for (const auto* name : {"keypoint_query_initializer", "keypoint_query_initializer_enc"}) {
     const auto handle = model.attr("transformer").attr(name).attr("register_forward_hook")(py::cpp_function([&archive, name](py::object, py::object args, py::object result) {
      const auto prefix = std::string("grouppose/") + name + "/";
      archive.add(prefix + "input", index(args, 0));
      archive.add(prefix + "output", result);
     }));
     keypoint_observers.push_back(std::make_unique<PythonContext>(handle));
    }
   }
   const auto hook = index(model.attr("backbone"), 0).attr("register_forward_hook")(py::cpp_function([&archive](py::object, py::object, py::object features) {
    std::size_t index = 0;
    for (py::handle family : features) {
     if (family.is_none()) continue;
     for (py::handle feature : family) archive.add("backbone/" + std::to_string(index++), object(feature).attr("tensors"));
    }
   }));
   PythonContext observed_features(hook);
   PythonContext autocast(torch.attr("autocast")("cuda", "dtype"_a = dtype, "enabled"_a = !fp32));
   execution_policy = {{"autocast_enabled", truth(torch.attr("is_autocast_enabled")("cuda"))}, {"autocast_dtype", string(torch.attr("get_autocast_dtype")("cuda"))}};
   outputs = model(nested_tensor(images, mask), "all_layer_masks"_a = true).cast<py::dict>();
   autocast.close();
  }
  forward_observation.close();
  if (operators) operators->close();
  forward_observation.save(archive);
  if (operators) operators->save(archive);
  const auto raw_outputs = output_tensors(outputs);
  for (const auto& [key, value] : raw_outputs) archive.add("output/" + key, value);
  if (step == 0) {
   py::object sum = py::int_(0);
   for (const auto& [key, value] : raw_outputs) {
    const auto elements = integer(value.attr("numel")());
    auto cotangent = torch.attr("arange")(elements, "device"_a = value.attr("device"), "dtype"_a = torch.attr("float32")).attr("reshape")(value.attr("shape"));
    cotangent = cotangent.attr("__mod__")(19).attr("__sub__")(9).attr("__truediv__")(std::max<std::int64_t>(elements, 1));
    cotangent = cotangent.attr("to")(value.attr("dtype"));
    archive.add("cotangent/" + key, cotangent);
    sum = add(sum, value.attr("__mul__")(cotangent).attr("sum")());
   }
   NamedTensors vjp_names{{"input", images}};
   for (const auto& [key, value] : trainable) vjp_names.emplace_back("parameter/" + key, value);
   const auto vjps = torch.attr("autograd").attr("grad")(sum, parameter_values(vjp_names), "allow_unused"_a = true, "retain_graph"_a = training);
   save_gradients(archive, "vjp/", vjp_names, vjps.cast<py::sequence>());
   operators->save_gradients(archive);
  }
  if (training) {
   const auto rng = torch.attr("cuda").attr("get_rng_state")();
   Observe stock;
   py::dict stock_losses;
   py::object stock_total;
   {
    PythonContext autocast(torch.attr("autocast")("cuda", "dtype"_a = dtype, "enabled"_a = !fp32));
    stock_losses = criterion(outputs, targets).cast<py::dict>();
    stock_total = save_losses(archive, "stock/", stock_losses, criterion.attr("weight_dict"));
    autocast.close();
   }
   stock.close();
   stock.save(archive, "stock/");
   const auto stock_gradients = torch.attr("autograd").attr("grad")(stock_total, parameter_values(trainable), "allow_unused"_a = true, "retain_graph"_a = true);
   save_gradients(archive, "stock/gradient/", trainable, stock_gradients.cast<py::sequence>());
   py::object total;
   std::unique_ptr<Observe> observation;
   if (fp32) {
    total = save_losses(archive, "", stock_losses, criterion.attr("weight_dict"));
    stock.save(archive);
   } else {
    stock_update_evidence(archive, optimizer, trainable, stock_gradients.cast<py::sequence>(), tc.attr("clip_max_norm"));
    torch.attr("cuda").attr("set_rng_state")(rng);
    Fp32MatcherCosts boundary;
    observation = std::make_unique<Observe>();
    {
     PythonContext autocast(torch.attr("autocast")("cuda", "dtype"_a = dtype));
     const auto losses = controlled(outputs, targets).cast<py::dict>();
     total = save_losses(archive, "", losses, controlled.attr("weight_dict"));
     autocast.close();
    }
    observation->close();
    boundary.close();
    observation->save(archive);
   }
   // K=1, fixed scale one: no replacement criterion/scaler equations.
   total.attr("backward")();
   save_parameter_gradients(archive, "gradient/", trainable);
   archive.add("clip_norm", clip_gradients(trainable, tc.attr("clip_max_norm")));
   save_parameter_gradients(archive, "clipped/", trainable);
   optimizer.attr("step")();
   save_optimizer_state(archive, "", optimizer, trainable);
   model.attr("eval")();
   const auto held_criterion = fp32 ? criterion : controlled;
   held_criterion.attr("eval")();
   const auto held = normalized_input(batch, resolution, 97).attr("cuda")();
   archive.add("held/input", held);
   torch.attr("manual_seed")(1701);
   py::dict held_outputs;
   {
    PythonContext no_grad(torch.attr("no_grad")());
    std::unique_ptr<Fp32MatcherCosts> boundary;
    if (!fp32) boundary = std::make_unique<Fp32MatcherCosts>();
    Observe held_observation;
    {
     PythonContext autocast(torch.attr("autocast")("cuda", "dtype"_a = dtype, "enabled"_a = !fp32));
     held_outputs = model(nested_tensor(held, mask), "all_layer_masks"_a = true).cast<py::dict>();
     const auto held_losses = held_criterion(held_outputs, targets).cast<py::dict>();
     save_losses(archive, "held/", held_losses, held_criterion.attr("weight_dict"));
     autocast.close();
    }
    held_observation.close();
    if (boundary) boundary->close();
    no_grad.close();
    held_observation.save(archive, "held/");
   }
   for (const auto& [key, value] : output_tensors(held_outputs)) archive.add("held/output/" + key, value);
   if (fp32 && batch == 1 && protocol == "controlled-group2") upstream_export = save_export_reference(archive, model, mc, held, export_reference, directory);
   model.attr("train")(true);
   held_criterion.attr("train")(true);
  } else if (fp32 && batch == 1 && protocol == "controlled-group2")
   upstream_export = save_export_reference(archive, model, mc, images, export_reference, directory);
  auto record = archive.write();
  record["step"] = step;
  records.push_back(std::move(record));
  std::cout << "upstream oracle produced " << identity << " step=" << step << std::endl;
 }
 Json trainable_names = Json::array();
 for (const auto& [name, value] : trainable) trainable_names.push_back(name);
 Json optimizer_settings = Json::object();
 for (const char* key : {"lr", "lr_encoder", "weight_decay", "lr_vit_layer_decay", "lr_component_decay"}) optimizer_settings[key] = to_json(tc.attr(key));
 return {
  {"id", identity}, {"protocol", protocol}, {"preset", preset}, {"precision", dtype_name}, {"batch", batch}, {"execution_policy", execution_policy},
  {"comparison_policy", fp32 || !training ? "untouched_stock" : "upstream_with_fp32_matcher_boundary"}, {"training", training}, {"resolution", resolution}, {"groups", to_json(mc.attr("group_detr"))},
  {"classes", py::cast<int>(mc.attr("num_classes")) + 1}, {"queries", to_json(mc.attr("num_queries"))}, {"keypoints_per_class", to_json(mc.attr("num_keypoints_per_class"))},
  {"trainable", trainable_names}, {"parameter_groups", groups}, {"loss_weights", to_json(criterion.attr("weight_dict"))}, {"clip_max_norm", to_json(tc.attr("clip_max_norm"))},
  {"optimizer", optimizer_settings}, {"steps", records}, {"upstream_export", upstream_export}
 };
}
Json constructor_evidence(py::object model) {
 const auto torch = module("torch");
 const py::dict state = model.attr("state_dict")();
 Json zero = Json::object();
 Json copies = Json::object();
 bool valid = true;
 for (const char* name : {"refpoint_embed.weight", "bbox_embed.layers.2.weight", "bbox_embed.layers.2.bias"}) {
  const bool is_zero = integer(torch.attr("count_nonzero")(state[name])) == 0;
  zero[name] = is_zero;
  valid = valid && is_zero;
 }
 const int groups = py::cast<int>(model.attr("group_detr"));
 const auto check_copy = [&](const std::string& destination, const std::string& source) {
  const bool equal = truth(torch.attr("equal")(state[py::str(destination)], state[py::str(source)]));
  copies[destination] = equal;
  valid = valid && equal;
 };
 for (int group = 0; group != groups; ++group) {
  for (const char* suffix : {"weight", "bias"}) check_copy("transformer.enc_out_class_embed." + std::to_string(group) + "." + suffix, std::string("class_embed.") + suffix);
  for (int layer = 0; layer != 3; ++layer) {
   for (const char* suffix : {"weight", "bias"})
    check_copy("transformer.enc_out_bbox_embed." + std::to_string(group) + ".layers." + std::to_string(layer) + "." + suffix, "bbox_embed.layers." + std::to_string(layer) + "." + suffix);
  }
 }
 if (!valid) throw std::runtime_error("pinned upstream constructor zero/copy invariants changed");
 return {{"zero", zero}, {"copy", copies}};
}
Json resize_evidence(const fs::path& directory) {
 const auto numpy = module("numpy");
 const auto torch = module("torch");
 const auto image_type = module("PIL.Image");
 const auto interpolation = module("torchvision.transforms").attr("InterpolationMode");
 const auto functional = module("torchvision.transforms.v2.functional");
 const auto transforms = module("torchvision.transforms.v2");
 const auto resize = module("rfdetr.datasets._torchvision").attr("Resize");
 const auto normalize = module("rfdetr.datasets.transforms").attr("Normalize");
 struct Case {
  const char* name;
  int height;
  int width;
  int out_height;
  int out_width;
 };
 constexpr std::array Cases{
  Case{"identity", 13, 17, 13, 17}, Case{"enlarge", 7, 11, 19, 27}, Case{"shrink", 37, 53, 11, 17}, Case{"odd_aspect", 9, 41, 23, 13}, Case{"high_frequency", 64, 65, 13, 17}
 };
 Archive archive(directory, "resize");
 Json case_records = Json::array();
 for (const auto& entry : Cases) {
  // This is the original bounded RGB8 test-pattern formula. Only the
  // source pixels are synthesized here; all resize/normalization paths
  // execute the real Pillow, torchvision, and upstream transforms.
  std::string bytes(static_cast<std::size_t>(entry.height) * entry.width * 3, '\0');
  for (int y = 0; y != entry.height; ++y) {
   for (int x = 0; x != entry.width; ++x) {
    const auto offset = static_cast<std::size_t>(y * entry.width + x) * 3;
    bytes[offset] = static_cast<char>((x * 31 + y * 17) % 256);
    bytes[offset + 1] = static_cast<char>(((x + y) % 2) * 255);
    bytes[offset + 2] = static_cast<char>((x * y * 13) % 256);
   }
  }
  const auto rgb = numpy.attr("frombuffer")(py::bytes(bytes), "dtype"_a = numpy.attr("uint8")).attr("reshape")(entry.height, entry.width, 3).attr("copy")();
  const auto image = image_type.attr("fromarray")(rgb, "RGB");
  const auto source = functional.attr("to_dtype")(functional.attr("to_image")(image), torch.attr("float32"), "scale"_a = true);
  const std::string name = entry.name;
  archive.add(name + "/rgb8", torch.attr("from_numpy")(rgb.attr("copy")()));
  archive.add(name + "/input", source);
  for (const bool antialias : {false, true}) {
   const auto value = functional.attr("resize")(source, py::list(py::make_tuple(entry.out_height, entry.out_width)), "interpolation"_a = interpolation.attr("BILINEAR"), "antialias"_a = antialias);
   archive.add(name + (antialias ? "/torch_antialias" : "/torch"), value);
  }
  const auto resized = resize(py::make_tuple(entry.out_height, entry.out_width))(image, py::none());
  const auto pil_tensor = transforms.attr("ToImage")()(index(resized, 0));
  const auto pil_float = transforms.attr("ToDtype")(torch.attr("float32"), "scale"_a = true)(pil_tensor);
  const auto normalized = normalize(py::list(py::make_tuple(.485, .456, .406)), py::list(py::make_tuple(.229, .224, .225)))(pil_float, index(resized, 1));
  archive.add(name + "/pil_rgb8", pil_tensor);
  archive.add(name + "/pil", pil_float);
  archive.add(name + "/pil_normalized", index(normalized, 0));
  case_records.push_back(Json::array({name, entry.height, entry.width, entry.out_height, entry.out_width}));
 }
 return {
  {"archive", archive.write()}, {"cases", case_records}, {"tensor_order", "RGB8 -> float32/255 -> bilinear align_corners=false (named antialias), no output quantization"},
  {"dataset_order", "RGB8 PIL -> upstream Resize bilinear antialias -> RGB8 quantization -> upstream ToImage/ToDtype(scale) -> Normalize"}
 };
}
Json keypoint_evidence(const fs::path& directory) {
 const auto torch = module("torch"), numpy = module("numpy");
 const auto detector = module("rfdetr.models.lwdetr").attr("LWDETR");
 const auto heads = module("rfdetr.models.heads.keypoints");
 Archive archive(directory, "keypoint-boundaries");
 Json cases = Json::array();
 // These are fixture inputs, not a parallel native model/task catalog.
 for (const auto& counts : std::vector<std::vector<int>>{{17, 0}, {0, 17}, {2, 0, 3}, {3, 2, 0}, {0, 0}}) {
  const auto id = std::to_string(cases.size());
  const auto schema = py::cast(counts);
  const int total = std::accumulate(counts.begin(), counts.end(), 0), maximum = *std::ranges::max_element(counts);
  const auto active = detector.attr("_create_kp_active_mask")(schema);
  archive.add(id + "/active", active);
  archive.add(id + "/class_mask", detector.attr("_create_keypoint_class_mask")(schema));
  const auto context = module("types").attr("SimpleNamespace")("use_grouppose_keypoints"_a = true, "num_keypoints_per_class"_a = schema, "_kp_active_mask"_a = active,
   "class_embed"_a = module("types").attr("SimpleNamespace")("out_features"_a = counts.size()), "_kp_zero_pad_warned"_a = false);
  const auto compact = torch.attr("arange")(2 * total * 8, "dtype"_a = torch.attr("float32")).attr("reshape")(1, 2, total, 8).attr("__truediv__")(100);
  const auto padded = detector.attr("_format_keypoint_output")(context, compact, 1, 2);
  archive.add(id + "/compact", compact);
  archive.add(id + "/padded", padded);
  archive.add(id + "/class_logits", detector.attr("_aggregate_keypoint_class_logits")(context, padded));
  cases.push_back({{"id", id}, {"counts", counts}});
  if (id == "2") {
   cases.back()["classes"] = Json::array({"tool", nullptr, "hand"});
   cases.back()["joint_names"] = Json::array({Json::array({"tip", "base"}), Json::array(), Json::array({"middle", "left", "right"})});
  } else if (id == "3") {
   cases.back()["classes"] = Json::array({"hand", "tool", nullptr});
   cases.back()["joint_names"] = Json::array({Json::array({"right", "middle", "left"}), Json::array({"base", "tip"}), Json::array()});
  }
  // The real pinned postprocessor owns stable tied-score selection, score
  // fusion, class-specific padded gathering and source-space box/joint output.
  const auto logits = torch.attr("zeros")(1, 2, counts.size());
  const auto boxes = torch.attr("tensor")(py::make_tuple(py::make_tuple(py::make_tuple(.1, .2, .4, .6), py::make_tuple(.9, .8, .4, .6))));
  const auto postprocessor = module("rfdetr.models.postprocess").attr("PostProcess")("num_select"_a = 2 * counts.size(), "num_keypoints_per_class"_a = schema);
  const auto selected =
   index(postprocessor(py::dict("pred_logits"_a = logits, "pred_boxes"_a = boxes, "pred_keypoints"_a = padded), torch.attr("tensor")(py::make_tuple(py::make_tuple(200, 400)))), 0);
  archive.add(id + "/selection/logits", logits);
  archive.add(id + "/selection/input_boxes", boxes);
  archive.add(id + "/selection/queries", index(postprocessor.attr("_select_topk")(logits), 2));
  for (const auto* field : {"scores", "labels", "boxes", "keypoints", "keypoint_precision_cholesky"}) archive.add(id + "/selection/" + field, selected[field]);
  const auto source_shapes = numpy.attr("tile")(numpy.attr("array")(py::make_tuple(200, 400)), py::make_tuple(2 * counts.size(), 1));
  const auto covariance = module("rfdetr.utilities.keypoints").attr("precision_cholesky_to_pixel_covariance")(object(selected["keypoint_precision_cholesky"]).attr("numpy")(), source_shapes);
  archive.add(id + "/selection/covariance", torch.attr("from_numpy")(covariance));
  for (const int n : {0, 2}) {
   const auto prefix = id + "/targets-" + std::to_string(n) + "/";
   const auto predictions = torch.attr("arange")(n * counts.size() * maximum * 8, "dtype"_a = torch.attr("float32"))
                             .attr("reshape")(n, counts.size() * maximum, 8)
                             .attr("__mod__")(23)
                             .attr("__truediv__")(100)
                             .attr("requires_grad_")(true);
   auto targets = torch.attr("arange")(n * maximum * 3, "dtype"_a = torch.attr("float32")).attr("reshape")(n, maximum, 3).attr("__mod__")(17).attr("__truediv__")(20);
   targets.attr("select")(-1, 2).attr("copy_")(torch.attr("arange")(n * maximum, "dtype"_a = torch.attr("float32")).attr("reshape")(n, maximum).attr("__mod__")(3));
   const auto labels = torch.attr("arange")(n, "dtype"_a = torch.attr("int64")).attr("__mod__")(counts.size());
   const auto areas = torch.attr("full")(py::make_tuple(n), .2);
   archive.add(prefix + "predictions", predictions);
   archive.add(prefix + "targets", targets);
   archive.add(prefix + "labels", labels);
   archive.add(prefix + "areas", areas);
   const auto losses = heads.attr("compute_l1_keypoint_loss")(predictions, targets, labels, areas, schema);
   const auto costs = heads.attr("compute_keypoint_matching_cost")(predictions.attr("unsqueeze")(0), targets, labels, areas, schema);
   py::object sum = predictions.attr("sum")().attr("__mul__")(0);
   for (int term = 0; term < 4; ++term) {
    archive.add(prefix + "loss/" + std::to_string(term), index(losses, term));
    archive.add(prefix + "cost/" + std::to_string(term), index(costs, term));
    sum = add(sum, index(losses, term).attr("sum")());
   }
   archive.add(prefix + "gradient", index(torch.attr("autograd").attr("grad")(sum, py::make_tuple(predictions)), 0));
  }
 }
 torch.attr("manual_seed")(731);
 const auto initializer = heads.attr("ConditionalQueryInitializer")(16, 5, 8);
 {
  PythonContext no_grad(torch.attr("no_grad")());
  for (py::handle pair : initializer.attr("named_parameters")()) {
   const auto parameter = index(pair, 1);
   // Nonzero modulation exercises every initializer branch and derivative.
   parameter.attr("copy_")(torch.attr("arange")(integer(parameter.attr("numel")()), "dtype"_a = torch.attr("float32"))
     .attr("reshape")(parameter.attr("shape"))
     .attr("__mod__")(29)
     .attr("__sub__")(14)
     .attr("__truediv__")(100));
   archive.add("initializer/parameter/" + string(index(pair, 0)), parameter);
  }
 }
 const auto condition = torch.attr("arange")(32, "dtype"_a = torch.attr("float32")).attr("reshape")(2, 16).attr("__truediv__")(30).attr("requires_grad_")(true);
 const auto initialized = initializer(condition);
 archive.add("initializer/input", condition);
 archive.add("initializer/output", initialized);
 initialized.attr("square")().attr("sum")().attr("backward")();
 archive.add("initializer/input_gradient", condition.attr("grad"));
 for (py::handle pair : initializer.attr("named_parameters")()) archive.add("initializer/gradient/" + string(index(pair, 0)), index(pair, 1).attr("grad"));
 // Use the evaluator imported by pinned RF-DETR, including its actual COCO
 // backend and sigma admission. Record visible/occluded/unannotated variants.
 const auto evaluation = module("rfdetr.evaluation.coco_eval");
 for (int visibility = 0; visibility < 3; ++visibility) {
  py::list gt_points, predicted_points, names;
  for (int joint = 0; joint < 3; ++joint) {
   for (double value : {20. + 3 * joint, 30. + joint, double(visibility)}) gt_points.append(value);
   for (double value : {21. + 3 * joint, 32. + joint, .8}) predicted_points.append(value);
   names.append("joint" + std::to_string(joint));
  }
  const auto gt = evaluation.attr("COCO")();
  gt.attr("dataset") = py::dict("info"_a = py::dict(), "images"_a = py::make_tuple(py::dict("id"_a = 1, "width"_a = 100, "height"_a = 100)),
   "categories"_a = py::make_tuple(py::dict("id"_a = 1, "name"_a = "person", "keypoints"_a = names)),
   "annotations"_a = py::make_tuple(py::dict("id"_a = 1, "image_id"_a = 1, "category_id"_a = 1, "bbox"_a = py::make_tuple(10, 10, 50, 60), "area"_a = 3000., "iscrowd"_a = 0, "keypoints"_a = gt_points,
    "num_keypoints"_a = (visibility ? 3 : 0))));
  gt.attr("createIndex")();
  const auto evaluator = evaluation.attr("CocoEvaluator")(gt, py::make_tuple("keypoints"), "keypoint_oks_sigmas"_a = py::make_tuple(.05, .08, .1), "log_summary"_a = false);
  py::list detections;
  detections.append(py::dict("image_id"_a = 1, "category_id"_a = 1, "keypoints"_a = predicted_points, "score"_a = .9));
  const auto dt = gt.attr("loadRes")(detections);
  const auto backend = evaluator.attr("coco_eval")["keypoints"];
  backend.attr("cocoDt") = dt;
  backend.attr("_prepare")();
  archive.add("oks/" + std::to_string(visibility), torch.attr("as_tensor")(backend.attr("computeOks")(1, 1), "dtype"_a = torch.attr("float64")));
 }
 const auto precision = numpy.attr("array")(py::make_tuple(py::make_tuple(py::make_tuple(.2, .3, -.4))), "dtype"_a = numpy.attr("float32"));
 const auto shape = numpy.attr("array")(py::make_tuple(py::make_tuple(200, 400)), "dtype"_a = numpy.attr("float32"));
 archive.add("covariance/input", torch.attr("from_numpy")(precision));
 archive.add("covariance/output", torch.attr("from_numpy")(module("rfdetr.utilities.keypoints").attr("precision_cholesky_to_pixel_covariance")(precision, shape)));
 return {{"archive", archive.write()}, {"cases", cases}};
}
struct Arguments {
 std::string payload_snapshot;
 bool check_environment = false;
 bool capabilities = false;
 std::string upstream;
 std::string plus_upstream;
 bool inventory_only = false;
 bool encoder_pretraining_only = false;
 std::string output;
 std::string weights;
 std::string fingerprint;
 std::string sdpa = "default";
 std::string selected_case;
 bool backward_operators = false;
 bool export_reference = false;
 bool help = false;
};
py::object checkpoint_value(py::handle checkpoint, py::handle arguments, const char* name) {
 const auto direct = object(checkpoint).attr("get")(name, py::none());
 if (!direct.is_none()) return direct;
 if (PyDict_Check(arguments.ptr())) return object(arguments).attr("get")(name, py::none());
 return module("builtins").attr("getattr")(arguments, name, py::none());
}
void acquire_inventory_asset(const fs::path& path, const std::string& url, const std::string& algorithm, const std::string& expected_digest, const std::uint64_t maximum_bytes,
 const std::optional<std::uint64_t> exact_bytes = std::nullopt) {
 const auto hash = [&](const fs::path& file) {
  const auto stream = module("builtins").attr("open")(file.string(), "rb");
  PythonContext close(stream);
  return string(module("hashlib").attr("file_digest")(stream, algorithm).attr("hexdigest")());
 };
 if (!fs::exists(path)) {
  const auto temporary = path.string() + ".inventory-partial";
  const auto response = module("requests").attr("get")(url, "stream"_a = true, "timeout"_a = 60);
  PythonContext close(response);
  response.attr("raise_for_status")();
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("cannot stage official asset: " + temporary);
  std::uint64_t bytes = 0;
  for (py::handle chunk : response.attr("iter_content")("chunk_size"_a = 1024 * 1024)) {
   check_signals();
   const auto data = py::cast<std::string>(chunk);
   bytes += data.size();
   if (bytes > maximum_bytes) throw std::runtime_error("official checkpoint exceeds inventory byte bound");
   output.write(data.data(), static_cast<std::streamsize>(data.size()));
   if (!output) throw std::runtime_error("official checkpoint write failed: " + temporary);
  }
  output.close();
  if (!output || (exact_bytes && bytes != *exact_bytes) || hash(temporary) != expected_digest) throw std::runtime_error("downloaded official checkpoint digest mismatch: " + path.string());
  fs::rename(temporary, path);
 }
 if (!fs::is_regular_file(path) || fs::file_size(path) > maximum_bytes || (exact_bytes && fs::file_size(path) != *exact_bytes) || hash(path) != expected_digest)
  throw std::runtime_error("cached official checkpoint digest mismatch: " + path.string());
}
void acquire_inventory_asset(const fs::path& path, py::handle asset) { acquire_inventory_asset(path, string(object(asset).attr("url")), "md5", string(object(asset).attr("md5_hash")), MaxArchive); }
void acquire_inventory_asset(const fs::path& path, const Json& asset) {
 const auto bytes = asset.at("file_bytes").get<std::uint64_t>();
 acquire_inventory_asset(path, asset.at("download_url"), "sha256", asset.at("sha256"), bytes, bytes);
}
std::string encoder_tensor_digest(py::handle tensor) {
 const auto data = object(tensor).attr("detach")().attr("cpu")().attr("contiguous")().attr("numpy")().attr("tobytes")();
 return string(module("hashlib").attr("sha256")(data).attr("hexdigest")());
}
Json encoder_tensor_record(py::handle tensor) {
 return {{"shape", sequence_json(object(tensor).attr("shape"))}, {"dtype", string(object(tensor).attr("dtype"))}, {"sha256", encoder_tensor_digest(tensor)}};
}
// Bounds are fixed before native comparison. The producer must independently
// satisfy them for FP32 versus FP64 matrix arithmetic and CPU versus CUDA math
// attention. The receipt remains specific to encoder initialization.
constexpr double EncoderPatchAtol = 3.e-5, EncoderPatchRtol = 3.e-4, EncoderFeatureAtol = 5.e-4, EncoderFeatureRtol = 5.e-4;
Json encoder_control(py::handle value, py::handle reference, const double atol, const double rtol) {
 const auto actual = object(value).attr("detach")().attr("cpu")().attr("float")();
 const auto expected = object(reference).attr("detach")().attr("cpu")().attr("float")();
 if (!truth(module("torch").attr("allclose")(actual, expected, "atol"_a = atol, "rtol"_a = rtol))) throw std::runtime_error("encoder independent numerical control exceeds the fixed FP32 bound");
 const auto error = actual.attr("sub")(expected).attr("abs")();
 return {{"max_abs", py::cast<double>(error.attr("max")().attr("item")())}, {"rms", py::cast<double>(error.attr("square")().attr("mean")().attr("sqrt")().attr("item")())}};
}
void install_source_paths(const fs::path& core, const fs::path& plus) {
 module("sys").attr("path").attr("insert")(0, (plus / "src").string());
 module("sys").attr("path").attr("insert")(0, (core / "src").string());
}
void generate_encoder_pretraining(const Arguments& args) {
 const fs::path source = args.upstream, plus_source = args.plus_upstream, output = args.output;
 const auto core_hashes = verify_source(source), plus_hashes = verify_source(plus_source, "plus");
 const auto environment = check_environment();
 const auto native = Json::parse(rfdetr_native_catalog_json());
 if (string(module("importlib.metadata").attr("version")("timm")) != source_pins().at("timm").get<std::string>()) throw std::runtime_error("encoder timm pin differs");
 install_source_paths(source, plus_source);
 const auto torch = module("torch"), safe = module("safetensors.torch"), patch_ops = module("timm.layers.patch_embed"), position_ops = module("timm.layers.pos_embed");
 const auto attention = module("torch.nn.attention");
 if (!truth(torch.attr("cuda").attr("is_available")())) throw std::runtime_error("encoder FP32 CUDA control unavailable");
 torch.attr("backends").attr("cuda").attr("matmul").attr("allow_tf32") = false;
 torch.attr("backends").attr("cudnn").attr("allow_tf32") = false;
 torch.attr("set_float32_matmul_precision")("highest");
 PythonContext no_grad(torch.attr("no_grad")());
 PythonContext math(attention.attr("sdpa_kernel")(attention.attr("SDPBackend").attr("MATH")));
 fs::create_directories(output / "assets");
 fs::create_directories(output / "references");
 if (fs::exists(output / "manifest.json")) throw std::runtime_error("refusing to overwrite published encoder evidence");
 Json manifest{
  {"format", "mmltk.rfdetr.encoder-pretraining.v1"}, {"mode", "encoder-pretraining"}, {"sources", source_pins()}, {"native_catalog", native},
  {"source_hashes", {{"core", core_hashes}, {"plus", plus_hashes}}}, {"environment", environment}, {"fingerprint", args.fingerprint}, {"producer_sha256", digest(support::executable_path())},
  {"dependency_sha256", digest(OracleRoot / "requirements.txt")}, {"source_manifest_sha256", digest(OracleRoot / "sources.json")},
  {"operator_sources", {{"patch_embed", digest(string(patch_ops.attr("__file__")))}, {"pos_embed", digest(string(position_ops.attr("__file__")))}}},
  {"execution_policy", {{"sdpa_backend", "math"}, {"matmul_tf32", truth(torch.attr("backends").attr("cuda").attr("matmul").attr("allow_tf32"))},
                        {"cudnn_tf32", truth(torch.attr("backends").attr("cudnn").attr("allow_tf32"))}, {"matmul_precision", string(torch.attr("get_float32_matmul_precision")())},
                        {"flash_sdp", truth(torch.attr("backends").attr("cuda").attr("flash_sdp_enabled")())}, {"math_sdp", truth(torch.attr("backends").attr("cuda").attr("math_sdp_enabled")())},
                        {"memory_efficient_sdp", truth(torch.attr("backends").attr("cuda").attr("mem_efficient_sdp_enabled")())},
                        {"cudnn_sdp", truth(torch.attr("backends").attr("cuda").attr("cudnn_sdp_enabled")())}, {"grad_enabled", truth(torch.attr("is_grad_enabled")())}}},
  {"precision_capability", precision_capability()}, {"assets", Json::array()}, {"cases", Json::array()},
  {"numerical_policy", {{"patch_atol", EncoderPatchAtol}, {"patch_rtol", EncoderPatchRtol}, {"feature_atol", EncoderFeatureAtol}, {"feature_rtol", EncoderFeatureRtol},
                        {"controls", "CPU FP32 vs FP64 pinv; independent upstream CPU vs CUDA FP32 math SDPA"}}},
  {"transformations", {{"patch", "timm.layers.patch_embed.resample_patch_embed"}, {"positions", "bicubic antialias align_corners=false; prefix preserved"}, {"intermediate_dtype", "torch.float32"},
                       {"pinv_atol", nullptr}, {"pinv_rtol", nullptr}, {"runtime_grid", "4*num_windows; stored grid follows native preset"}}}
 };
 for (const auto& asset : native.at("encoder_pretraining")) {
  const auto kind = asset.at("backbone").get<std::string>();
  const auto repository = asset.at("source_repository").get<std::string>(), revision = asset.at("source_revision").get<std::string>();
  auto cache_name = repository;
  std::ranges::replace(cache_name, '/', '-');
  const auto cached = fs::path(args.weights) / "encoders" / (cache_name + "-" + revision) / asset.at("filename").get<std::string>();
  fs::create_directories(cached.parent_path());
  acquire_inventory_asset(cached, asset);
  const auto evidence_asset = output / "assets" / (kind + ".safetensors");
  if (!fs::exists(evidence_asset)) fs::copy_file(cached, evidence_asset);
  acquire_inventory_asset(evidence_asset, asset);
  const auto original = safe.attr("load_file")(evidence_asset.string(), "device"_a = "cpu").cast<py::dict>();
  const bool pe = kind == "PeCoreTiny";
  const auto dino = module("rfdetr.models.backbone.dinov2");
  // The complete original model is loaded strictly, including PE's discarded
  // classifier. This verifies source coverage independently of native mapping.
  py::object base;
  if (pe)
   base = module("timm").attr("create_model")(module("rfdetr_plus.models.pe_core").attr("PE_CORE_T_TIMM_ARCH"), "pretrained"_a = false);
  else {
   const auto configuration = dino.attr("get_config")(kind == "DinoV2Small" ? "small" : "base", false);
   const auto config = invoke(module("transformers").attr("Dinov2Config"), py::tuple(), configuration);
   base = module("transformers").attr("Dinov2Model")(config);
  }
  base.attr("load_state_dict")(original, "strict"_a = true);
  base = py::none();
  auto asset_record = asset;
  asset_record["path"] = evidence_asset.lexically_relative(output).generic_string();
  asset_record["tensors"] = Json::object();
  for (const auto& entry : original) asset_record["tensors"][string(entry.first)] = encoder_tensor_record(entry.second);
  manifest["assets"].push_back(asset_record);
  for (const auto& row : native.at("presets")) {
   if (row.at("backbone").at("kind") != kind) continue;
   check_signals();
   const auto name = row.at("preset_name").get<std::string>();
   const auto config = Json::parse(rfdetr_native_configuration_json(name, 1));
   const auto patch = config.at("patch_size").get<int>(), grid = config.at("positional_encoding_size").get<int>(), windows = config.at("num_windows").get<int>();
   const int side = patch * windows * 4;
   py::object encoder;
   if (pe)
    encoder = module("rfdetr_plus.models.pe_core")
               .attr("PECoreEncoder")("out_feature_indexes"_a = config.at("backbone").at("features").get<std::vector<int>>(), "shape"_a = py::make_tuple(side, side), "patch_size"_a = patch,
                "num_windows"_a = windows, "positional_encoding_size"_a = grid, "load_pretrained_weights"_a = false);
   else
    encoder = dino.attr("DinoV2")("shape"_a = py::make_tuple(side, side), "out_feature_indexes"_a = config.at("backbone").at("features").get<std::vector<int>>(),
     "size"_a = (kind == "DinoV2Small" ? "small" : "base"), "use_registers"_a = false, "load_dinov2_weights"_a = false, "patch_size"_a = patch, "num_windows"_a = windows,
     "positional_encoding_size"_a = grid);
   encoder.attr("eval")();
   const std::string prefix = pe ? "model." : "encoder.", kernel = pe ? "patch_embed.proj.weight" : "embeddings.patch_embeddings.projection.weight",
                     positions = pe ? "pos_embed" : "embeddings.position_embeddings";
   py::dict admitted, references;
   Json exact = Json::object(), transformed = Json::array();
   for (const auto& target : encoder.attr("state_dict")().cast<py::dict>()) {
    const auto destination = string(target.first);
    if (!destination.starts_with(prefix)) throw std::runtime_error("unexpected independent encoder owner");
    const auto key = destination.substr(prefix.size());
    if (!original.contains(py::str(key))) throw std::runtime_error("official source misses independent encoder state: " + key);
    auto value = object(original[py::str(key)]);
    if (key == kernel) value = patch_ops.attr("resample_patch_embed")(value, py::make_tuple(patch, patch));
    if (key == positions) value = position_ops.attr("resample_abs_pos_embed")(value, py::make_tuple(grid, grid), "num_prefix_tokens"_a = 1, "interpolation"_a = "bicubic", "antialias"_a = true);
    admitted[target.first] = value;
    if (key == kernel || key == positions) { references[py::str("state/" + destination)] = value.attr("contiguous")().attr("clone")(); }
    const bool changed = (key == kernel && patch != asset.at("geometry").at("patch_size").get<int>()) || (key == positions && grid != asset.at("geometry").at("position_grid").get<int>());
    if (changed)
     transformed.push_back(destination);
    else
     exact[destination] = encoder_tensor_record(value);
   }
   encoder.attr("load_state_dict")(admitted, "strict"_a = true);
   const auto original_kernel = object(original[py::str(kernel)]);
   const auto old = asset.at("geometry").at("patch_size").get<int>();
   py::object double_control;
   if (old == patch)
    double_control = original_kernel.attr("clone")();
   else {
    const auto matrix = patch_ops.attr("_compute_resize_matrix")(py::make_tuple(old, old), py::make_tuple(patch, patch), "bicubic", true, torch.attr("device")("cpu"), torch.attr("float32"));
    const auto inverse = torch.attr("linalg").attr("pinv")(matrix.attr("double")());
    double_control = original_kernel.attr("flatten")(2).attr("double")().attr("matmul")(inverse).attr("reshape")(admitted[py::str(prefix + kernel)].attr("shape")).attr("float")();
   }
   Json controls{{"patch_fp64", encoder_control(admitted[py::str(prefix + kernel)], double_control, EncoderPatchAtol, EncoderPatchRtol)}, {"features_cuda", Json::array()}};
   references["control/patch_fp64"] = double_control.attr("contiguous")();
   const auto input =
    torch.attr("arange")(3 * side * side, "dtype"_a = torch.attr("float32")).attr("remainder")(251).attr("div")(125.0).attr("sub")(1.0).attr("reshape")(py::make_tuple(1, 3, side, side));
   references["input"] = input;
   const auto features = encoder(input);
   encoder.attr("cuda")();
   const auto cuda_features = encoder(input.attr("cuda")());
   if (py::len(features) != py::len(cuda_features)) throw std::runtime_error("encoder feature inventory differs across independent controls");
   for (py::ssize_t index_value = 0; index_value < py::len(features); ++index_value) {
    const auto cpu = index(features, index_value).attr("contiguous")(), gpu = index(cuda_features, index_value).attr("cpu")().attr("contiguous")();
    references[py::str("feature/" + std::to_string(index_value))] = cpu;
    references[py::str("control/cuda_feature/" + std::to_string(index_value))] = gpu;
    controls["features_cuda"].push_back(encoder_control(cpu, gpu, EncoderFeatureAtol, EncoderFeatureRtol));
   }
   const auto reference = output / "references" / (name + ".safetensors");
   publish_file(reference, [&](const auto& temporary) { safe.attr("save_file")(references, temporary.string()); });
   manifest["cases"].push_back(
    {{"preset", name}, {"backbone", kind}, {"source_sha256", asset.at("sha256")}, {"adaptation_revision", asset.at("adaptation_revision")}, {"configuration", config}, {"runtime_side", side},
     {"reference", file_record(reference)}, {"exact_state", exact}, {"transformed_state", transformed}, {"controls", controls}});
   std::cout << "encoder pretraining " << name << ": complete source state and FP32 controls" << std::endl;
  }
  if (digest(evidence_asset) != asset.at("sha256")) throw std::runtime_error("encoder asset changed during independent generation");
 }
 if (manifest["cases"].size() != native.at("presets").size() || verify_source(source) != core_hashes || verify_source(plus_source, "plus") != plus_hashes)
  throw std::runtime_error("encoder evidence coverage or source identity changed");
 manifest["status"] = "encoder_references_complete_native_comparison_pending";
 json_write(output / "manifest.json", manifest);
}
Json encoder_metadata(const Json& topology) {
 const auto encoder = topology.at("encoder").get<std::string>();
 if (encoder.starts_with("dinov2_windowed_")) {
  const auto size = encoder.substr(std::string_view("dinov2_windowed_").size());
  const auto config = to_json(module("rfdetr.models.backbone.dinov2").attr("get_config")(size, false));
  const auto dimension = config.at("hidden_size").get<std::uint32_t>();
  return {
   {"encoder", encoder}, {"dimension", dimension}, {"heads", config.at("num_attention_heads")}, {"layers", config.at("num_hidden_layers")},
   {"feedforward_dimension", dimension * config.at("mlp_ratio").get<std::uint32_t>()}, {"source", "pinned upstream get_config"}
  };
 }
 const auto pe = module("rfdetr_plus.models.pe_core");
 if (encoder != string(pe.attr("PE_CORE_T_ENCODER"))) throw std::runtime_error("unrecognized pinned upstream encoder: " + encoder);
 PythonContext meta(module("torch").attr("device")("meta"));
 const auto trunk = module("timm").attr("create_model")(pe.attr("PE_CORE_T_TIMM_ARCH"), "pretrained"_a = false, "patch_size"_a = topology.at("patch_size").get<int>(),
  "img_size"_a = topology.at("positional_encoding_size").get<int>() * topology.at("patch_size").get<int>());
 const auto blocks = trunk.attr("blocks");
 const auto first = index(blocks, 0);
 return {
  {"encoder", encoder}, {"dimension", integer(trunk.attr("embed_dim"))}, {"heads", integer(first.attr("attn").attr("num_heads"))}, {"layers", py::len(blocks)},
  {"feedforward_dimension", integer(first.attr("mlp").attr("fc1").attr("out_features"))}, {"source", string(pe.attr("PE_CORE_T_TIMM_ARCH")) + " metadata on meta device; no forward"}
 };
}
void inventory(const Arguments& args) {
 const fs::path source = args.upstream, plus_source = args.plus_upstream, output = args.output;
 const auto core_hashes = verify_source(source);
 const auto plus_hashes = verify_source(plus_source, "plus");
 const auto environment = check_environment();
 if (string(module("importlib.metadata").attr("version")("timm")) != source_pins().at("timm").get<std::string>()) throw std::runtime_error("timm source pin disagrees with installed dependency");
 install_source_paths(source, plus_source);
 const auto core = module("rfdetr"), plus = module("rfdetr_plus"), torch = module("torch");
 const auto core_assets = module("rfdetr.assets.model_weights").attr("ModelWeights");
 const auto plus_assets = module("rfdetr_plus.assets.model_weights").attr("ModelWeights");
 const auto native = Json::parse(rfdetr_native_catalog_json());
 Json manifest{
  {"format", "mmltk.rfdetr.inventory.v1"}, {"sources", source_pins()}, {"source_hashes", {{"core", core_hashes}, {"plus", plus_hashes}}}, {"environment", environment},
  {"fingerprint", args.fingerprint}, {"producer_sha256", digest(support::executable_path())}, {"dependency_sha256", digest(OracleRoot / "requirements.txt")},
  {"source_manifest_sha256", digest(OracleRoot / "sources.json")}, {"native_catalog", native}, {"assets", Json::array()}, {"mismatches", Json::array()}
 };
 fs::create_directories(output / "assets");
 const auto check = [&](bool condition, const std::string& message) {
  if (!condition) manifest["mismatches"].push_back(message);
 };
 std::size_t maximum_foregrounds = 0;
 for (const auto& row : native.at("presets")) {
  const auto name = row.at("preset_name").get<std::string>();
  const auto filename = row.at("canonical_weight_filename").get<std::string>();
  const bool platform = row.at("license") == "PlatformModel10";
  const auto registry = platform ? plus_assets : core_assets;
  const auto asset = registry.attr("from_filename")(filename);
  if (asset.is_none()) throw std::runtime_error("required native asset absent from pinned upstream registry: " + filename);
  check(string(asset.attr("url")) == row.at("canonical_weight_url").get<std::string>() && string(asset.attr("md5_hash")) == row.at("canonical_weight_md5").get<std::string>(),
   name + ": registry identity differs");
  const py::object model_type = (platform ? plus : core).attr(row.at("upstream_class").get<std::string>().c_str());
  const auto instance = model_type.attr("__new__")(model_type);
  const auto config = instance.attr("get_model_config")("device"_a = "cpu");
  const auto topology = to_json(config.attr("model_dump")("mode"_a = "json"));
  // This conversion is the external Python configuration format, not a second model registry.
  for (const auto& [native_field, external_field] : std::to_array<std::pair<const char*, const char*>>(
        {{"resolution", "resolution"}, {"patch_size", "patch_size"}, {"window_count", "num_windows"}, {"positional_encoding_size", "positional_encoding_size"}, {"decoder_layer_count", "dec_layers"},
         {"query_count", "num_queries"}, {"selected_query_count", "num_select"}, {"configured_class_count", "num_classes"}, {"hidden_dimension", "hidden_dim"}, {"group_count", "group_detr"},
         {"two_stage", "two_stage"}, {"self_attention_heads", "sa_nheads"}, {"cross_attention_heads", "ca_nheads"}, {"sampling_points", "dec_n_points"}, {"feedforward_dimension", "dim_feedforward"}}))
   check(row.at(native_field) == topology.at(external_field), name + ": topology differs: " + external_field);
  const auto encoder = encoder_metadata(topology);
  for (const auto* field : {"dimension", "heads", "layers", "feedforward_dimension"}) check(row.at("backbone").at(field) == encoder.at(field), name + ": encoder metadata differs: " + field);
  check(row.at("backbone").at("features") == topology.at("out_feature_indexes"), name + ": encoder feature indexes differ");
  for (const auto& [native_field, external_field] : KeypointTopologyFields) check(row.at(native_field) == topology.at(external_field), name + ": keypoint topology differs: " + external_field);
  const bool dual = row.at("projector") == "DualKeypointOnly";
  check(dual == topology.at("dual_projector").get<bool>() && dual == topology.at("dual_projector_kp_only").get<bool>(), name + ": projector differs");
  const auto training = to_json(instance.attr("get_train_config")("dataset_dir"_a = ".", "output_dir"_a = ".").attr("model_dump")("mode"_a = "json"));
  for (const auto& [native_field, external_field] : std::to_array<std::pair<const char*, const char*>>(
        {{"classification_loss_coefficient", "cls_loss_coef"}, {"keypoint_l1_loss_coefficient", "keypoint_l1_loss_coef"}, {"keypoint_findable_loss_coefficient", "keypoint_findable_loss_coef"},
         {"keypoint_visible_loss_coefficient", "keypoint_visible_loss_coef"}, {"keypoint_nll_loss_coefficient", "keypoint_nll_loss_coef"}}))
   check(row.at(native_field) == training.at(external_field), name + ": stock training coefficient differs: " + external_field);
  check((row.at("task") == "Segmentation") == topology.at("segmentation_head").get<bool>(), name + ": segmentation head differs");
  check((row.at("task") == "Keypoints") == topology.at("use_grouppose_keypoints").get<bool>(), name + ": keypoint head differs");
  const fs::path weights = fs::path(args.weights) / filename;
  std::cout << "inventory " << name << ": acquiring verified asset" << std::endl;
  acquire_inventory_asset(weights, asset);
  const auto sha256 = digest(weights);
  const auto checkpoint = torch.attr("load")(weights.string(), "map_location"_a = "cpu", "weights_only"_a = false);
  const auto state = checkpoint.attr("get")("model", py::none());
  if (!PyDict_Check(state.ptr())) throw std::runtime_error("official checkpoint has no model dictionary: " + name);
  Json tensors = Json::object();
  std::int64_t classifier_width = 0;
  for (const auto& pair : py::reinterpret_borrow<py::dict>(state)) {
   if (!PyUnicode_Check(pair.first.ptr()) || !py::isinstance(pair.second, torch.attr("Tensor"))) throw std::runtime_error("invalid official state entry: " + name);
   const auto tensor_name = string(pair.first);
   const auto tensor = object(pair.second);
   tensors[tensor_name] = {{"shape", to_json(tensor.attr("shape"))}, {"dtype", string(tensor.attr("dtype"))}};
   if (tensor_name == "class_embed.weight") classifier_width = integer(index(tensor.attr("shape"), 0));
  }
  if (classifier_width <= 0) throw std::runtime_error("missing official classifier evidence: " + name);
  const auto arguments = checkpoint.attr("get")("args", py::none());
  const auto raw_names = checkpoint_value(checkpoint, arguments, "class_names");
  const auto ns = module("types").attr("SimpleNamespace");
  const auto context = ns("model"_a = ns("class_names"_a = raw_names));
  const auto names = core.attr("RFDETR").attr("class_names").attr("fget")(context);
  Json class_names = to_json(names);
  std::string names_source = raw_names.is_none() ? "pinned RFDETR.class_names default" : "checkpoint class_names";
  if (raw_names.is_none() && topology.at("use_grouppose_keypoints").get<bool>()) {
   const auto declaration = "with `" + string(model_type.attr("__name__")) + "`, pretrained on COCO person keypoints.";
   if (support::read_text(source / "README.md").find(declaration) == std::string::npos) throw std::runtime_error("official keypoint asset lacks authoritative foreground meaning");
   class_names = Json::array({"person"});
   names_source = "pinned README stock COCO-person declaration; active mask establishes output slots";
  }
  const auto active = state.attr("get")("_kp_active_mask", py::none());
  Json active_mask = nullptr;
  if (!active.is_none()) active_mask = to_json(active.attr("tolist")());
  std::string layout = "Unresolved";
  Json foreground_slots = Json::array();
  if (class_names == to_json(module("rfdetr.assets.coco_classes").attr("COCO_CLASS_NAMES")) && classifier_width == 91 && row.at("task") != "Keypoints") {
   layout = "CocoSparse";
   for (const auto& pair : py::reinterpret_borrow<py::dict>(module("rfdetr.assets.coco_classes").attr("COCO_CLASSES"))) foreground_slots.push_back(integer(pair.first));
  } else if (class_names == Json::array({"person"}) && !active.is_none() && string(active.attr("dtype")) == "torch.bool" && integer(active.attr("dim")()) == 2) {
   const auto counts = to_json(active.attr("sum")(-1).attr("tolist")());
   for (std::size_t index = 0; index < counts.size(); ++index)
    if (counts[index].get<std::int64_t>() != 0) foreground_slots.push_back(index);
   if (foreground_slots == Json::array({0}) && counts.at(0) == 17) layout = "PersonActiveFirst";
   if (foreground_slots == Json::array({1}) && counts.at(1) == 17) layout = "PersonBackgroundFirst";
  }
  check(layout != "Unresolved", name + ": ambiguous checkpoint class meaning");
  check(layout == row.at("official_class_layout").get<std::string>(), name + ": native class layout differs: " + layout);
  check(classifier_width == row.at("class_count").get<std::int64_t>(), name + ": native classifier width differs: " + std::to_string(classifier_width));
  maximum_foregrounds = std::max(maximum_foregrounds, class_names.size());
  Json evidence{
   {"preset", name}, {"upstream_class", row.at("upstream_class")}, {"registry", platform ? "rfdetr_plus.assets.model_weights.ModelWeights" : "rfdetr.assets.model_weights.ModelWeights"},
   {"filename", filename}, {"url", string(asset.attr("url"))}, {"md5", string(asset.attr("md5_hash"))}, {"sha256", sha256}, {"bytes", fs::file_size(weights)}, {"configured_topology", topology},
   {"encoder_metadata", encoder}, {"training_defaults", training}, {"classifier_width", classifier_width}, {"class_names", class_names}, {"embedded_class_names", to_json(raw_names)},
   {"class_names_source", names_source}, {"class_layout", layout}, {"foreground_slots", foreground_slots}, {"active_keypoint_mask", active_mask}, {"tensors", tensors}
  };
  if (!arguments.is_none()) {
   const auto dictionary = PyDict_Check(arguments.ptr()) ? arguments : module("builtins").attr("vars")(arguments);
   evidence["checkpoint_arguments"] = Json::parse(string(module("json").attr("dumps")(dictionary, "default"_a = module("builtins").attr("str"))));
  }
  if (digest(weights) != sha256) throw std::runtime_error("official checkpoint changed during inventory: " + name);
  const auto evidence_path = output / "assets" / (name + ".json");
  json_write(evidence_path, evidence);
  manifest["assets"].push_back(
   {{"preset", name}, {"path", "assets/" + name + ".json"}, {"sha256", digest(evidence_path)}, {"checkpoint_sha256", sha256}, {"classifier_width", classifier_width},
    {"foreground_count", class_names.size()}, {"class_layout", layout}});
  std::cout << "inventory " << name << ": " << classifier_width << " slots, " << class_names.size() << " foreground names, " << layout << std::endl;
 }
 check(maximum_foregrounds <= native.at("foreground_capacity").get<std::size_t>(), "verified foreground meaning exceeds native catalog capacity");
 manifest["capacity_decision"] = {
  {"maximum_verified_foregrounds", maximum_foregrounds}, {"foreground_capacity", native.at("foreground_capacity")},
  {"instance_reference_bits", native.at("foreground_capacity").get<std::size_t>() <= 256 ? 8 : 16}, {"physical_output_slot_capacity", native.at("output_slot_capacity")}
 };
 if (verify_source(source) != core_hashes || verify_source(plus_source, "plus") != plus_hashes) throw std::runtime_error("source changed during inventory");
 manifest["status"] = manifest["mismatches"].empty() ? "verified_inventory_complete" : "native_catalog_mismatch";
 json_write(output / "manifest.json", manifest);
 if (!manifest["mismatches"].empty()) throw std::runtime_error("native catalog differs from verified inventory: " + manifest["mismatches"].dump());
}
Json checked_native_configuration(const Json& evidence, const int groups, py::handle instance, const py::dict& initial) {
 const auto name = evidence.at("preset").get<std::string>();
 const auto config = Json::parse(rfdetr_native_configuration_json(name, groups));
 const auto context = object(instance).attr("model"), model = context.attr("model");
 const auto topology = to_json(object(instance).attr("model_config").attr("model_dump")("mode"_a = "json"));
 const auto require = [&](const Json& actual, const Json& expected, const std::string_view field) {
  if (actual != expected) throw std::runtime_error("actual upstream configuration differs: " + name + ": " + std::string(field));
 };
 // Compare shared external scalars structurally. Python num_classes excludes
 // background; the canonical native field describes the physical classifier.
 for (const auto& [field, value] : config.items())
  if (field != "num_classes" && value.is_primitive() && topology.contains(field)) require(topology.at(field), value, field);
 require(evidence.at("classifier_width"), config.at("num_classes"), "official classifier width");
 require(integer(model.attr("class_embed").attr("out_features")), config.at("num_classes"), "classifier width");
 require(topology.at("num_classes").get<int>() + 1, config.at("num_classes"), "external class count plus background");
 require(evidence.at("configured_topology").at("resolution"), config.at("resolution"), "stock resolution");
 require(integer(context.attr("resolution")), config.at("resolution"), "actual resolution");
 require(integer(context.attr("postprocess").attr("num_select")), config.at("num_select"), "actual selection count");
 for (const auto* field : {"num_queries", "group_detr", "two_stage", "bbox_reparam", "lite_refpoint_refine"}) require(to_json(model.attr(field)), config.at(field), field);
 const auto transformer = model.attr("transformer");
 for (const auto* field : {"num_queries", "group_detr", "two_stage", "dec_layers"}) require(to_json(transformer.attr(field)), config.at(field), field);
 require(integer(transformer.attr("d_model")), config.at("hidden_dim"), "actual transformer width");
 const auto query_rows = config.at("num_queries").get<std::int64_t>() * groups;
 require(to_json(object(initial["query_feat.weight"]).attr("shape")), Json::array({query_rows, config.at("hidden_dim")}), "query embedding");
 require(to_json(object(initial["refpoint_embed.weight"]).attr("shape")), Json::array({query_rows, 4}), "reference embedding");
 require(topology.at("encoder"), evidence.at("configured_topology").at("encoder"), "encoder");
 require(topology.at("out_feature_indexes"), config.at("backbone").at("features"), "encoder feature indexes");
 for (const auto* field : {"dimension", "heads", "layers", "feedforward_dimension"}) require(evidence.at("encoder_metadata").at(field), config.at("backbone").at(field), field);
 const auto patch = config.at("backbone").at("kind") == "PeCoreTiny" ? "backbone.0.encoder.model.patch_embed.proj.weight" : "backbone.0.encoder.encoder.embeddings.patch_embeddings.projection.weight";
 require(to_json(object(initial[patch]).attr("shape")), Json::array({config.at("backbone").at("dimension"), 3, config.at("patch_size"), config.at("patch_size")}), "actual patch embedding");
 const bool keypoints = config.at("task") == "Keypoints", masks = config.at("task") == "Segmentation";
 require(truth(model.attr("use_grouppose_keypoints")), keypoints, "actual keypoint task");
 require(!model.attr("segmentation_head").is_none(), masks, "actual segmentation task");
 require(topology.at("use_grouppose_keypoints"), keypoints, "configured keypoint task");
 require(topology.at("segmentation_head"), masks, "configured segmentation task");
 for (const auto& [native_field, external_field] : KeypointTopologyFields) require(topology.at(external_field), config.at(native_field), external_field);
 const bool dual = config.at("projector") == "DualKeypointOnly";
 require(topology.at("dual_projector"), dual, "dual projector");
 require(topology.at("dual_projector_kp_only"), dual, "keypoint-only projector");
 require(to_json(model.attr("num_keypoints_per_class")), config.at("keypoints_per_class"), "actual physical joint counts");
 require(topology.at("num_keypoints_per_class"), config.at("keypoints_per_class"), "configured physical joint counts");
 const auto active = model.attr("_kp_active_mask");
 if (keypoints) {
  require(string(active.attr("dtype")), "torch.bool", "active-mask dtype");
  require(to_json(active.attr("sum")(-1).attr("tolist")()), config.at("keypoints_per_class"), "active-mask joint counts");
  require(to_json(active.attr("tolist")()), evidence.at("active_keypoint_mask"), "official active mask");
 } else
  require(integer(active.attr("numel")()), 0, "non-keypoint active mask");
 return config;
}
void generate(const Arguments& args) {
 const char* trace_value = std::getenv("MMLTK_RFDETR_ORACLE_TRACE");
 const bool tracing = trace_value && std::string_view(trace_value) == "1";
 const auto trace = [tracing](std::string_view stage, std::string_view preset = {}) {
  if (!tracing) return;
  std::cerr << Json{{"event", "oracle.stage"}, {"stage", stage}, {"preset", preset}}.dump() << std::endl;
 };
 trace("generate.begin");
 const auto torch = module("torch");
 const auto source = fs::weakly_canonical(fs::absolute(args.upstream));
 const auto source_hashes = verify_source(source);
 const fs::path plus_source = args.plus_upstream;
 const auto plus_hashes = verify_source(plus_source, "plus");
 const auto environment = check_environment();
 install_source_paths(source, plus_source);
 const auto core = module("rfdetr"), plus = module("rfdetr_plus");
 const auto defaults_type = module("rfdetr.models._defaults").attr("ModelDefaults");
 const py::object get_param_dict = module("rfdetr.training.param_groups").attr("get_param_dict");
 (void)get_param_dict;  // Import/attribute validation is intentionally eager.
 const py::object cuda = torch.attr("cuda");
 if (!truth(cuda.attr("is_available")())) throw std::runtime_error("upstream oracle requires CUDA; no hardware skip counts as evidence");
 const py::object cuda_backend = torch.attr("backends").attr("cuda");
 const py::object cudnn = torch.attr("backends").attr("cudnn");
 cuda_backend.attr("matmul").attr("allow_tf32") = false;
 cudnn.attr("allow_tf32") = false;
 torch.attr("set_float32_matmul_precision")("highest");
 cudnn.attr("benchmark") = false;
 if (args.sdpa == "math") {
  cuda_backend.attr("enable_flash_sdp")(false);
  cuda_backend.attr("enable_mem_efficient_sdp")(false);
  cuda_backend.attr("enable_cudnn_sdp")(false);
  cuda_backend.attr("enable_math_sdp")(true);
 }
 const fs::path output = args.output;
 fs::create_directories(output);
 if (fs::exists(output / "manifest.json")) throw std::runtime_error("refusing to overwrite published oracle evidence");
 // Completed shards survive interrupted preparation. A manifest is published
 // only after the complete matrix and both clean sources have been verified.
 const fs::path directory = output / "references";
 fs::create_directories(directory);
 Arguments inventory_args = args;
 inventory_args.output = (output / "inventory").string();
 if (!fs::exists(fs::path(inventory_args.output) / "manifest.json")) inventory(inventory_args);
 const auto admitted_inventory = read_json(fs::path(inventory_args.output) / "manifest.json");
 if (admitted_inventory.at("sources") != source_pins() || admitted_inventory.at("fingerprint") != args.fingerprint || admitted_inventory.at("status") != "verified_inventory_complete" ||
     admitted_inventory.at("native_catalog") != Json::parse(rfdetr_native_catalog_json()))
  throw std::runtime_error("cached inventory provenance differs");
 trace("inventory.admitted");
 const py::object build_config = torch.attr("__config__").attr("show")();
 const auto build_config_size = py::len(build_config);
 const auto build_config_prefix = item(build_config, py::slice(py::none(), py::int_(65536), py::none()));
 Json policy = {
  {"sdpa_backend", args.sdpa}, {"matmul_tf32", truth(cuda_backend.attr("matmul").attr("allow_tf32"))}, {"cudnn_tf32", truth(cudnn.attr("allow_tf32"))},
  {"matmul_precision", string(torch.attr("get_float32_matmul_precision")())}, {"cudnn_benchmark", truth(cudnn.attr("benchmark"))}, {"flash_sdp", truth(cuda_backend.attr("flash_sdp_enabled")())},
  {"math_sdp", truth(cuda_backend.attr("math_sdp_enabled")())}, {"memory_efficient_sdp", truth(cuda_backend.attr("mem_efficient_sdp_enabled")())},
  {"cudnn_sdp", truth(cuda_backend.attr("cudnn_sdp_enabled")())}, {"attention", "upstream grid_sample deformable attention; native production CUDA"},
  {"matcher",
   "FP32 cases use untouched stock; AMP main uses fp32_matcher_costs: original HungarianMatcher._focal_classification_cost, matcher.box_cxcywh_to_xyxy, matcher.batch_sigmoid_ce_loss, "
   "matcher.batch_dice_loss and matcher-origin torch.cdist receive FP32 operands with autocast disabled; original mask point_sample/einsum and matcher forward/class/batched eligibility are "
   "unchanged; stock prefix is untouched AMP"},
  {"gradient_scale", 1}, {"accumulated_microbatches", 1}
 };
 Json manifest = {
  {"format", Format}, {"revision", core_revision()}, {"source_hashes", source_hashes}, {"producer_sha256", digest(support::executable_path())},
  {"dependency_sha256", digest(OracleRoot / "requirements.txt")}, {"payload_sha256", digest(OracleRoot / "nvidia-payload.json")}, {"environment", environment}, {"fingerprint", args.fingerprint},
  {"cuda_device", string(cuda.attr("get_device_name")())}, {"torch_build_config", string(build_config_prefix)}, {"torch_build_config_truncated", build_config_size > 65536}, {"policy", policy},
  {"presets", Json::array()}, {"cases", Json::array()}, {"unsupported", Json::array()}, {"sources", source_pins()}, {"plus_source_hashes", plus_hashes},
  {"inventory_sha256", digest(fs::path(inventory_args.output) / "manifest.json")},
  {"preprocessing", "RGB unit float32 -> upstream RFDETR.means/stds; deterministic source pixels and exact normalized input retained"},
  {"tensor_families",
   Json::array(
    {"backbone", "grouppose", "forward-operator", "output", "vjp", "assignment", "loss", "gradient", "clipped", "updated", "optimizer", "held", "schema", "selection", "oks", "export-math"})},
  {"export_policy", {{"device", "cuda"}, {"sdpa_backend", "math"}, {"prefix", "export-math/"}}}
 };
 trace("precision.inspect");
 const auto capability = precision_capability();
 trace("precision.ready");
 manifest["precision_capability"] = capability;
 if (!args.selected_case.empty()) manifest["selected_case"] = args.selected_case;
 manifest["backward_operators"] = args.backward_operators;
 manifest["export_reference"] = args.export_reference;
 if (args.export_reference) {
  module("rfdetr.export._onnx.exporter").attr("OnnxExporter").attr("check_dependencies")();
  for (const auto* family : {"export", "export-cuda", "upstream-export"}) manifest["tensor_families"].push_back(family);
 }
 if (args.backward_operators)
  for (const auto* family : {"backward-operator", "decoder-forward-operator", "decoder-backward-operator"}) manifest["tensor_families"].push_back(family);
 std::vector<std::string> precisions{"fp32", "fp16"};
 if (capability.at("bf16").get<bool>())
  precisions.emplace_back("bf16");
 else
  manifest["unsupported"].push_back("bf16: CUDA device does not support BF16");
 trace("defaults.construct");
 const auto defaults = defaults_type("force_no_pretrain"_a = true);
 trace("defaults.ready");
 for (const auto& record : admitted_inventory.at("assets")) {
  const auto evidence_path = output / "inventory" / record.at("path").get<std::string>();
  if (digest(evidence_path) != record.at("sha256").get<std::string>()) throw std::runtime_error("cached asset evidence changed");
  const auto evidence = read_json(evidence_path);
  const auto name = evidence.at("preset").get<std::string>(), filename = evidence.at("filename").get<std::string>();
  if (!args.selected_case.empty() && !args.selected_case.starts_with(name + "-")) continue;
  const auto weights = fs::path(args.weights) / filename;
  if (digest(weights) != evidence.at("sha256").get<std::string>()) throw std::runtime_error("official asset changed: " + name);
  const auto original = admitted_file(directory, "original-" + filename);
  if (!fs::exists(original)) publish_file(original, [&](const auto& temporary) { fs::copy_file(weights, temporary); });
  if (digest(original) != evidence.at("sha256").get<std::string>()) throw std::runtime_error("retained official asset changed: " + name);
  const bool platform = evidence.at("registry").get<std::string>().starts_with("rfdetr_plus.");
  trace("model.class_accessor", name);
  const py::object model_type = (platform ? plus : core).attr(evidence.at("upstream_class").get<std::string>().c_str());
  trace("model.class_ready", name);
  for (const std::string protocol : {"controlled-group2", "stock"}) {
   const bool stock = protocol == "stock";
   const int groups = stock ? evidence.at("configured_topology").at("group_detr").get<int>() : 2;
   torch.attr("manual_seed")(731);
   trace("model.construct", name);
   const auto instance = model_type("pretrain_weights"_a = weights.string(), "group_detr"_a = groups, "device"_a = "cuda");
   trace("model.constructed", name);
   const auto mc = instance.attr("model_config"), model = instance.attr("model").attr("model");
   const auto tc =
    instance.attr("get_train_config")("dataset_dir"_a = ".", "output_dir"_a = ".", "lr"_a = 1e-4, "lr_encoder"_a = 1e-4, "grad_accum_steps"_a = 1, "clip_max_norm"_a = .1, "weight_decay"_a = 1e-4);
   if (integer(mc.attr("resolution")) != evidence.at("configured_topology").at("resolution") || integer(mc.attr("group_detr")) != groups ||
       integer(model.attr("class_embed").attr("out_features")) != evidence.at("classifier_width"))
    throw std::runtime_error("actual upstream topology differs: " + name);
   py::dict initial;
   for (const auto& pair : model.attr("state_dict")().cast<py::dict>()) initial[pair.first] = object(pair.second).attr("detach")().attr("cpu")().attr("clone")();
   const auto configuration = checked_native_configuration(evidence, groups, instance, initial);
   const auto checkpoint = stock ? original : admitted_file(directory, name + ".pth");
   if (!stock) {
    const auto make_namespace = module("rfdetr._namespace").attr("_namespace_from_configs");
    const auto arguments = make_namespace(mc, tc, defaults);
    const auto namespace_json = [](
                                 py::handle value) { return Json::parse(string(module("json").attr("dumps")(module("builtins").attr("vars")(value), "default"_a = module("builtins").attr("str")))); };
    // Decode the reflected object into Python once; a JSON string here would
    // violate the production bridge's canonical-object contract.
    const auto native = module("json").attr("loads")(configuration.dump());
    if (!PyDict_Check(native.ptr())) throw std::runtime_error("native configuration projection is not an object");
    if (!fs::exists(checkpoint))
     publish_file(checkpoint, [&](const auto& temporary) { torch.attr("save")(py::dict("model"_a = initial, "args"_a = arguments, "native_configuration"_a = native), temporary.string()); });
    const auto retained_checkpoint = torch.attr("load")(checkpoint.string(), "map_location"_a = "cpu", "weights_only"_a = false);
    if (to_json(retained_checkpoint.attr("get")("native_configuration", py::none())) != configuration || namespace_json(retained_checkpoint["args"]) != namespace_json(arguments))
     throw std::runtime_error("cached initial configuration changed: " + name);
    const auto retained = retained_checkpoint["model"].cast<py::dict>();
    if (py::len(retained) != py::len(initial)) throw std::runtime_error("cached initial inventory changed: " + name);
    for (const auto& pair : initial)
     if (!retained.contains(pair.first) || !py::isinstance(retained[pair.first], torch.attr("Tensor")) || !object(pair.second).attr("dtype").is(object(retained[pair.first]).attr("dtype")) ||
         to_json(object(pair.second).attr("shape")) != to_json(object(retained[pair.first]).attr("shape")) ||
         to_json(object(pair.second).attr("stride")()) != to_json(object(retained[pair.first]).attr("stride")()) || !truth(torch.attr("equal")(pair.second, retained[pair.first])))
      throw std::runtime_error("cached initial tensor changed: " + name);
    manifest["presets"].push_back(
     {{"name", name}, {"checkpoint", checkpoint.filename().string()}, {"sha256", digest(checkpoint)}, {"original", original.filename().string()}, {"original_sha256", evidence.at("sha256")},
      {"verified_asset", evidence}, {"native_configuration", configuration}, {"model_config", to_json(mc.attr("model_dump")("mode"_a = "json"))},
      {"train_config", to_json(tc.attr("model_dump")("mode"_a = "json"))}, {"constructor", constructor_evidence(module("rfdetr.models.lwdetr").attr("build_model_from_config")(mc, tc, defaults))}});
   }
   const auto checkpoint_hash = digest(checkpoint);
   // RFDETR's public constructor deliberately keeps its underlying model on
   // CPU. Direct oracle forwards require this explicit real-model placement.
   model.attr("cuda")();
   for (py::handle parameter : model.attr("parameters")())
    if (!truth(object(parameter).attr("is_cuda"))) throw std::runtime_error("upstream parameter was not placed on CUDA");
   for (const auto& precision : precisions)
    for (const bool training : {false, true})
     for (const int batch : {1, 2}) {
      if (stock && (precision != "fp32" || !training || batch != 1)) continue;
      const int steps = !stock && training && batch == 2 ? 8 : 1;
      const auto id = name + "-" + protocol + "-" + precision + "-b" + std::to_string(batch) + (training ? "-train" : "-eval");
      if (!args.selected_case.empty() && id != args.selected_case) continue;
      const auto completed = admitted_file(directory, id), staging = admitted_file(directory, id + ".partial");
      Json entry;
      if (fs::exists(completed)) {
       const auto completion = read_json(admitted_file(completed, "case.json"));
       entry = authenticated_json(completed, completion.at("index"));
       if (entry.at("producer_sha256") != manifest.at("producer_sha256") || entry.at("fingerprint") != args.fingerprint || entry.at("checkpoint_sha256") != checkpoint_hash || entry.at("id") != id ||
           entry.at("protocol") != protocol || entry.at("groups") != groups || entry.at("steps").size() != steps)
        throw std::runtime_error("completed numerical case provenance changed: " + id);
       for (const auto& step : entry.at("steps")) verify_archive(completed, step);
       if (args.export_reference) {
        const auto& artifact = entry.at("upstream_export");
        const auto [file, bytes] = admitted_file_record(completed, artifact);
        if (!bytes || bytes > MaxArchive || fs::file_size(file) != bytes || digest(file) != artifact.at("sha256").get<std::string>())
         throw std::runtime_error("completed upstream ONNX export changed");
       }
      } else {
       fs::remove_all(staging);
       fs::create_directory(staging);
       entry = run_case(staging, model, initial, mc, tc, defaults, name, precision, batch, training, steps, protocol, args.backward_operators, args.export_reference);
       entry["producer_sha256"] = manifest.at("producer_sha256");
       entry["fingerprint"] = args.fingerprint;
       entry["checkpoint_sha256"] = checkpoint_hash;
       json_write(staging / "index.json", entry);
       json_write(staging / "case.json", {{"index", file_record(staging / "index.json")}});
       fs::rename(staging, completed);
      }
      auto reference = file_record(completed / "index.json");
      reference["directory"] = id;
      manifest["cases"].push_back(std::move(reference));
     }
  }
 }
 for (const auto* name : {"resize", "keypoints"}) {
  const auto completed = admitted_file(directory, name), staging = admitted_file(directory, std::string(name) + ".partial");
  Json evidence;
  if (fs::exists(completed)) {
   const auto completion = read_json(admitted_file(completed, "complete.json"));
   evidence = authenticated_json(completed, completion.at("index"));
   verify_archive(completed, evidence.at("archive"));
  } else {
   fs::remove_all(staging);
   fs::create_directory(staging);
   evidence = std::string_view(name) == "resize" ? resize_evidence(staging) : keypoint_evidence(staging);
   json_write(staging / "index.json", evidence);
   json_write(staging / "complete.json", {{"index", file_record(staging / "index.json")}});
   fs::rename(staging, completed);
  }
  auto reference = file_record(completed / "index.json");
  reference["directory"] = name;
  manifest[name] = std::move(reference);
 }
 if (source_hashes != verify_source(source) || plus_hashes != verify_source(plus_source, "plus")) throw std::runtime_error("upstream source changed during production");
 if (!args.selected_case.empty() && (manifest["cases"].size() != 1 || manifest["cases"].front().at("directory") != args.selected_case || manifest["presets"].size() != 1))
  throw std::runtime_error("requested case is not in the supported upstream matrix: " + args.selected_case);
 manifest["status"] = args.selected_case.empty() ? "references_complete_native_comparison_pending" : "selected_reference_native_comparison_pending";
 manifest["directory"] = "references";
 json_write(output / "manifest.json", manifest);
}
void usage(std::ostream& stream, const char* program) {
 stream << "usage: " << program
        << " [-h] [--payload-snapshot PATH] [--check-environment]\n"
           "       [--upstream PATH] [--output PATH] [--weights PATH]\n"
           "       [--fingerprint VALUE] [--sdpa {default,math}]\n\n"
           "Bounded independent RF-DETR evidence. Execute only through ./mmltk.\n"
           "Imports the read-only pinned upstream source and produces ordinary\n"
           "tensor archives. Stock AMP and the controlled FP32 matcher are\n"
           "separate executions, not Match-Free or dataset AP evidence.\n\n"
           "  -h, --help             show this help message and exit\n"
           "  --payload-snapshot PATH\n"
           "                         atomically record donor package versions\n"
           "  --check-environment    validate pinned payload and dependency closure\n"
           "  --capabilities         report actual required CUDA precision support\n"
           "  --upstream PATH        clean checkout of the pinned revision\n"
           "  --output PATH          fresh reference output directory\n"
           "  --weights PATH         official cached pretrained artifacts\n"
           "  --fingerprint VALUE    wrapper-provided image fingerprint\n"
           "  --sdpa {default,math}  upstream SDPA policy (default: default)\n";
 stream << "  --case ID              produce exactly one named case, not full-matrix evidence\n";
 stream << "  --encoder-pretraining-only  prepare only official base encoders and FP32 initialization controls\n";
 stream << "  --backward-operators   include intermediate gradients for the selected case\n";
 stream << "  --export-reference    include CPU/CUDA controls and an actual upstream ONNX export for one FP32 batch-one case\n";
}
bool optional_token(std::string_view value) {
 if (value.size() < 2 || value.front() != '-' || value.find(' ') != std::string_view::npos) return false;
 // argparse accepts negative integer/decimal strings as ordinary values.
 std::size_t position = 1;
 while (position < value.size() && value[position] >= '0' && value[position] <= '9') ++position;
 if (position == value.size() && position > 1) return false;
 if (position < value.size() && value[position] == '.') {
  const auto decimal = ++position;
  while (position < value.size() && value[position] >= '0' && value[position] <= '9') ++position;
  if (position == value.size() && position > decimal) return false;
 }
 return true;
}
Arguments parse_arguments(int argc, char** argv) {
 Arguments result;
 const std::array<std::string_view, 16> options{
  "--capabilities", "--payload-snapshot", "--check-environment", "--upstream", "--plus-upstream", "--inventory-only", "--output", "--weights", "--fingerprint", "--sdpa", "--case",
  "--backward-operators", "--export-reference", "--encoder-pretraining-only", "--help", "-h"
 };
 for (int i = 1; i < argc; ++i) {
  const std::string argument = argv[i];
  const auto equals = argument.find('=');
  auto option = argument.substr(0, equals);
  if (option == "--" && equals == std::string::npos) {
   if (i + 1 != argc) throw support::ToolError("unrecognized arguments: " + std::string(argv[i + 1]), 2);
   break;
  }
  if (!std::any_of(options.begin(), options.end(), [&](std::string_view known) { return known == option; })) {
   // argparse's default long-option abbreviation behavior.
   std::vector<std::string_view> matches;
   if (option.starts_with("--") && option.size() > 2)
    for (const auto known : options)
     if (known.starts_with(option)) matches.push_back(known);
   if (matches.size() > 1) throw support::ToolError("ambiguous option: " + option, 2);
   if (matches.empty()) throw support::ToolError("unrecognized arguments: " + argument, 2);
   option = matches.front();
  }
  if (option == "--help" || option == "-h") {
   if (equals != std::string::npos) throw support::ToolError("argument " + option + ": ignored explicit argument", 2);
   result.help = true;
   return result;
  }
  if (option == "--check-environment" || option == "--inventory-only" || option == "--encoder-pretraining-only" || option == "--capabilities" || option == "--backward-operators" ||
      option == "--export-reference") {
   if (equals != std::string::npos) throw support::ToolError("argument " + option + ": ignored explicit argument", 2);
   (option == "--inventory-only"             ? result.inventory_only
    : option == "--encoder-pretraining-only" ? result.encoder_pretraining_only
     : option == "--capabilities"            ? result.capabilities
      : option == "--backward-operators"     ? result.backward_operators
       : option == "--export-reference"      ? result.export_reference
                                             : result.check_environment) = true;
   continue;
  }
  std::string value;
  if (equals != std::string::npos)
   value = argument.substr(equals + 1);
  else {
   if (++i == argc || optional_token(argv[i])) throw support::ToolError("argument " + option + ": expected one argument", 2);
   value = argv[i];
  }
  if (option == "--payload-snapshot")
   result.payload_snapshot = value;
  else if (option == "--upstream")
   result.upstream = value;
  else if (option == "--plus-upstream")
   result.plus_upstream = value;
  else if (option == "--output")
   result.output = value;
  else if (option == "--weights")
   result.weights = value;
  else if (option == "--fingerprint")
   result.fingerprint = value;
  else if (option == "--case")
   result.selected_case = value;
  else if (option == "--sdpa") {
   if (value != "default" && value != "math") throw support::ToolError("argument --sdpa: invalid choice: '" + value + "' (choose from 'default', 'math')", 2);
   result.sdpa = value;
  }
 }
 if (result.payload_snapshot.empty() && !result.check_environment && !result.capabilities &&
     (result.upstream.empty() || result.plus_upstream.empty() || result.output.empty() || result.weights.empty() || result.fingerprint.empty()))
  throw support::ToolError("generation requires --upstream, --plus-upstream, --output, --weights and --fingerprint", 2);
 if (result.export_reference &&
     (result.inventory_only || !result.selected_case.contains("-controlled-group2-fp32-b1-") || !(result.selected_case.ends_with("-train") || result.selected_case.ends_with("-eval"))))
  throw support::ToolError("--export-reference requires one controlled FP32 batch-one --case", 2);
 if (result.encoder_pretraining_only && (result.inventory_only || !result.selected_case.empty() || result.backward_operators || result.export_reference))
  throw support::ToolError("--encoder-pretraining-only excludes inventory-only, case, backward-operators and export-reference", 2);
 return result;
}
int execute(const Arguments& args) {
 // Honor the donor interpreter's environment/site configuration without
 // inserting the launch directory ahead of the pinned dependency overlay.
 py::scoped_interpreter interpreter{true, 0, nullptr, false};
 try {
  if (!args.payload_snapshot.empty())
   json_write(args.payload_snapshot, payload());
  else if (args.check_environment)
   check_environment();
  else if (args.capabilities)
   std::cout << precision_capability().dump() << std::endl;
  else if (args.encoder_pretraining_only)
   generate_encoder_pretraining(args);
  else if (args.inventory_only)
   inventory(args);
  else
   generate(args);
  return 0;
 } catch (py::error_already_set& error) {
  if (error.matches(PyExc_SystemExit)) {
   const py::object code = error.value().attr("code");
   if (code.is_none()) return 0;
   if (PyLong_Check(code.ptr())) return py::cast<int>(code);
   std::cerr << string(code) << '\n';
   return 1;
  }
  // Keep both the concise fatal notice and genuine upstream traceback.
  // The interpreter must remain alive until the exception is destroyed.
  std::cerr << "RF-DETR upstream oracle failed: " << string(error.type().attr("__name__")) << ": " << string(error.value()) << '\n';
  const bool interrupted = error.matches(PyExc_KeyboardInterrupt);
  error.restore();
  PyErr_PrintEx(0);
  return interrupted ? 130 : 1;
 }
}
}  // namespace
export int mmltk_tool_generate_rfdetr_upstream_oracle(int argc, char** argv) {
 try {
  const auto args = parse_arguments(argc, argv);
  if (args.help) {
   usage(std::cout, argv[0]);
   return 0;
  }
  return execute(args);
 } catch (const support::ToolError& error) {
  if (error.status == 2) {
   usage(std::cerr, argv[0]);
   std::cerr << argv[0] << ": error: " << error.what() << '\n';
  } else
   std::cerr << "RF-DETR upstream oracle failed: RuntimeError: " << error.what() << '\n';
  return error.status;
 } catch (const std::exception& error) {
  std::cerr << "RF-DETR upstream oracle failed: RuntimeError: " << error.what() << '\n';
  return 1;
 }
}
