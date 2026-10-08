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
// The manifest and tensor archive contracts are otherwise the same v1 format.

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <span>
#include <stdexcept>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

export module mmltk.tools.generate_rfdetr_upstream_oracle;
import mmltk.tools.cpp_digest;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;

namespace {

namespace py = pybind11;
namespace fs = std::filesystem;
namespace support = mmltk::tools;
using Json = nlohmann::json; // Sorted object keys, as json.dumps(sort_keys=True).
using namespace pybind11::literals;

constexpr std::string_view Revision = "e9a138f70cc14e3fddca029a0683055e34ab8f35";
constexpr std::string_view Format = "mmltk.rfdetr.upstream-oracle.v1";
const fs::path OracleRoot = "/opt/rfdetr-oracle";
constexpr std::uint64_t MaxManifest = 32ULL * 1024 * 1024;
constexpr std::uint64_t MaxArchive = 4ULL * 1024 * 1024 * 1024;

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

py::object invoke(py::handle callable, py::handle args, py::handle kwargs) {
    return owned_result(PyObject_Call(callable.ptr(), args.ptr(), kwargs.is_none() ? nullptr : kwargs.ptr()));
}

py::object item(py::handle value, py::handle index) {
    return owned_result(PyObject_GetItem(value.ptr(), index.ptr()));
}

py::object index(py::handle value, py::ssize_t offset) { return item(value, py::int_(offset)); }

py::object add(py::handle left, py::handle right) {
    return owned_result(PyNumber_Add(left.ptr(), right.ptr()));
}

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

Json read_json(const fs::path& path) { return Json::parse(support::read_text(path)); }

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
        if (node.is_number_float() && !std::isfinite(node.get<double>()))
            throw std::runtime_error("out of range float values are not JSON compliant");
        if (node.is_structured()) for (const auto& child : node) self(self, child);
    };
    finite(finite, value);
    const auto data = value.dump(2, ' ', true);
    if (data.size() > MaxManifest) throw std::runtime_error("oracle manifest exceeds its declared bound");
    const fs::path temporary = path.string() + ".partial";
    support::FileDescriptor file(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666));
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
    return {{"distributions", distributions}, {"torch", to_json(torch.attr("__version__"))},
            {"torchvision", to_json(torchvision.attr("__version__"))},
            {"cuda", to_json(torch.attr("version").attr("cuda"))}};
}

Json check_environment() {
    auto current = payload();
    const auto expected = read_json(OracleRoot / "payload.json");
    if (current != expected)
        throw std::runtime_error("authoritative NGC payload replaced: " + current.dump() + " != " + expected.dump());
    const auto manifest = read_json(OracleRoot / "nvidia-payload.json");
    if (!current.at("torch").get<std::string>().starts_with(manifest.at("versions").at("torch").get<std::string>()))
        throw std::runtime_error("oracle Torch does not match docker/nvidia-payload.json");
    const auto metadata = module("importlib.metadata");
    const auto requirement_type = module("packaging.requirements").attr("Requirement");
    const auto canonicalize = module("packaging.utils").attr("canonicalize_name");
    const auto canonical = [&](py::handle name) { return string(canonicalize(name)); };
    std::vector<std::pair<std::string, std::string>> pending;
    for (const auto& line : support::lines(support::read_text(OracleRoot / "requirements.txt"))) {
        if (line.empty() || line.starts_with('#')) continue;
        const auto separator = line.find("==");
        if (separator == std::string::npos || line.find("==", separator + 2) != std::string::npos)
            throw std::runtime_error("oracle dependency must have exactly one == pin: " + line);
        const auto name = line.substr(0, separator);
        const auto version = line.substr(separator + 2);
        if (string(metadata.attr("version")(name)) != version)
            throw std::runtime_error("incompatible oracle dependency " + name + ": expected " + version);
        pending.emplace_back(canonical(py::str(name)), "");
    }
    std::set<std::string> authoritative;
    for (const auto& [name, version] : current.at("distributions").items())
        authoritative.insert(canonical(py::str(name)));
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
                throw std::runtime_error("incompatible dependency closure: " + name + "[" + extra +
                                         "] requires " + string(requirement) + ", installed " + string(version));
            const auto dependency = canonical(requirement.attr("name"));
            pending.emplace_back(dependency, "");
            for (py::handle selected : requirement.attr("extras")) pending.emplace_back(dependency, string(selected));
        }
    }
    for (const char* name : {"transformers", "pytorch_lightning", "torchmetrics", "torch_linear_assignment"})
        module(name); // Actual eager numerical and Lightning import closure.
    current["dependency_closure"] = Json::object();
    for (const auto& [name, extra] : seen) current["dependency_closure"][name] = string(metadata.attr("version")(name));
    return current;
}

