#include "vast_runtime.h"
#include "runtime_paths.h"
#include "subprocess_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace mmltk::gui {

namespace {

using json = nlohmann::json;

constexpr std::string_view kDefaultPythonExecutable = "python3";
constexpr std::string_view kDefaultBridgeScript = "utilities/vast_bridge.py";

enum class ChildSetupFailureStage : std::uint8_t {
    kDup2 = 0,
    kSetenv = 1,
    kExec = 2,
};

const char* child_setup_failure_stage_label(const ChildSetupFailureStage stage) {
    switch (stage) {
        case ChildSetupFailureStage::kDup2:
            return "dup2";
        case ChildSetupFailureStage::kSetenv:
            return "setenv";
        case ChildSetupFailureStage::kExec:
            return "execvp";
    }
    return "unknown";
}

std::string normalize_gpu_name(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char ch : value) {
        if (ch >= 'a' && ch <= 'z') {
            normalized.push_back(static_cast<char>(ch - ('a' - 'A')));
        } else if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            normalized.push_back(ch);
        }
    }
    return normalized;
}

bool has_prefix(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool matches_family(const RemoteGpuFamily family, std::string_view normalized_gpu_name) {
    switch (family) {
        case RemoteGpuFamily::A100:
            return normalized_gpu_name.find("A100") != std::string_view::npos;
        case RemoteGpuFamily::B200:
            return normalized_gpu_name.find("B200") != std::string_view::npos;
        case RemoteGpuFamily::H100:
            return normalized_gpu_name.find("H100") != std::string_view::npos;
        case RemoteGpuFamily::H200:
            return normalized_gpu_name.find("H200") != std::string_view::npos;
        case RemoteGpuFamily::LSeries:
            return has_prefix(normalized_gpu_name, "L4") || has_prefix(normalized_gpu_name, "L20") ||
                   has_prefix(normalized_gpu_name, "L40");
    }
    return false;
}

std::optional<RemoteGpuFamily> classify_family(std::string_view gpu_name) {
    const std::string normalized = normalize_gpu_name(gpu_name);
    for (const RemoteGpuFamily family : {RemoteGpuFamily::A100, RemoteGpuFamily::B200, RemoteGpuFamily::H100,
                                         RemoteGpuFamily::H200, RemoteGpuFamily::LSeries}) {
        if (matches_family(family, normalized)) {
            return family;
        }
    }
    return std::nullopt;
}

double json_number_or_default(const json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_number()) {
        return 0.0;
    }
    return found->get<double>();
}

int json_int_or_default(const json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_number_integer()) {
        if (found != value.end() && found->is_number()) {
            return static_cast<int>(std::lround(found->get<double>()));
        }
        return 0;
    }
    return found->get<int>();
}

std::string json_string_or_default(const json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string()) {
        return {};
    }
    return found->get<std::string>();
}

bool json_bool_or_default(const json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_boolean()) {
        return false;
    }
    return found->get<bool>();
}

std::string trim_copy(std::string_view value) {
    const std::size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(start, end - start + 1U));
}

json parse_json_payload(std::string_view payload, const std::string_view context) {
    const std::string trimmed = trim_copy(payload);
    if (trimmed.empty()) {
        throw std::runtime_error(std::string(context) + " is empty");
    }

    json parsed = json::parse(trimmed, nullptr, false);
    if (parsed.is_discarded()) {
        throw std::runtime_error(std::string(context) + " is not valid JSON");
    }
    return parsed;
}

std::vector<json> extract_offer_objects(const json& payload) {
    if (payload.is_array()) {
        return payload.get<std::vector<json>>();
    }
    if (payload.is_object()) {
        for (const char* key : {"offers", "rows", "results"}) {
            const auto found = payload.find(key);
            if (found != payload.end() && found->is_array()) {
                return found->get<std::vector<json>>();
            }
        }
    }
    throw std::runtime_error("Vast bridge returned an unexpected search-offers payload shape");
}

