#include "rfdetr/python_checkpoint_bridge.h"

#include "filesystem_utils.h"
#include "gui/subprocess_utils.h"
#include "mmltk_logging.h"
#include "runtime_paths.h"
#include "rfdetr/checkpoint_internal.h"
#include "rfdetr/scalar_type_utils.h"

#include <nlohmann/json.hpp>
#include <torch/torch.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace mmltk::rfdetr {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

size_t tensor_nbytes(const torch::Tensor& tensor) {
    return static_cast<size_t>(tensor.numel()) * static_cast<size_t>(tensor.element_size());
}

std::string tensor_entry_filename(const size_t index) {
    return std::format("entry_{:06}.bin", index);
}

fs::path make_temp_directory(const char* prefix) {
    const fs::path template_path = fs::temp_directory_path() / (std::string(prefix) + "XXXXXX");
    std::string template_string = template_path.string();
    std::vector<char> buffer(template_string.begin(), template_string.end());
    buffer.push_back('\0');

    char* created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
        throw std::runtime_error(std::string("failed to create temporary RF-DETR checkpoint directory: ") +
                                 std::strerror(errno));
    }
    return {created};
}

void remove_path_recursively_best_effort(const fs::path& path) {
    mmltk::filesystem_utils::remove_path_recursively_best_effort(path);
}

void ensure_output_path_writable(const fs::path& output_path) {
    const fs::path output_dir = output_path.has_parent_path() ? output_path.parent_path() : fs::current_path();
    std::error_code create_error;
    fs::create_directories(output_dir, create_error);
    if (create_error) {
        throw std::runtime_error("RF-DETR Python checkpoint bridge cannot prepare output directory " +
                                 output_dir.string() + ": " + create_error.message() + "; check perms");
    }

    const std::string output_dir_string = output_dir.string();
    if (::access(output_dir_string.c_str(), W_OK | X_OK) != 0) {
        throw std::runtime_error("RF-DETR Python checkpoint bridge cannot write to output directory " +
                                 output_dir.string() + ": " + std::strerror(errno) + "; check perms");
    }

    std::error_code exists_error;
    if (fs::exists(output_path, exists_error) && !exists_error) {
        const std::string output_path_string = output_path.string();
        if (::access(output_path_string.c_str(), W_OK) != 0) {
            throw std::runtime_error("RF-DETR Python checkpoint bridge cannot overwrite output file " +
                                     output_path.string() + ": " + std::strerror(errno) + "; check perms");
        }
    }
}

struct ScopedTempDirectory {
    explicit ScopedTempDirectory(const char* prefix) : path(make_temp_directory(prefix)) {}

    ~ScopedTempDirectory() {
        try {
            remove_path_recursively_best_effort(path);
        } catch (...) {
            (void)0;
        }
    }

    fs::path path;
};

void write_json_file(const fs::path& path, const json& payload) {
    std::ofstream stream(path);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to write RF-DETR checkpoint manifest: " + path.string());
    }
    stream << payload.dump(2) << '\n';
}

json read_json_file(const fs::path& path) {
    std::ifstream stream(path);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to read RF-DETR checkpoint manifest: " + path.string());
    }
    return json::parse(stream);
}

std::optional<bool> manifest_optional_bool(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return std::nullopt;
    }
    if (!found->is_boolean()) {
        throw std::runtime_error(std::string("RF-DETR checkpoint manifest metadata key is not a bool: ") + key);
    }
    return found->get<bool>();
}

std::optional<int64_t> manifest_optional_int(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return std::nullopt;
    }
    if (!found->is_number_integer()) {
        throw std::runtime_error(std::string("RF-DETR checkpoint manifest metadata key is not an int: ") + key);
    }
    return found->get<int64_t>();
}

std::optional<double> manifest_optional_number(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return std::nullopt;
    }
    if (!found->is_number()) {
        throw std::runtime_error(std::string("RF-DETR checkpoint manifest metadata key is not numeric: ") + key);
    }
    return found->get<double>();
}

