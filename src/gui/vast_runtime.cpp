#include "vast_runtime.h"
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
    kExecv = 2,
};

const char* child_setup_failure_stage_label(const ChildSetupFailureStage stage) {
    switch (stage) {
    case ChildSetupFailureStage::kDup2:
        return "dup2";
    case ChildSetupFailureStage::kSetenv:
        return "setenv";
    case ChildSetupFailureStage::kExecv:
        return "execv";
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
        return has_prefix(normalized_gpu_name, "L4") ||
               has_prefix(normalized_gpu_name, "L20") ||
               has_prefix(normalized_gpu_name, "L40");
    }
    return false;
}

std::optional<RemoteGpuFamily> classify_family(std::string_view gpu_name) {
    const std::string normalized = normalize_gpu_name(gpu_name);
    for (const RemoteGpuFamily family : {RemoteGpuFamily::A100,
                                         RemoteGpuFamily::B200,
                                         RemoteGpuFamily::H100,
                                         RemoteGpuFamily::H200,
                                         RemoteGpuFamily::LSeries}) {
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
    return "rentable=True rented=False verified=True external=False num_gpus>=" +
           std::to_string(std::max(1, min_gpus));
}

std::string run_vast_bridge(const std::filesystem::path& python_executable,
                            const std::filesystem::path& bridge_script_path,
                            const std::string& api_key,
                            const std::string& query,
                            const std::size_t limit) {
    std::vector<std::string> argv_strings;
    argv_strings.push_back(python_executable.empty() ? std::string{kDefaultPythonExecutable}
                                                     : python_executable.string());
    argv_strings.push_back(bridge_script_path.empty() ? std::string{kDefaultBridgeScript}
                                                      : bridge_script_path.string());
    argv_strings.emplace_back("search-offers");
    argv_strings.emplace_back("--query");
    argv_strings.push_back(query);
    argv_strings.emplace_back("--limit");
    argv_strings.push_back(std::to_string(limit));
    argv_strings.emplace_back("--order");
    argv_strings.emplace_back("dlperf_usd-");

    std::vector<char*> argv;
    argv.reserve(argv_strings.size() + 1U);
    for (auto& item : argv_strings) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);

    const auto captured = subprocess::run_captured_child_process<ChildSetupFailureStage>(
        "Vast bridge",
        "failed to read Vast bridge output: ",
        [&](const int stdout_fd, const int setup_error_fd) {
            if (::dup2(stdout_fd, STDOUT_FILENO) < 0 || ::dup2(stdout_fd, STDERR_FILENO) < 0) {
                (void)subprocess::write_child_setup_failure(setup_error_fd, ChildSetupFailureStage::kDup2);
                std::_Exit(127);
            }
            ::close(stdout_fd);
            if (!api_key.empty()) {
                if (::setenv("VAST_API_KEY", api_key.c_str(), 1) != 0) {
                    (void)subprocess::write_child_setup_failure(setup_error_fd, ChildSetupFailureStage::kSetenv);
                    std::_Exit(127);
                }
            }
            ::execv(argv.front(), argv.data());
            (void)subprocess::write_child_setup_failure(setup_error_fd, ChildSetupFailureStage::kExecv);
            std::_Exit(127);
        });
    if (captured.setup_failure.has_value()) {
        throw std::runtime_error(subprocess::format_child_setup_failure(
            *captured.setup_failure,
            child_setup_failure_stage_label,
            "Vast bridge"));
    }
    if (!WIFEXITED(captured.status) || WEXITSTATUS(captured.status) != 0) {
        throw std::runtime_error(captured.output.empty() ? "Vast bridge failed" : captured.output);
    }
    return captured.output;
}

} // namespace

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
    const json parsed = json::parse(payload);
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

std::vector<VastOfferSummary> rank_vast_offers(const std::vector<VastRawOffer>& offers,
                                               const std::vector<RemoteGpuFamily>& selected_families,
                                               const std::size_t result_limit,
                                               const int min_gpus) {
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
        if (std::find(selected_families.begin(), selected_families.end(), *family) == selected_families.end()) {
            continue;
        }
        VastOfferSummary summary;
        static_cast<VastRawOffer&>(summary) = offer;
        summary.family = *family;
        filtered.push_back(std::move(summary));
    }

    std::sort(filtered.begin(), filtered.end(), [](const VastOfferSummary& lhs, const VastOfferSummary& rhs) {
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

std::vector<VastOfferSummary> query_vast_offers(const VastQueryConfig& config,
                                                const std::vector<RemoteGpuFamily>& selected_families) {
    if (config.api_key.empty()) {
        throw std::runtime_error("remote Vast query requires --vast-api-key or VAST_API_KEY");
    }
    if (selected_families.empty()) {
        throw std::runtime_error("remote Vast query requires at least one selected GPU family");
    }

    const std::string output = run_vast_bridge(config.python_executable,
                                               config.bridge_script_path,
                                               config.api_key,
                                               build_vast_search_query(config.min_gpus),
                                               std::max<std::size_t>(32, config.result_limit * 16U));
    return rank_vast_offers(parse_vast_offer_payload(output),
                            selected_families,
                            config.result_limit,
                            config.min_gpus);
}

} // namespace mmltk::gui