std::string build_vast_search_query(const int min_gpus) {
    return "rentable=True rented=False verified=True external=False num_gpus>=" + std::to_string(std::max(1, min_gpus));
}

std::vector<json> extract_instance_objects(const json& payload) {
    if (payload.is_array()) {
        return payload.get<std::vector<json>>();
    }
    if (payload.is_object()) {
        if (const auto found = payload.find("instances"); found != payload.end() && found->is_array()) {
            return found->get<std::vector<json>>();
        }
        return {payload};
    }
    throw std::runtime_error("Vast bridge returned an unexpected instance payload shape");
}

std::filesystem::path default_vast_python_executable() {
#ifdef MMLTK_GUI_PYTHON_EXECUTABLE
    return MMLTK_GUI_PYTHON_EXECUTABLE;
#else
    return std::filesystem::path{kDefaultPythonExecutable};
#endif
}

std::filesystem::path default_vast_bridge_script_path() {
#ifdef MMLTK_GUI_VAST_BRIDGE_SOURCE
    std::filesystem::path source_path = MMLTK_GUI_VAST_BRIDGE_SOURCE;
    if (std::filesystem::exists(source_path)) {
        return source_path;
    }
#endif
    return runtime_paths::python_asset_path("vast_bridge.py");
}

VastInstanceInfo vast_instance_from_json(const json& payload) {
    VastInstanceInfo instance;
    instance.instance_id = json_int_or_default(payload, "id");
    instance.machine_id = json_int_or_default(payload, "machine_id");
    instance.actual_status = json_string_or_default(payload, "actual_status");
    instance.current_state = json_string_or_default(payload, "cur_state");
    instance.next_state = json_string_or_default(payload, "next_state");
    instance.intended_status = json_string_or_default(payload, "intended_status");
    instance.image_uuid = json_string_or_default(payload, "image_uuid");
    instance.label = json_string_or_default(payload, "label");
    instance.num_gpus = json_int_or_default(payload, "num_gpus");
    instance.gpu_name = json_string_or_default(payload, "gpu_name");
    instance.ssh_host = json_string_or_default(payload, "ssh_host");
    instance.ssh_port = json_int_or_default(payload, "ssh_port");
    instance.public_ipaddr = json_string_or_default(payload, "public_ipaddr");
    instance.status_message = json_string_or_default(payload, "status_msg");
    instance.jupyter_token = json_string_or_default(payload, "jupyter_token");
    instance.ports = json_string_or_default(payload, "ports");
    instance.duration_seconds = json_number_or_default(payload, "duration");
    return instance;
}

void append_bridge_option(std::vector<std::string>& args, const std::string_view name, const std::string& value) {
    if (value.empty()) {
        return;
    }
    args.emplace_back(name);
    args.push_back(value);
}

void append_bridge_option(std::vector<std::string>& args, const std::string_view name,
                          const std::optional<double>& value) {
    if (!value.has_value()) {
        return;
    }
    args.emplace_back(name);
    args.push_back(std::to_string(*value));
}

void append_bridge_option(std::vector<std::string>& args, const std::string_view name, const bool enabled) {
    if (!enabled) {
        return;
    }
    args.emplace_back(name);
}

void require_vast_api_key(const VastBridgeConfig& config, const std::string_view context) {
    if (config.api_key.empty()) {
        throw std::runtime_error(std::string(context) + " requires --vast-api-key or VAST_API_KEY");
    }
}