void write_raw_tensor_file(const fs::path& path, const torch::Tensor& tensor) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to write RF-DETR checkpoint tensor payload: " + path.string());
    }
    const size_t bytes = tensor_nbytes(tensor);
    if (bytes > 0) {
        stream.write(static_cast<const char*>(tensor.data_ptr()), static_cast<std::streamsize>(bytes));
        if (!stream.good()) {
            throw std::runtime_error("failed to write RF-DETR checkpoint tensor payload: " + path.string());
        }
    }
}
void wait_for_child(const pid_t child_pid, const char* operation) {
    const int status = mmltk::gui::subprocess::wait_child_process(child_pid);
    if (status < 0) {
        throw std::runtime_error(std::format("failed to wait for RF-DETR Python checkpoint bridge during {}: {}",
                                             operation, std::strerror(errno)));
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return;
    }

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
    if (fs::exists(source_path)) {
        return source_path;
    }
#endif
    return runtime_paths::python_asset_path("rfdetr_checkpoint_bridge.py");
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
        throw std::runtime_error(std::string("failed to fork RF-DETR Python checkpoint bridge during ") + operation +
                                 ": " + std::strerror(errno));
    }
    if (child_pid == 0) {
        ::unsetenv("LD_LIBRARY_PATH");
        ::unsetenv("PYTHONPATH");
        ::execv(argv.front(), argv.data());
        mmltk::logging::logger("rfdetr.checkpoint")
            ->error("failed to exec Python checkpoint bridge: {}", std::strerror(errno));
        std::_Exit(127);
    }

    wait_for_child(child_pid, operation);
#endif
}

void populate_metadata_from_manifest(const json& metadata_json, NativeCheckpointMetadata& metadata) {
    metadata.sum_group_losses = manifest_optional_bool(metadata_json, "sum_group_losses");
    metadata.use_varifocal_loss = manifest_optional_bool(metadata_json, "use_varifocal_loss");
    metadata.use_position_supervised_loss = manifest_optional_bool(metadata_json, "use_position_supervised_loss");
    metadata.ia_bce_loss = manifest_optional_bool(metadata_json, "ia_bce_loss");
    metadata.aux_loss = manifest_optional_bool(metadata_json, "aux_loss");
    metadata.mask_point_sample_ratio = manifest_optional_int(metadata_json, "mask_point_sample_ratio");
    metadata.focal_alpha = manifest_optional_number(metadata_json, "focal_alpha");
    metadata.cls_loss_coef = manifest_optional_number(metadata_json, "cls_loss_coef");
    metadata.bbox_loss_coef = manifest_optional_number(metadata_json, "bbox_loss_coef");
    metadata.giou_loss_coef = manifest_optional_number(metadata_json, "giou_loss_coef");
    metadata.mask_ce_loss_coef = manifest_optional_number(metadata_json, "mask_ce_loss_coef");
    metadata.mask_dice_loss_coef = manifest_optional_number(metadata_json, "mask_dice_loss_coef");
    metadata.set_cost_class = manifest_optional_number(metadata_json, "set_cost_class");
    metadata.set_cost_bbox = manifest_optional_number(metadata_json, "set_cost_bbox");
    metadata.set_cost_giou = manifest_optional_number(metadata_json, "set_cost_giou");
}