Json verify_source(const fs::path& source) {
    const auto git = [&](std::initializer_list<std::string> arguments) {
        std::vector<std::string> command{"git", "-c", "safe.directory=" + source.string(), "-C", source.string()};
        command.insert(command.end(), arguments.begin(), arguments.end());
        support::RunOptions options;
        options.capture_stdout = true;
        const auto result = support::run(command, options);
        check_signals();
        if (result.exit_code) throw std::runtime_error("upstream git validation failed with status " + std::to_string(result.exit_code));
        return support::trim(result.out);
    };
    if (git({"rev-parse", "HEAD"}) != Revision || !git({"status", "--porcelain", "--untracked-files=all"}).empty())
        throw std::runtime_error("upstream must be a clean checkout of " + std::string(Revision));
    if (!git({"diff", "--name-only", "HEAD"}).empty()) throw std::runtime_error("upstream source changed");
    std::vector<fs::path> files;
    const auto package = source / "src/rfdetr";
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
        try { close(); }
        catch (py::error_already_set& error) { error.discard_as_unraisable("oracle context restoration"); }
        catch (const std::exception& error) { std::cerr << "oracle context restoration failed: " << error.what() << '\n'; }
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
            try { patch.owner.attr(patch.name.c_str()) = patch.original; }
            catch (...) { if (!first) first = std::current_exception(); }
        }
        if (first) std::rethrow_exception(first);
    }
private:
    void restore_noexcept() noexcept {
        try { restore(); }
        catch (py::error_already_set& error) { error.discard_as_unraisable("oracle attribute restoration"); }
        catch (const std::exception& error) { std::cerr << "oracle attribute restoration failed: " << error.what() << '\n'; }
    }
    struct Patch { py::object owner; std::string name; py::object original; };
    std::vector<Patch> originals_;
};

class Archive {
public:
    void add(const std::string& name, py::handle value) {
        const auto torch = module("torch");
        if (inventory_.contains(name)) throw std::runtime_error("duplicate oracle tensor " + name);
        if (value.is_none()) {
            inventory_[name] = {{"defined", false}};
            return;
        }
        if (!py::isinstance(value, torch.attr("Tensor"))) throw py::type_error(name);
        const auto detached = object(value).attr("detach")().attr("as_subclass")(torch.attr("Tensor"));
        const auto tensor = detached.attr("cpu")().attr("contiguous")().attr("clone")();
        tensors_[py::str(name)] = tensor;
        const bool floating = truth(tensor.attr("is_floating_point")());
        const auto finite = floating ? torch.attr("isfinite")(tensor) :
                                      torch.attr("ones_like")(tensor, "dtype"_a = torch.attr("bool"));
        inventory_[name] = {{"defined", true}, {"shape", sequence_json(object(value).attr("shape"))},
                           {"dtype", string(object(value).attr("dtype"))},
                           {"device", string(object(value).attr("device"))},
                           {"stride", sequence_json(object(value).attr("stride")())},
                           {"finite_count", integer(finite.attr("sum")())},
                           {"elements", integer(object(value).attr("numel")())}};
        if (floating) tensors_[py::str("finite/" + name)] = finite;
    }
    Json write(const fs::path& directory, const std::string& name) {
        const auto path = directory / (name + ".pt");
        std::uint64_t size = 0;
        for (const auto& pair : tensors_) {
            const auto value = object(pair.second);
            const auto elements = static_cast<std::uint64_t>(integer(value.attr("numel")()));
            const auto width = static_cast<std::uint64_t>(integer(value.attr("element_size")()));
            if (width == 0 || elements > (MaxArchive - size) / width)
                throw std::runtime_error("tensor shard " + name + " exceeds its bound");
            size += elements * width;
        }
        module("torch").attr("save")(tensors_, path.string());
        const auto bytes = fs::file_size(path);
        if (bytes > MaxArchive) throw std::runtime_error("serialized tensor shard " + name + " exceeds its bound");
        return {{"file", path.filename().string()}, {"sha256", digest(path)}, {"bytes", bytes}, {"tensors", inventory_}};
    }
private:
    py::dict tensors_;
    Json inventory_ = Json::object();
};

using NamedTensors = std::vector<std::pair<std::string, py::object>>;