std::string run_vast_bridge_command(const VastBridgeConfig& config, const std::vector<std::string>& bridge_args) {
    std::vector<std::string> argv_strings;
    argv_strings.reserve(bridge_args.size() + 2U);
    argv_strings.push_back(config.python_executable.empty() ? std::string{kDefaultPythonExecutable}
                                                            : config.python_executable.string());
    argv_strings.push_back(config.bridge_script_path.empty() ? std::string{kDefaultBridgeScript}
                                                             : config.bridge_script_path.string());
    argv_strings.insert(argv_strings.end(), bridge_args.begin(), bridge_args.end());

    std::vector<char*> argv;
    argv.reserve(argv_strings.size() + 1U);
    for (auto& item : argv_strings) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);

    const auto captured = subprocess::run_captured_child_process<ChildSetupFailureStage>(
        "Vast bridge", "failed to read Vast bridge output: ", [&](const int stdout_fd, const int setup_error_fd) {
            if (::dup2(stdout_fd, STDOUT_FILENO) < 0 || ::dup2(stdout_fd, STDERR_FILENO) < 0) {
                (void)subprocess::write_child_setup_failure(setup_error_fd, ChildSetupFailureStage::kDup2);
                std::_Exit(127);
            }
            ::close(stdout_fd);
            if (!config.api_key.empty()) {
                if (::setenv("VAST_API_KEY", config.api_key.c_str(), 1) != 0) {
                    (void)subprocess::write_child_setup_failure(setup_error_fd, ChildSetupFailureStage::kSetenv);
                    std::_Exit(127);
                }
            }
            ::execvp(argv.front(), argv.data());
            (void)subprocess::write_child_setup_failure(setup_error_fd, ChildSetupFailureStage::kExec);
            std::_Exit(127);
        });
    if (captured.setup_failure.has_value()) {
        throw std::runtime_error(subprocess::format_child_setup_failure(
            *captured.setup_failure, child_setup_failure_stage_label, "Vast bridge"));
    }
    if (!WIFEXITED(captured.status) || WEXITSTATUS(captured.status) != 0) {
        throw std::runtime_error(captured.output.empty() ? "Vast bridge failed" : captured.output);
    }
    return trim_copy(captured.output);
}

std::string normalize_log_output(std::string_view payload) {
    std::istringstream input{std::string(payload)};
    std::string output;
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with("waiting on logs for instance ")) {
            continue;
        }
        if (!output.empty()) {
            output.push_back('\n');
        }
        output += line;
    }
    return trim_copy(output);
}

void expect_vast_action_success(std::string_view payload, const std::string_view context) {
    const json parsed = parse_json_payload(payload, context);
    if (!parsed.is_object()) {
        throw std::runtime_error(std::string(context) + " must be a JSON object");
    }
    if (const auto success = parsed.find("success");
        success != parsed.end() && success->is_boolean() && !success->get<bool>()) {
        const std::string message = json_string_or_default(parsed, "msg");
        throw std::runtime_error(std::string(context) + (message.empty() ? " failed" : ": " + message));
    }
}

}  // namespace

const char* remote_gpu_family_label(const RemoteGpuFamily family) {
    switch (family) {
        case RemoteGpuFamily::A100:
            return "A100";
        case RemoteGpuFamily::B200:
            return "B200";
        case RemoteGpuFamily::H100:
            return "H100";
        case RemoteGpuFamily::H200:
            return "H200";
        case RemoteGpuFamily::LSeries:
            return "L Series";
    }
    return "Unknown";
}

std::string summarize_selected_remote_gpu_families(const std::vector<RemoteGpuFamily>& families) {
    return std::to_string(families.size()) + " families selected";
}

std::vector<VastRawOffer> parse_vast_offer_payload(std::string_view payload) {
    const json parsed = parse_json_payload(payload, "Vast offer payload");
    const std::vector<json> offer_objects = extract_offer_objects(parsed);
    std::vector<VastRawOffer> offers;
    offers.reserve(offer_objects.size());
    for (const json& item : offer_objects) {
        VastRawOffer offer;
        offer.offer_id = json_int_or_default(item, "id");
        offer.gpu_name = json_string_or_default(item, "gpu_name");
        offer.num_gpus = json_int_or_default(item, "num_gpus");
        offer.gpu_ram = json_number_or_default(item, "gpu_ram");
        offer.dph = json_number_or_default(item, "dph");
        offer.dlperf = json_number_or_default(item, "dlperf");
        offer.dlperf_usd = json_number_or_default(item, "dlperf_usd");
        offer.reliability = json_number_or_default(item, "reliability");
        offer.inet_down = json_number_or_default(item, "inet_down");
        offer.disk_space = json_number_or_default(item, "disk_space");
        offer.geolocation = json_string_or_default(item, "geolocation");
        offers.push_back(std::move(offer));
    }
    return offers;
}

