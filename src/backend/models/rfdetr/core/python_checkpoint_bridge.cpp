#include <sys/wait.h>
#include <torch/torch.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <meta>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "detail/model_state_access.h"
#include "detail/model_state_technical.h"
#include "detail/scalar_type_utils.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/common/io/filesystem_utils.h"
#include "src/common/system/runtime_paths.h"
#include "src/frameworks/process/subprocess_utils.h"

namespace mmltk::backend::models::rfdetr {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

void write_child_diagnostic(const char* data, size_t size) noexcept {
    while (size > 0U) {
        const ssize_t written = ::write(STDERR_FILENO, data, size);
        if (written > 0) {
            const auto consumed = static_cast<size_t>(written);
            data += consumed;
            size -= consumed;
            continue;
        }
        if (written < 0 && errno == EINTR) { continue; }
        return;
    }
}

size_t tensor_nbytes(const torch::Tensor& tensor) {
    return static_cast<size_t>(tensor.numel()) * static_cast<size_t>(tensor.element_size());
}

std::string tensor_entry_filename(const size_t index) { return std::format("entry_{:06}.bin", index); }

fs::path make_temp_directory(const char* prefix) {
    const fs::path template_path = fs::temp_directory_path() / (std::string(prefix) + "XXXXXX");
    std::string template_string = template_path.string();
    std::vector<char> buffer(template_string.begin(), template_string.end());
    buffer.push_back('\0');

    char* created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
        throw std::runtime_error(std::string("failed to create temporary RF-DETR checkpoint directory: ") + std::strerror(errno));
    }
    return {created};
}

void remove_path_recursively_best_effort(const fs::path& path) {
    mmltk::common::io::filesystem_utils::remove_path_recursively_best_effort(path);
}

struct ScopedTempDirectory {
    explicit ScopedTempDirectory(const char* prefix) : path(make_temp_directory(prefix)) {}

    ~ScopedTempDirectory() {
        try {
            remove_path_recursively_best_effort(path);
        } catch (...) { (void)0; }
    }

    fs::path path;
};

void write_json_file(const fs::path& path, const json& payload) {
    std::ofstream stream(path);
    if (!stream.is_open()) { throw std::runtime_error("failed to write RF-DETR checkpoint manifest: " + path.string()); }
    stream << payload.dump(2) << '\n';
}

void write_raw_tensor_file(const fs::path& path, const torch::Tensor& tensor) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream.is_open()) { throw std::runtime_error("failed to write RF-DETR checkpoint tensor payload: " + path.string()); }
    const size_t bytes = tensor_nbytes(tensor);
    if (bytes != 0U) { stream.write(static_cast<const char*>(tensor.data_ptr()), static_cast<std::streamsize>(bytes)); }
    if (!stream.good()) { throw std::runtime_error("failed to write RF-DETR checkpoint tensor payload: " + path.string()); }
}

json read_json_file(const fs::path& path) {
    std::ifstream stream(path);
    if (!stream.is_open()) { throw std::runtime_error("failed to read RF-DETR checkpoint manifest: " + path.string()); }
    return json::parse(stream);
}

template <typename Value>
std::optional<Value> manifest_optional_value(const json& object, const char* key) {
    static_assert(std::is_same_v<Value, bool> || std::is_same_v<Value, int64_t> || std::is_same_v<Value, double>);
    const auto found = object.find(key);
    if (found == object.end()) { return std::nullopt; }
    const bool matches = [&] {
        if constexpr (std::is_same_v<Value, bool>) {
            return found->is_boolean();
        } else if constexpr (std::is_same_v<Value, int64_t>) {
            return found->is_number_integer();
        } else {
            return found->is_number();
        }
    }();
    constexpr const char* expected_type = [] {
        if constexpr (std::is_same_v<Value, bool>) {
            return "a bool";
        } else if constexpr (std::is_same_v<Value, int64_t>) {
            return "an int";
        } else {
            return "numeric";
        }
    }();
    if (!matches) {
        throw std::runtime_error(std::string("RF-DETR checkpoint manifest metadata key is not ") + expected_type + ": " + key);
    }
    return found->get<Value>();
}