NativeCheckpoint load_checkpoint_from_manifest(const fs::path& manifest_path) {
    const json manifest = read_json_file(manifest_path);
    NativeCheckpoint checkpoint;
    if (const auto metadata_it = manifest.find("metadata"); metadata_it != manifest.end()) {
        if (!metadata_it->is_object()) {
            throw std::runtime_error("RF-DETR checkpoint manifest metadata is not an object: " +
                                     manifest_path.string());
        }
        populate_metadata_from_manifest(*metadata_it, checkpoint.metadata);
    }

    const auto found = manifest.find("state_dict");
    if (found == manifest.end() || !found->is_array()) {
        throw std::runtime_error("RF-DETR checkpoint manifest is missing state_dict array: " + manifest_path.string());
    }

    checkpoint.state_dict.reserve(found->size());
    for (const auto& entry_json : *found) {
        if (!entry_json.is_object()) {
            throw std::runtime_error("RF-DETR checkpoint manifest entry is not an object: " + manifest_path.string());
        }
        const auto name_it = entry_json.find("name");
        const auto tensor_it = entry_json.find("tensor_path");
        const auto dtype_it = entry_json.find("dtype");
        const auto sizes_it = entry_json.find("sizes");
        if (name_it == entry_json.end() || !name_it->is_string() || tensor_it == entry_json.end() ||
            !tensor_it->is_string() || dtype_it == entry_json.end() || !dtype_it->is_string() ||
            sizes_it == entry_json.end() || !sizes_it->is_array()) {
            throw std::runtime_error("RF-DETR checkpoint manifest entry is missing name/tensor_path/dtype/sizes");
        }

        const fs::path tensor_path = manifest_path.parent_path() / tensor_it->get<std::string>();
        std::vector<int64_t> sizes;
        sizes.reserve(sizes_it->size());
        for (const auto& size_json : *sizes_it) {
            if (!size_json.is_number_integer()) {
                throw std::runtime_error("RF-DETR checkpoint manifest tensor shape is not integral");
            }
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
        checkpoint.state_dict.push_back({
            name_it->get<std::string>(),
            detail::prepare_tensor_for_checkpoint_write(tensor),
        });
    }

    if (checkpoint.state_dict.empty()) {
        throw std::runtime_error("RF-DETR checkpoint manifest produced an empty state_dict: " + manifest_path.string());
    }
    return checkpoint;
}

json manifest_from_state_dict(const fs::path& root, const std::vector<StateDictEntry>& state_dict) {
    json manifest;
    manifest["state_dict"] = json::array();
    const fs::path tensor_dir = root / "tensors";
    fs::create_directories(tensor_dir);

    for (size_t index = 0; index < state_dict.size(); ++index) {
        const torch::Tensor prepared = detail::prepare_tensor_for_checkpoint_write(state_dict[index].tensor);
        const fs::path tensor_path = tensor_dir / tensor_entry_filename(index);
        write_raw_tensor_file(tensor_path, prepared);
        manifest["state_dict"].push_back({
            {"name", state_dict[index].name},
            {"tensor_path", fs::relative(tensor_path, root).string()},
            {"dtype", scalar_type_name(prepared.scalar_type())},
            {"sizes", prepared.sizes().vec()},
        });
    }
    return manifest;
}

}  // namespace

NativeCheckpoint load_upstream_python_checkpoint(const fs::path& checkpoint_path) {
    ScopedTempDirectory temp_dir("mmltk_rfdetr_load_");
    const fs::path manifest_path = temp_dir.path / "manifest.json";
    const fs::path tensor_dir = temp_dir.path / "tensors";
    fs::create_directories(tensor_dir);

    run_python_bridge("checkpoint export", {
                                               "export-upstream",
                                               "--input",
                                               detail::canonical_checkpoint_path_string(checkpoint_path),
                                               "--manifest",
                                               manifest_path.string(),
                                               "--tensor-dir",
                                               tensor_dir.string(),
                                           });

    return load_checkpoint_from_manifest(manifest_path);
}

std::vector<StateDictEntry> load_upstream_python_state_dict(const fs::path& checkpoint_path) {
    return load_upstream_python_checkpoint(checkpoint_path).state_dict;
}

void write_upstream_python_checkpoint(const fs::path& checkpoint_path, const std::vector<StateDictEntry>& state_dict) {
    if (state_dict.empty()) {
        throw std::runtime_error("RF-DETR upstream checkpoint state_dict must not be empty");
    }

    ScopedTempDirectory temp_dir("mmltk_rfdetr_save_");
    const fs::path manifest_path = temp_dir.path / "manifest.json";
    write_json_file(manifest_path, manifest_from_state_dict(temp_dir.path, state_dict));

    const fs::path absolute_output = detail::canonical_checkpoint_path_string(checkpoint_path);
    ensure_output_path_writable(absolute_output);
    try {
        run_python_bridge("checkpoint write", {
                                                  "write-upstream",
                                                  "--output",
                                                  absolute_output.string(),
                                                  "--manifest",
                                                  manifest_path.string(),
                                              });
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(error.what()) + "; check perms for " +
                                 absolute_output.parent_path().string());
    }
}

}  // namespace mmltk::rfdetr