VastLaunchTemplateOptions parse_vast_launch_template(std::string_view template_text) {
    VastLaunchTemplateOptions options;
    const std::string trimmed = trim_copy(template_text);
    if (trimmed.empty()) {
        return options;
    }
    if (!trimmed.starts_with('{')) {
        options.template_hash = trimmed;
        return options;
    }

    const json parsed = parse_json_payload(trimmed, "Vast launch template");
    if (!parsed.is_object()) {
        throw std::runtime_error("Vast launch template must be a JSON object");
    }

    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        const std::string& key = it.key();
        const json& value = *it;
        if (key == "bid_price" || key == "price") {
            if (!value.is_number()) {
                throw std::runtime_error("Vast launch template `" + key + "` must be numeric");
            }
            options.bid_price = value.get<double>();
            continue;
        }
        if (key == "disk") {
            if (!value.is_number()) {
                throw std::runtime_error("Vast launch template `disk` must be numeric");
            }
            options.disk = value.get<double>();
            continue;
        }
        if (key == "user") {
            if (!value.is_string()) {
                throw std::runtime_error("Vast launch template `user` must be a string");
            }
            options.user = value.get<std::string>();
            continue;
        }
        if (key == "login") {
            if (!value.is_string()) {
                throw std::runtime_error("Vast launch template `login` must be a string");
            }
            options.login = value.get<std::string>();
            continue;
        }
        if (key == "label") {
            if (!value.is_string()) {
                throw std::runtime_error("Vast launch template `label` must be a string");
            }
            options.label = value.get<std::string>();
            continue;
        }
        if (key == "onstart") {
            if (!value.is_string()) {
                throw std::runtime_error("Vast launch template `onstart` must be a string");
            }
            options.onstart = value.get<std::string>();
            continue;
        }
        if (key == "onstart_cmd") {
            if (!value.is_string()) {
                throw std::runtime_error("Vast launch template `onstart_cmd` must be a string");
            }
            options.onstart_cmd = value.get<std::string>();
            continue;
        }
        if (key == "entrypoint") {
            if (!value.is_string()) {
                throw std::runtime_error("Vast launch template `entrypoint` must be a string");
            }
            options.entrypoint = value.get<std::string>();
            continue;
        }
        if (key == "ssh") {
            if (!value.is_boolean()) {
                throw std::runtime_error("Vast launch template `ssh` must be a boolean");
            }
            options.ssh = value.get<bool>();
            continue;
        }
        if (key == "jupyter") {
            if (!value.is_boolean()) {
                throw std::runtime_error("Vast launch template `jupyter` must be a boolean");
            }
            options.jupyter = value.get<bool>();
            continue;
        }
        if (key == "direct") {
            if (!value.is_boolean()) {
                throw std::runtime_error("Vast launch template `direct` must be a boolean");
            }
            options.direct = value.get<bool>();
            continue;
        }
        if (key == "jupyter_dir") {
            if (!value.is_string()) {
                throw std::runtime_error("Vast launch template `jupyter_dir` must be a string");
            }
            options.jupyter_dir = value.get<std::string>();
            continue;
        }
        if (key == "jupyter_lab") {
            if (!value.is_boolean()) {
                throw std::runtime_error("Vast launch template `jupyter_lab` must be a boolean");
            }
            options.jupyter_lab = value.get<bool>();
            continue;
        }
        if (key == "lang_utf8") {
            if (!value.is_boolean()) {
                throw std::runtime_error("Vast launch template `lang_utf8` must be a boolean");
            }
            options.lang_utf8 = value.get<bool>();
            continue;
        }
        if (key == "python_utf8") {
            if (!value.is_boolean()) {
                throw std::runtime_error("Vast launch template `python_utf8` must be a boolean");
            }
            options.python_utf8 = value.get<bool>();
            continue;
        }
        if (key == "extra") {
            if (!value.is_string()) {
                throw std::runtime_error("Vast launch template `extra` must be a string");
            }
            options.extra = value.get<std::string>();
            continue;
        }
        if (key == "env") {
            if (!value.is_string()) {
                throw std::runtime_error("Vast launch template `env` must be a string");
            }
            options.env = value.get<std::string>();
            continue;
        }
        if (key == "args") {
            if (!value.is_array()) {
                throw std::runtime_error("Vast launch template `args` must be an array");
            }
            for (const json& arg : value) {
                if (!arg.is_string()) {
                    throw std::runtime_error("Vast launch template `args[]` entries must be strings");
                }
                options.args.push_back(arg.get<std::string>());
            }
            continue;
        }
        if (key == "force") {
            if (!value.is_boolean()) {
                throw std::runtime_error("Vast launch template `force` must be a boolean");
            }
            options.force = value.get<bool>();
            continue;
        }
        if (key == "cancel_unavail") {
            if (!value.is_boolean()) {
                throw std::runtime_error("Vast launch template `cancel_unavail` must be a boolean");
            }
            options.cancel_unavail = value.get<bool>();
            continue;
        }
        if (key == "template_hash") {
            if (!value.is_string()) {
                throw std::runtime_error("Vast launch template `template_hash` must be a string");
            }
            options.template_hash = value.get<std::string>();
            continue;
        }

        throw std::runtime_error("unsupported Vast launch template field: " + key);
    }
    return options;
}