void wait_for_child(const pid_t child_pid, const char* operation) {
    const int status = mmltk::frameworks::process::wait_child_process(child_pid);
    if (status < 0) {
        throw std::runtime_error(
            std::format("failed to wait for RF-DETR Python checkpoint bridge during {}: {}", operation, std::strerror(errno)));
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) { return; }

    std::string detail;
    if (WIFEXITED(status)) {
        detail = std::format(" with exit code {}", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        detail = std::format(" with signal {}", WTERMSIG(status));
    }
    throw std::runtime_error(std::format("RF-DETR Python checkpoint bridge failed during {}{}", operation, detail));
}

fs::path checkpoint_bridge_script_path() {
#ifdef MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE
    fs::path source_path = MMLTK_RFDETR_PYTHON_CHECKPOINT_BRIDGE_SOURCE;
    if (fs::exists(source_path)) { return source_path; }
#endif
    return mmltk::common::system::runtime_paths::python_asset_path("rfdetr_checkpoint_bridge.py");
}

void run_python_bridge(const char* operation, const std::vector<std::string>& arguments) {
#if !MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
    (void)operation;
    (void)arguments;
    throw std::runtime_error("RF-DETR upstream Python checkpoint loading is disabled at build time");
#else
    std::vector<std::string> command;
    command.reserve(arguments.size() + 2);
    command.emplace_back(MMLTK_RFDETR_PYTHON_EXECUTABLE);
    command.emplace_back(checkpoint_bridge_script_path().string());
    command.insert(command.end(), arguments.begin(), arguments.end());

    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (auto& part : command) {
        argv.push_back(part.data());
    }
    argv.push_back(nullptr);

    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        throw std::runtime_error(std::string("failed to fork RF-DETR Python checkpoint bridge during ") + operation + ": " +
                                 std::strerror(errno));
    }
    if (child_pid == 0) {
        ::unsetenv("LD_LIBRARY_PATH");
        ::unsetenv("PYTHONPATH");
        ::execv(argv.front(), argv.data());
        constexpr char exec_failure[] = "RF-DETR Python checkpoint bridge exec failed\n";
        write_child_diagnostic(exec_failure, sizeof(exec_failure) - 1U);
        std::_Exit(127);
    }

    wait_for_child(child_pid, operation);
#endif
}

void populate_metadata_from_manifest(const json& metadata_json, NativeCheckpointMetadata& metadata) {
    metadata.num_queries = manifest_optional_value<int64_t>(metadata_json, "num_queries").value_or(0);
    metadata.num_select = manifest_optional_value<int64_t>(metadata_json, "num_select").value_or(0);
    metadata.for_each_detection_field([&metadata_json]<class Name, class Optional>(const Name& name, Optional& field) {
        field = manifest_optional_value<typename Optional::value_type>(metadata_json, name);
    });
}