NamedTensors output_tensors(py::dict outputs) {
    std::vector<std::pair<std::string, py::dict>> layers{{"main", outputs}};
    if (outputs.contains("aux_outputs")) {
        std::size_t layer = 0;
        for (py::handle value : outputs["aux_outputs"])
            layers.emplace_back("aux." + std::to_string(layer++), py::cast<py::dict>(value));
    }
    if (outputs.contains("enc_outputs")) layers.emplace_back("enc", py::cast<py::dict>(outputs["enc_outputs"]));
    static const std::set<std::string> Allowed{"pred_logits", "pred_boxes", "pred_masks", "aux_outputs", "enc_outputs"};
    NamedTensors result;
    for (const auto& [label, layer] : layers) {
        for (const auto& pair : layer)
            if (!Allowed.contains(string(pair.first)))
                throw std::runtime_error("unexpected upstream output inventory in " + label + ": " + string(layer));
        for (const char* key : {"pred_logits", "pred_boxes", "pred_masks"}) {
            if (!layer.contains(key)) continue;
            const auto value = object(layer[key]);
            if (py::isinstance<py::dict>(value)) {
                for (const auto& pair : value.cast<py::dict>())
                    result.emplace_back(label + "/" + key + "/" + string(pair.first), object(pair.second));
            } else result.emplace_back(label + "/" + key, value);
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

bool frame_from(py::handle frame, std::string_view suffix) {
    return !frame.is_none() && string(object(frame).attr("f_code").attr("co_filename")).ends_with(suffix);
}

py::object detached_clone(py::handle value) { return object(value).attr("detach")().attr("clone")(); }

class ForwardOperators {
public:
    struct State {
        NamedTensors outputs;
        std::map<std::string, std::size_t> counts;
        bool active = true;
    };
    ForwardOperators() : state_(std::make_shared<State>()) {
        const auto functional = module("torch.nn.functional");
        for (const char* name : {"conv2d", "layer_norm", "linear", "gelu", "scaled_dot_product_attention"}) {
            const py::object original = functional.attr(name);
            auto callback = py::cpp_function([state = state_, name = std::string(name), original](py::args args, py::kwargs kwargs) {
                auto output = invoke(original, args, kwargs);
                if (state->active) {
                    if (state->outputs.size() >= 256) throw std::runtime_error("forward operator observation exceeds its bound");
                    const auto index = state->counts[name]++;
                    state->outputs.emplace_back("forward-operator/" + name + "/" + std::to_string(index), detached_clone(output));
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
    const std::shared_ptr<State>& state() const { return state_; }
private:
    std::shared_ptr<State> state_;
    AttributePatches patches_;
};

class Observe {
public:
    explicit Observe(ForwardOperators* operators = nullptr) : state_(std::make_shared<State>()),
                                                            sys_(module("sys")), previous_profile_(sys_.attr("getprofile")()) {
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
            if (uncertain) state->uncertain.push_back(detached_clone(result));
            else if (matcher) state->matcher.push_back(detached_clone(result));
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
                    for (const char* name : {"point_coords", "point_logits", "point_labels"})
                        direct.emplace_back(name, detached_clone(locals[name]));
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
        try { close(); }
        catch (py::error_already_set& error) { error.discard_as_unraisable("oracle observation restoration"); }
        catch (const std::exception& error) { std::cerr << "oracle observation restoration failed: " << error.what() << '\n'; }
    }
    Observe(const Observe&) = delete;
    Observe& operator=(const Observe&) = delete;
    void close() {
        // Match the original exit ordering: restore torch before the profiler.
        std::exception_ptr first;
        try { patches_.restore(); } catch (...) { first = std::current_exception(); }
        if (profiling_) {
            profiling_ = false;
            try { sys_.attr("setprofile")(previous_profile_); }
            catch (...) { if (!first) first = std::current_exception(); }
        }
        if (first) std::rethrow_exception(first);
    }
    void save(Archive& archive, const std::string& prefix = {}) const {
        for (std::size_t group = 0; group != state_->encoder.size(); ++group)
            archive.add(prefix + "encoder/" + std::to_string(group), state_->encoder[group]);
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

py::list targets_for(int batch, int resolution, bool segmentation) {
    const auto torch = module("torch");
    py::list targets;
    for (int image = 0; image != batch; ++image) {
        const int count = image == 0 ? 2 : 0;
        py::list labels;
        py::list boxes;
        if (count) {
            labels.append(1);
            labels.append(3);
            boxes.append(py::make_tuple(.31, .34, .22, .28));
            boxes.append(py::make_tuple(.72, .65, .19, .24));
        }
        py::dict target;
        target["labels"] = torch.attr("tensor")(labels, "dtype"_a = torch.attr("int64"), "device"_a = "cuda");
        target["boxes"] = torch.attr("tensor")(boxes, "dtype"_a = torch.attr("float32"), "device"_a = "cuda").attr("reshape")(count, 4);
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
        for (const auto& [name, tensor] : py::reinterpret_borrow<py::dict>(value))
            archive.add("target/" + std::to_string(image) + "/" + string(name), tensor);
        ++image;
    }
}

py::object float_operands(py::handle value) {
    if (py::isinstance(value, module("torch").attr("Tensor")))
        return truth(object(value).attr("is_floating_point")()) ? object(value).attr("float")() : object(value);
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
        for (const char* name : {"box_cxcywh_to_xyxy", "batch_sigmoid_ce_loss", "batch_dice_loss"})
            install(matcher, name, false, false);
        install(torch, "cdist", false, true);
    }
    void close() { patches_.restore(); }
private:
    void install(py::object owner, const char* name, bool method, bool caller_only) {
        const py::object original = owner.attr(name);
        py::object callback = py::cpp_function([original, caller_only](py::args args, py::kwargs kwargs) {
            if (caller_only && !frame_from(current_python_frame(), "/rfdetr/models/matcher.py"))
                return invoke(original, args, kwargs);
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
    for (const auto& pair : losses)
        archive.add(prefix + (weights.contains(pair.first) ? "loss/" : "statistic/") + string(pair.first), pair.second);
    archive.add(prefix + "loss/total", total);
    return total;
}

py::list parameter_values(const NamedTensors& parameters) {
    py::list values;
    for (const auto& [name, value] : parameters) values.append(value);
    return values;
}

void save_gradients(Archive& archive, const std::string& prefix, const NamedTensors& names, py::sequence gradients) {
    if (names.size() != static_cast<std::size_t>(gradients.size()))
        throw std::runtime_error("upstream gradient inventory length changed");
    for (std::size_t i = 0; i != names.size(); ++i) archive.add(prefix + names[i].first, gradients[i]);
}

void save_parameter_gradients(Archive& archive, const std::string& prefix, const NamedTensors& parameters) {
    for (const auto& [name, parameter] : parameters) archive.add(prefix + name, parameter.attr("grad"));
}

void save_optimizer_state(Archive& archive, const std::string& prefix, py::object optimizer, const NamedTensors& parameters) {
    for (const auto& [name, parameter] : parameters) {
        archive.add(prefix + "updated/" + name, parameter);
        const auto state = optimizer.attr("state").attr("get")(parameter, py::dict());
        for (const char* field : {"step", "exp_avg", "exp_avg_sq"})
            archive.add(prefix + "optimizer/" + name + "/" + field, state.attr("get")(field));
    }
}

py::object clip_gradients(const NamedTensors& parameters, py::handle max_norm) {
    return module("torch.nn.utils").attr("clip_grad_norm_")(
        parameter_values(parameters), max_norm, "foreach"_a = false, "error_if_nonfinite"_a = true);
}

void stock_update_evidence(Archive& archive, py::object optimizer, const NamedTensors& names,
                           py::sequence gradients, py::handle max_norm) {
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
        for (const auto& pair : group) if (string(pair.first) != "params") copy[pair.first] = pair.second;
        py::list parameters;
        for (py::handle parameter : group["params"]) parameters.append(shadow.at(index_by_id.at(parameter.ptr())).second);
        copy["params"] = parameters;
        groups.append(copy);
    }
    const auto stock_optimizer = module("torch.optim").attr("AdamW")(groups, "foreach"_a = false, "fused"_a = false);
    if (shadow.size() != static_cast<std::size_t>(gradients.size()))
        throw std::runtime_error("upstream stock gradient inventory length changed");
    for (std::size_t i = 0; i != shadow.size(); ++i)
        shadow[i].second.attr("grad") = gradients[i].is_none() ? py::none() : detached_clone(gradients[i]);
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

Json run_case(const fs::path& directory, py::object model, py::dict initial, py::object mc, py::object tc,
              py::object defaults, const std::string& preset, const std::string& dtype_name,
              int batch, bool training, int steps) {
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
    const auto optimizer = module("torch.optim").attr("AdamW")(
        get_param_dict(make_namespace(mc, tc, defaults), model), "lr"_a = tc.attr("lr"),
        "weight_decay"_a = tc.attr("weight_decay"), "foreach"_a = false, "fused"_a = false);
    const auto groups = optimizer_groups(optimizer, names);
    const auto identity = preset + "-" + dtype_name + "-b" + std::to_string(batch) + (training ? "-train" : "-eval");
    const int resolution = py::cast<int>(mc.attr("resolution"));
    Json records = Json::array();
    Json execution_policy;
    for (int step = 0; step != steps; ++step) {
        Archive archive;
        torch.attr("manual_seed")(731 + step);
        const auto images = normalized_input(batch, resolution, step * 11).attr("cuda")().attr("requires_grad_")(true);
        const auto mask = torch.attr("zeros")(batch, resolution, resolution, "dtype"_a = torch.attr("bool"), "device"_a = "cuda");
        const auto targets = targets_for(batch, resolution, truth(mc.attr("segmentation_head")));
        save_targets(archive, targets);
        archive.add("input", images);
        archive.add("input_mask", mask);
        optimizer.attr("zero_grad")("set_to_none"_a = true);
        std::unique_ptr<ForwardOperators> operators;
        if (step == 0) operators = std::make_unique<ForwardOperators>();
        Observe forward_observation(operators.get());
        py::dict outputs;
        {
            PythonContext autocast(torch.attr("autocast")("cuda", "dtype"_a = dtype, "enabled"_a = !fp32));
            execution_policy = {{"autocast_enabled", truth(torch.attr("is_autocast_enabled")("cuda"))},
                                {"autocast_dtype", string(torch.attr("get_autocast_dtype")("cuda"))}};
            outputs = model(nested_tensor(images, mask)).cast<py::dict>();
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
                auto cotangent = torch.attr("arange")(elements, "device"_a = value.attr("device"), "dtype"_a = torch.attr("float32"))
                                     .attr("reshape")(value.attr("shape"));
                cotangent = cotangent.attr("__mod__")(19).attr("__sub__")(9).attr("__truediv__")(std::max<std::int64_t>(elements, 1));
                cotangent = cotangent.attr("to")(value.attr("dtype"));
                archive.add("cotangent/" + key, cotangent);
                sum = add(sum, value.attr("__mul__")(cotangent).attr("sum")());
            }
            NamedTensors vjp_names{{"input", images}};
            for (const auto& [key, value] : trainable) vjp_names.emplace_back("parameter/" + key, value);
            const auto vjps = torch.attr("autograd").attr("grad")(sum, parameter_values(vjp_names),
                                                                 "allow_unused"_a = true, "retain_graph"_a = training);
            save_gradients(archive, "vjp/", vjp_names, vjps.cast<py::sequence>());
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
            const auto stock_gradients = torch.attr("autograd").attr("grad")(stock_total, parameter_values(trainable),
                                                                            "allow_unused"_a = true, "retain_graph"_a = true);
            save_gradients(archive, "stock/gradient/", trainable, stock_gradients.cast<py::sequence>());
            py::object total;
            std::unique_ptr<Observe> observation;
            if (fp32) {
                total = save_losses(archive, "", stock_losses, criterion.attr("weight_dict"));
                stock.save(archive);
            } else {
                if (steps != 1) throw std::runtime_error("AMP stock update evidence requires an identical initial state");
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
                    held_outputs = model(nested_tensor(held, mask)).cast<py::dict>();
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
            model.attr("train")(true);
            held_criterion.attr("train")(true);
        }
        auto record = archive.write(directory, identity + "-step" + std::to_string(step));
        record["step"] = step;
        records.push_back(std::move(record));
        std::cout << "upstream oracle produced " << identity << " step=" << step << std::endl;
    }
    Json trainable_names = Json::array();
    for (const auto& [name, value] : trainable) trainable_names.push_back(name);
    Json optimizer_settings = Json::object();
    for (const char* key : {"lr", "lr_encoder", "weight_decay", "lr_vit_layer_decay", "lr_component_decay"})
        optimizer_settings[key] = to_json(tc.attr(key));
    return {{"id", identity}, {"preset", preset}, {"precision", dtype_name}, {"batch", batch},
            {"execution_policy", execution_policy},
            {"comparison_policy", fp32 || !training ? "untouched_stock" : "upstream_with_fp32_matcher_boundary"},
            {"training", training}, {"resolution", resolution}, {"groups", to_json(mc.attr("group_detr"))},
            {"classes", py::cast<int>(mc.attr("num_classes")) + 1}, {"trainable", trainable_names},
            {"parameter_groups", groups}, {"loss_weights", to_json(criterion.attr("weight_dict"))},
            {"clip_max_norm", to_json(tc.attr("clip_max_norm"))}, {"optimizer", optimizer_settings}, {"steps", records}};
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
        for (const char* suffix : {"weight", "bias"})
            check_copy("transformer.enc_out_class_embed." + std::to_string(group) + "." + suffix,
                       std::string("class_embed.") + suffix);
        for (int layer = 0; layer != 3; ++layer) {
            for (const char* suffix : {"weight", "bias"})
                check_copy("transformer.enc_out_bbox_embed." + std::to_string(group) + ".layers." + std::to_string(layer) + "." + suffix,
                           "bbox_embed.layers." + std::to_string(layer) + "." + suffix);
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
    struct Case { const char* name; int height; int width; int out_height; int out_width; };
    constexpr std::array Cases{
        Case{"identity", 13, 17, 13, 17}, Case{"enlarge", 7, 11, 19, 27},
        Case{"shrink", 37, 53, 11, 17}, Case{"odd_aspect", 9, 41, 23, 13},
        Case{"high_frequency", 64, 65, 13, 17}};
    Archive archive;
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
        const auto rgb = numpy.attr("frombuffer")(py::bytes(bytes), "dtype"_a = numpy.attr("uint8"))
                             .attr("reshape")(entry.height, entry.width, 3).attr("copy")();
        const auto image = image_type.attr("fromarray")(rgb, "RGB");
        const auto source = functional.attr("to_dtype")(functional.attr("to_image")(image), torch.attr("float32"), "scale"_a = true);
        const std::string name = entry.name;
        archive.add(name + "/rgb8", torch.attr("from_numpy")(rgb.attr("copy")()));
        archive.add(name + "/input", source);
        for (const bool antialias : {false, true}) {
            const auto value = functional.attr("resize")(source, py::list(py::make_tuple(entry.out_height, entry.out_width)),
                                                         "interpolation"_a = interpolation.attr("BILINEAR"), "antialias"_a = antialias);
            archive.add(name + (antialias ? "/torch_antialias" : "/torch"), value);
        }
        const auto resized = resize(py::make_tuple(entry.out_height, entry.out_width))(image, py::none());
        const auto pil_tensor = transforms.attr("ToImage")()(index(resized, 0));
        const auto pil_float = transforms.attr("ToDtype")(torch.attr("float32"), "scale"_a = true)(pil_tensor);
        const auto normalized = normalize(py::list(py::make_tuple(.485, .456, .406)), py::list(py::make_tuple(.229, .224, .225)))
                                    (pil_float, index(resized, 1));
        archive.add(name + "/pil_rgb8", pil_tensor);
        archive.add(name + "/pil", pil_float);
        archive.add(name + "/pil_normalized", index(normalized, 0));
        case_records.push_back(Json::array({name, entry.height, entry.width, entry.out_height, entry.out_width}));
    }
    return {{"archive", archive.write(directory, "resize")}, {"cases", case_records},
            {"tensor_order", "RGB8 -> float32/255 -> bilinear align_corners=false (named antialias), no output quantization"},
            {"dataset_order", "RGB8 PIL -> upstream Resize bilinear antialias -> RGB8 quantization -> upstream ToImage/ToDtype(scale) -> Normalize"}};
}

struct Arguments {
    std::string payload_snapshot;
    bool check_environment = false;
    std::string upstream;
    std::string output;
    std::string weights;
    std::string fingerprint;
    std::string sdpa = "default";
    bool help = false;
};

void generate(const Arguments& args) {
    const auto torch = module("torch");
    const auto source = fs::weakly_canonical(fs::absolute(args.upstream));
    const auto source_hashes = verify_source(source);
    const auto environment = check_environment();
    module("sys").attr("path").attr("insert")(0, (source / "src").string());
    const auto configs = module("rfdetr.config");
    const auto defaults_type = module("rfdetr.models._defaults").attr("ModelDefaults");
    const auto build_model = module("rfdetr.models.lwdetr").attr("build_model_from_config");
    const auto load_pretrain = module("rfdetr.models.weights").attr("load_pretrain_weights");
    const auto weight_assets = module("rfdetr.assets.model_weights");
    const py::object get_param_dict = module("rfdetr.training.param_groups").attr("get_param_dict");
    (void)get_param_dict; // Import/attribute validation is intentionally eager.
    const py::object cuda = torch.attr("cuda");
    if (!truth(cuda.attr("is_available")()))
        throw std::runtime_error("upstream oracle requires CUDA; no hardware skip counts as evidence");
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
    // Deliberately no cleanup guard: just as in the Python producer, an
    // incomplete generation retains its tensors for diagnosis, without a
    // manifest. Only a complete generation is renamed into references/.
    std::string pattern = (output / ".producing-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::system_error(errno, std::generic_category(), "mkdtemp oracle generation");
    const fs::path directory = pattern;
    const py::object build_config = torch.attr("__config__").attr("show")();
    const auto build_config_size = py::len(build_config);
    const auto build_config_prefix = item(build_config, py::slice(py::none(), py::int_(65536), py::none()));
    Json policy = {
        {"sdpa_backend", args.sdpa},
        {"matmul_tf32", truth(cuda_backend.attr("matmul").attr("allow_tf32"))},
        {"cudnn_tf32", truth(cudnn.attr("allow_tf32"))},
        {"matmul_precision", string(torch.attr("get_float32_matmul_precision")())},
        {"cudnn_benchmark", truth(cudnn.attr("benchmark"))},
        {"flash_sdp", truth(cuda_backend.attr("flash_sdp_enabled")())},
        {"math_sdp", truth(cuda_backend.attr("math_sdp_enabled")())},
        {"memory_efficient_sdp", truth(cuda_backend.attr("mem_efficient_sdp_enabled")())},
        {"cudnn_sdp", truth(cuda_backend.attr("cudnn_sdp_enabled")())},
        {"attention", "upstream grid_sample deformable attention; native production CUDA"},
        {"matcher", "FP32 cases use untouched stock; AMP main uses fp32_matcher_costs: original HungarianMatcher._focal_classification_cost, matcher.box_cxcywh_to_xyxy, matcher.batch_sigmoid_ce_loss, matcher.batch_dice_loss and matcher-origin torch.cdist receive FP32 operands with autocast disabled; original mask point_sample/einsum and matcher forward/class/batched eligibility are unchanged; stock prefix is untouched AMP"},
        {"gradient_scale", 1}, {"accumulated_microbatches", 1}};
    Json manifest = {{"format", Format}, {"revision", Revision}, {"source_hashes", source_hashes},
                     {"producer_sha256", digest(support::executable_path())},
                     {"dependency_sha256", digest(OracleRoot / "requirements.txt")},
                     {"payload_sha256", digest(OracleRoot / "nvidia-payload.json")},
                     {"environment", environment}, {"fingerprint", args.fingerprint},
                     {"cuda_device", string(cuda.attr("get_device_name")())},
                     {"torch_build_config", string(build_config_prefix)},
                     {"torch_build_config_truncated", build_config_size > 65536}, {"policy", policy},
                     {"presets", Json::array()}, {"cases", Json::array()}, {"unsupported", Json::array()}};
    std::vector<std::string> precisions{"fp32", "fp16"};
    if (truth(cuda.attr("is_bf16_supported")())) precisions.emplace_back("bf16");
    else manifest["unsupported"].push_back("bf16: CUDA device does not support BF16");
    const auto defaults = defaults_type("force_no_pretrain"_a = true);
    struct Preset { const char* name; const char* config; const char* train; const char* weight; };
    constexpr std::array Presets{
        Preset{"rf-detr-nano", "RFDETRNanoConfig", "TrainConfig", "rf-detr-nano.pth"},
        Preset{"rf-detr-seg-nano", "RFDETRSegNanoConfig", "SegmentationTrainConfig", "rf-detr-seg-nano.pt"}};
    for (const auto& preset : Presets) {
        const auto weights = fs::path(args.weights) / preset.weight;
        if (!fs::is_regular_file(weights)) throw std::runtime_error("official pretrained artifact missing: " + weights.string());
        const auto asset = weight_assets.attr("ModelWeights").attr("from_filename")(preset.weight);
        if (asset.is_none() || !truth(asset.attr("md5_hash")))
            throw std::runtime_error("pinned upstream registry has no integrity identity: " + std::string(preset.weight));
        weight_assets.attr("validate_pretrain_weights")(weights.string(), "strict"_a = true);
        const auto verified_sha256 = digest(weights);
        const auto mc = configs.attr(preset.config)("pretrain_weights"_a = weights.string(), "group_detr"_a = 2, "device"_a = "cuda");
        const auto tc = configs.attr(preset.train)("dataset_dir"_a = ".", "output_dir"_a = ".", "lr"_a = 1e-4,
                                                 "lr_encoder"_a = 1e-4, "grad_accum_steps"_a = 1,
                                                 "clip_max_norm"_a = .1, "weight_decay"_a = 1e-4);
        torch.attr("manual_seed")(731);
        const auto model = build_model(mc, tc, defaults);
        const py::object attention = index(model.attr("backbone"), 0).attr("encoder").attr("encoder").attr("config").attr("_attn_implementation");
        std::cout << "upstream backbone attention=" << string(attention) << std::endl;
        const auto invariants = constructor_evidence(model);
        load_pretrain(model, mc);
        py::dict initial;
        {
            const py::dict state = model.attr("state_dict")();
            for (const auto& pair : state)
                initial[pair.first] = object(pair.second).attr("detach")().attr("cpu")().attr("clone")();
        }
        const auto checkpoint = directory / (std::string(preset.name) + ".pth");
        const auto make_namespace = module("rfdetr._namespace").attr("_namespace_from_configs");
        torch.attr("save")(py::dict("model"_a = initial, "args"_a = make_namespace(mc, tc, defaults)), checkpoint.string());
        const auto original = directory / ("original-" + std::string(preset.weight));
        fs::copy_file(weights, original, fs::copy_options::overwrite_existing);
        const auto original_sha256 = digest(original);
        if (original_sha256 != verified_sha256)
            throw std::runtime_error("verified pretrained artifact changed during production: " + weights.string());
        manifest["presets"].push_back({
            {"name", preset.name}, {"checkpoint", checkpoint.filename().string()}, {"sha256", digest(checkpoint)},
            {"original", original.filename().string()}, {"original_sha256", original_sha256},
            {"verified_asset", {{"registry", "rfdetr.assets.model_weights.ModelWeights"}, {"revision", Revision},
                                {"filename", to_json(asset.attr("filename"))}, {"url", to_json(asset.attr("url"))},
                                {"md5", to_json(asset.attr("md5_hash"))}, {"sha256", verified_sha256}}},
            {"model_config", to_json(mc.attr("model_dump")("mode"_a = "json"))},
            {"train_config", to_json(tc.attr("model_dump")("mode"_a = "json"))}, {"constructor", invariants}});
        model.attr("cuda")();
        for (const auto& precision : precisions) {
            for (const bool training : {false, true}) {
                for (const int batch : {1, 2}) {
                    const int steps = training && batch == 2 && std::string_view(preset.name) == "rf-detr-nano" && precision == "fp32" ? 8 : 1;
                    manifest["cases"].push_back(run_case(directory, model, initial, mc, tc, defaults, preset.name, precision, batch, training, steps));
                }
            }
        }
    }
    manifest["resize"] = resize_evidence(directory);
    if (source_hashes != verify_source(source)) throw std::runtime_error("upstream source changed during production");
    manifest["status"] = "references_complete_native_comparison_pending";
    fs::rename(directory, output / "references");
    manifest["directory"] = "references";
    json_write(output / "manifest.json", manifest);
}

void usage(std::ostream& stream, const char* program) {
    stream << "usage: " << program << " [-h] [--payload-snapshot PATH] [--check-environment]\n"
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
              "  --upstream PATH        clean checkout of the pinned revision\n"
              "  --output PATH          fresh reference output directory\n"
              "  --weights PATH         official cached pretrained artifacts\n"
              "  --fingerprint VALUE    wrapper-provided image fingerprint\n"
              "  --sdpa {default,math}  upstream SDPA policy (default: default)\n";
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
    const std::array<std::string_view, 9> options{
        "--payload-snapshot", "--check-environment", "--upstream", "--output", "--weights", "--fingerprint", "--sdpa", "--help", "-h"};
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
                for (const auto known : options) if (known.starts_with(option)) matches.push_back(known);
            if (matches.size() > 1) throw support::ToolError("ambiguous option: " + option, 2);
            if (matches.empty()) throw support::ToolError("unrecognized arguments: " + argument, 2);
            option = matches.front();
        }
        if (option == "--help" || option == "-h") {
            if (equals != std::string::npos) throw support::ToolError("argument " + option + ": ignored explicit argument", 2);
            result.help = true;
            return result;
        }
        if (option == "--check-environment") {
            if (equals != std::string::npos) throw support::ToolError("argument " + option + ": ignored explicit argument", 2);
            result.check_environment = true;
            continue;
        }
        std::string value;
        if (equals != std::string::npos) value = argument.substr(equals + 1);
        else {
            if (++i == argc || optional_token(argv[i]))
                throw support::ToolError("argument " + option + ": expected one argument", 2);
            value = argv[i];
        }
        if (option == "--payload-snapshot") result.payload_snapshot = value;
        else if (option == "--upstream") result.upstream = value;
        else if (option == "--output") result.output = value;
        else if (option == "--weights") result.weights = value;
        else if (option == "--fingerprint") result.fingerprint = value;
        else if (option == "--sdpa") {
            if (value != "default" && value != "math")
                throw support::ToolError("argument --sdpa: invalid choice: '" + value + "' (choose from 'default', 'math')", 2);
            result.sdpa = value;
        }
    }
    if (result.payload_snapshot.empty() && !result.check_environment &&
        (result.upstream.empty() || result.output.empty() || result.weights.empty() || result.fingerprint.empty()))
        throw support::ToolError("generation requires --upstream, --output, --weights and --fingerprint", 2);
    return result;
}

int execute(const Arguments& args) {
    // Honor the donor interpreter's environment/site configuration without
    // inserting the launch directory ahead of the pinned dependency overlay.
    py::scoped_interpreter interpreter{true, 0, nullptr, false};
    try {
        if (!args.payload_snapshot.empty()) json_write(args.payload_snapshot, payload());
        else if (args.check_environment) check_environment();
        else generate(args);
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
        std::cerr << "RF-DETR upstream oracle failed: " << string(error.type().attr("__name__"))
                  << ": " << string(error.value()) << '\n';
        const bool interrupted = error.matches(PyExc_KeyboardInterrupt);
        error.restore();
        PyErr_PrintEx(0);
        return interrupted ? 130 : 1;
    }
}

} // namespace

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
        } else std::cerr << "RF-DETR upstream oracle failed: RuntimeError: " << error.what() << '\n';
        return error.status;
    } catch (const std::exception& error) {
        std::cerr << "RF-DETR upstream oracle failed: RuntimeError: " << error.what() << '\n';
        return 1;
    }
}