VastCreateInstanceResult parse_vast_create_instance_payload(std::string_view payload) {
    const json parsed = parse_json_payload(payload, "Vast create-instance payload");
    if (!parsed.is_object()) {
        throw std::runtime_error("Vast create-instance payload must be a JSON object");
    }

    VastCreateInstanceResult result;
    result.success = json_bool_or_default(parsed, "success");
    result.offer_id = json_int_or_default(parsed, "ask_id");
    result.instance_id = json_int_or_default(parsed, "new_contract");
    result.instance_api_key = json_string_or_default(parsed, "instance_api_key");
    if (!result.success) {
        const std::string message = json_string_or_default(parsed, "msg");
        throw std::runtime_error(message.empty() ? "Vast create-instance request failed" : message);
    }
    if (result.instance_id <= 0) {
        throw std::runtime_error("Vast create-instance payload is missing `new_contract`");
    }
    return result;
}

VastInstanceInfo parse_vast_instance_payload(std::string_view payload) {
    const json parsed = parse_json_payload(payload, "Vast instance payload");
    if (!parsed.is_object()) {
        throw std::runtime_error("Vast instance payload must be a JSON object");
    }
    const VastInstanceInfo instance = vast_instance_from_json(parsed);
    if (instance.instance_id <= 0) {
        throw std::runtime_error("Vast instance payload is missing `id`");
    }
    return instance;
}

std::vector<VastInstanceInfo> parse_vast_instances_payload(std::string_view payload) {
    const json parsed = parse_json_payload(payload, "Vast instances payload");
    const std::vector<json> instance_objects = extract_instance_objects(parsed);
    std::vector<VastInstanceInfo> instances;
    instances.reserve(instance_objects.size());
    for (const json& item : instance_objects) {
        VastInstanceInfo instance = vast_instance_from_json(item);
        if (instance.instance_id > 0) {
            instances.push_back(std::move(instance));
        }
    }
    return instances;
}