DecodedNativeModelState load_checkpoint_from_manifest(const fs::path& manifest_path) {
    const json manifest = read_json_file(manifest_path);
    DecodedNativeModelState checkpoint;
    if (const auto metadata_it = manifest.find("metadata"); metadata_it != manifest.end()) {
        if (!metadata_it->is_object()) {
            throw std::runtime_error("RF-DETR checkpoint manifest metadata is not an object: " + manifest_path.string());
        }
        populate_metadata_from_manifest(*metadata_it, checkpoint.metadata);
    }

    const auto found = manifest.find("state_dict");
    if (found == manifest.end() || !found->is_array()) {
        throw std::runtime_error("RF-DETR checkpoint manifest is missing state_dict array: " + manifest_path.string());
    }

    auto& entries = detail::model_state_owner(checkpoint).entries;
    entries.reserve(found->size());
    for (const auto& entry_json : *found) {
        if (!entry_json.is_object()) {
            throw std::runtime_error("RF-DETR checkpoint manifest entry is not an object: " + manifest_path.string());
        }
        const auto name_it = entry_json.find("name");
        const auto tensor_it = entry_json.find("tensor_path");
        const auto dtype_it = entry_json.find("dtype");
        const auto sizes_it = entry_json.find("sizes");
        if (name_it == entry_json.end() || !name_it->is_string() || tensor_it == entry_json.end() || !tensor_it->is_string() ||
            dtype_it == entry_json.end() || !dtype_it->is_string() || sizes_it == entry_json.end() || !sizes_it->is_array()) {
            throw std::runtime_error("RF-DETR checkpoint manifest entry is missing name/tensor_path/dtype/sizes");
        }

        const fs::path tensor_path = manifest_path.parent_path() / tensor_it->get<std::string>();
        std::vector<int64_t> sizes;
        sizes.reserve(sizes_it->size());
        for (const auto& size_json : *sizes_it) {
            if (!size_json.is_number_integer()) { throw std::runtime_error("RF-DETR checkpoint manifest tensor shape is not integral"); }
            sizes.push_back(size_json.get<int64_t>());
        }
        const auto scalar_type = scalar_type_from_name(dtype_it->get<std::string>());
        torch::Tensor tensor = torch::empty(sizes, torch::TensorOptions().dtype(scalar_type).device(torch::kCPU));
        std::ifstream tensor_stream(tensor_path, std::ios::binary);
        if (!tensor_stream.is_open()) {
            throw std::runtime_error("failed to open RF-DETR checkpoint tensor payload: " + tensor_path.string());
        }
        const size_t bytes = tensor_nbytes(tensor);
        if (bytes > 0) {
            tensor_stream.read(static_cast<char*>(tensor.data_ptr()), static_cast<std::streamsize>(bytes));
            if (tensor_stream.gcount() != static_cast<std::streamsize>(bytes) || tensor_stream.bad()) {
                throw std::runtime_error("failed to read RF-DETR checkpoint tensor payload: " + tensor_path.string());
            }
        }
        entries.push_back({
            name_it->get<std::string>(),
            tensor.contiguous(),
        });
    }

    if (entries.empty()) {
        throw std::runtime_error("RF-DETR checkpoint manifest produced an empty state_dict: " + manifest_path.string());
    }
    return checkpoint;
}

json manifest_from_model_state(const fs::path& root, const std::vector<NormalizedModelStateEntry>& entries) {
    json manifest;
    manifest["state_dict"] = json::array();
    const fs::path tensor_dir = root / "tensors";
    fs::create_directories(tensor_dir);
    for (size_t index = 0U; index < entries.size(); ++index) {
        const torch::Tensor tensor = entries[index].tensor.detach().to(torch::kCPU).contiguous();
        const fs::path tensor_path = tensor_dir / tensor_entry_filename(index);
        write_raw_tensor_file(tensor_path, tensor);
        manifest["state_dict"].push_back({{"name", entries[index].name},
                                          {"tensor_path", fs::relative(tensor_path, root).string()},
                                          {"dtype", scalar_type_name(tensor.scalar_type())},
                                          {"sizes", tensor.sizes().vec()}});
    }
    return manifest;
}

}  // namespace

DecodedNativeModelState decode_upstream_python_model_state(const fs::path& checkpoint_path) {
    ScopedTempDirectory temp_dir("mmltk_rfdetr_load_");
    const fs::path manifest_path = temp_dir.path / "manifest.json";
    const fs::path tensor_dir = temp_dir.path / "tensors";
    fs::create_directories(tensor_dir);

    run_python_bridge("checkpoint export", {
                                               "export-upstream",
                                               "--input",
                                               std::filesystem::absolute(checkpoint_path).lexically_normal().string(),
                                               "--manifest",
                                               manifest_path.string(),
                                               "--tensor-dir",
                                               tensor_dir.string(),
                                           });

    return load_checkpoint_from_manifest(manifest_path);
}

void write_upstream_model_state(const fs::path& checkpoint_path, const DecodedNativeModelState& model_state) {
    const auto& entries = detail::model_state_owner(model_state).entries;
    if (entries.empty()) { throw std::invalid_argument("RF-DETR upstream checkpoint model state must not be empty"); }
    ScopedTempDirectory temp_dir("mmltk_rfdetr_save_");
    const fs::path manifest_path = temp_dir.path / "manifest.json";
    write_json_file(manifest_path, manifest_from_model_state(temp_dir.path, entries));
    const fs::path output = std::filesystem::absolute(checkpoint_path).lexically_normal();
    fs::create_directories(output.parent_path());
    run_python_bridge("checkpoint write", {"write-upstream", "--output", output.string(), "--manifest", manifest_path.string()});
}

}  // namespace mmltk::backend::models::rfdetr