std::vector<VastOfferSummary> rank_vast_offers(const std::vector<VastRawOffer>& offers,
                                               const std::vector<RemoteGpuFamily>& selected_families,
                                               const std::size_t result_limit, const int min_gpus) {
    std::vector<VastOfferSummary> filtered;
    filtered.reserve(offers.size());
    for (const VastRawOffer& offer : offers) {
        if (offer.num_gpus < min_gpus) {
            continue;
        }
        const auto family = classify_family(offer.gpu_name);
        if (!family.has_value()) {
            continue;
        }
        if (std::ranges::find(selected_families, *family) == selected_families.end()) {
            continue;
        }
        VastOfferSummary summary;
        static_cast<VastRawOffer&>(summary) = offer;
        summary.family = *family;
        filtered.push_back(std::move(summary));
    }

    std::ranges::sort(filtered, [](const VastOfferSummary& lhs, const VastOfferSummary& rhs) {
        if (lhs.dlperf_usd != rhs.dlperf_usd) {
            return lhs.dlperf_usd > rhs.dlperf_usd;
        }
        if (lhs.dlperf != rhs.dlperf) {
            return lhs.dlperf > rhs.dlperf;
        }
        if (lhs.dph != rhs.dph) {
            return lhs.dph < rhs.dph;
        }
        if (lhs.reliability != rhs.reliability) {
            return lhs.reliability > rhs.reliability;
        }
        return lhs.offer_id < rhs.offer_id;
    });

    if (filtered.size() > result_limit) {
        filtered.resize(result_limit);
    }
    return filtered;
}

VastBridgeConfig make_vast_bridge_config(const std::string_view api_key) {
    VastBridgeConfig config;
    config.python_executable = default_vast_python_executable();
    config.bridge_script_path = default_vast_bridge_script_path();
    config.api_key = std::string(api_key);
    return config;
}

VastCreateInstanceResult create_vast_instance(const VastBridgeConfig& config, const int offer_id,
                                              const std::string_view image, const VastLaunchTemplateOptions& options) {
    require_vast_api_key(config, "remote Vast instance creation");
    if (offer_id <= 0) {
        throw std::runtime_error("remote Vast instance creation requires a positive offer id");
    }
    if (image.empty()) {
        throw std::runtime_error("remote Vast instance creation requires a container image");
    }

    std::vector<std::string> bridge_args{
        "create-instance",
        std::to_string(offer_id),
        "--image",
        std::string(image),
    };
    append_bridge_option(bridge_args, "--bid-price", options.bid_price);
    append_bridge_option(bridge_args, "--disk", options.disk);
    append_bridge_option(bridge_args, "--user", options.user);
    append_bridge_option(bridge_args, "--login", options.login);
    append_bridge_option(bridge_args, "--label", options.label);
    append_bridge_option(bridge_args, "--onstart", options.onstart);
    append_bridge_option(bridge_args, "--onstart-cmd", options.onstart_cmd);
    append_bridge_option(bridge_args, "--entrypoint", options.entrypoint);
    append_bridge_option(bridge_args, "--ssh", options.ssh);
    append_bridge_option(bridge_args, "--jupyter", options.jupyter);
    append_bridge_option(bridge_args, "--direct", options.direct);
    append_bridge_option(bridge_args, "--jupyter-dir", options.jupyter_dir);
    append_bridge_option(bridge_args, "--jupyter-lab", options.jupyter_lab);
    append_bridge_option(bridge_args, "--lang-utf8", options.lang_utf8);
    append_bridge_option(bridge_args, "--python-utf8", options.python_utf8);
    append_bridge_option(bridge_args, "--extra", options.extra);
    append_bridge_option(bridge_args, "--env", options.env);
    append_bridge_option(bridge_args, "--force", options.force);
    append_bridge_option(bridge_args, "--cancel-unavail", options.cancel_unavail);
    append_bridge_option(bridge_args, "--template-hash", options.template_hash);
    if (!options.args.empty()) {
        bridge_args.emplace_back("--args");
        bridge_args.insert(bridge_args.end(), options.args.begin(), options.args.end());
    }

    return parse_vast_create_instance_payload(run_vast_bridge_command(config, bridge_args));
}

VastInstanceInfo show_vast_instance(const VastBridgeConfig& config, const int instance_id) {
    require_vast_api_key(config, "remote Vast instance lookup");
    if (instance_id <= 0) {
        throw std::runtime_error("remote Vast instance lookup requires a positive instance id");
    }
    return parse_vast_instance_payload(run_vast_bridge_command(config, {"show-instance", std::to_string(instance_id)}));
}

std::vector<VastInstanceInfo> show_vast_instances(const VastBridgeConfig& config) {
    require_vast_api_key(config, "remote Vast instance listing");
    return parse_vast_instances_payload(run_vast_bridge_command(config, {"show-instances"}));
}

std::string fetch_vast_instance_logs(const VastBridgeConfig& config, const int instance_id,
                                     const std::optional<std::size_t> tail_lines) {
    require_vast_api_key(config, "remote Vast log retrieval");
    if (instance_id <= 0) {
        throw std::runtime_error("remote Vast log retrieval requires a positive instance id");
    }

    std::vector<std::string> bridge_args{
        "logs",
        std::to_string(instance_id),
    };
    if (tail_lines.has_value()) {
        bridge_args.emplace_back("--tail");
        bridge_args.push_back(std::to_string(*tail_lines));
    }

    const json parsed = parse_json_payload(run_vast_bridge_command(config, bridge_args), "Vast logs payload");
    if (!parsed.is_object()) {
        throw std::runtime_error("Vast logs payload must be a JSON object");
    }
    return normalize_log_output(json_string_or_default(parsed, "logs"));
}

void start_vast_instance(const VastBridgeConfig& config, const int instance_id) {
    require_vast_api_key(config, "remote Vast instance start");
    if (instance_id <= 0) {
        throw std::runtime_error("remote Vast instance start requires a positive instance id");
    }
    expect_vast_action_success(run_vast_bridge_command(config, {"start-instance", std::to_string(instance_id)}),
                               "Vast start-instance payload");
}

void stop_vast_instance(const VastBridgeConfig& config, const int instance_id) {
    require_vast_api_key(config, "remote Vast instance stop");
    if (instance_id <= 0) {
        throw std::runtime_error("remote Vast instance stop requires a positive instance id");
    }
    expect_vast_action_success(run_vast_bridge_command(config, {"stop-instance", std::to_string(instance_id)}),
                               "Vast stop-instance payload");
}

void destroy_vast_instance(const VastBridgeConfig& config, const int instance_id) {
    require_vast_api_key(config, "remote Vast instance destroy");
    if (instance_id <= 0) {
        throw std::runtime_error("remote Vast instance destroy requires a positive instance id");
    }
    expect_vast_action_success(run_vast_bridge_command(config, {"destroy-instance", std::to_string(instance_id)}),
                               "Vast destroy-instance payload");
}

std::vector<VastOfferSummary> query_vast_offers(const VastQueryConfig& config,
                                                const std::vector<RemoteGpuFamily>& selected_families) {
    require_vast_api_key(config, "remote Vast query");
    if (selected_families.empty()) {
        throw std::runtime_error("remote Vast query requires at least one selected GPU family");
    }

    const std::string output =
        run_vast_bridge_command(config, {
                                            "search-offers",
                                            "--query",
                                            build_vast_search_query(config.min_gpus),
                                            "--limit",
                                            std::to_string(std::max<std::size_t>(32, config.result_limit * 16U)),
                                            "--order",
                                            "dlperf_usd-",
                                        });
    return rank_vast_offers(parse_vast_offer_payload(output), selected_families, config.result_limit, config.min_gpus);
}

}  // namespace mmltk::gui
