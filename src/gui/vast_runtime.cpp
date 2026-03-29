#include "vast_runtime.h"

#include <nlohmann/json.hpp>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace fastloader::gui {

namespace {

using json = nlohmann::json;

constexpr char kDefaultPythonExecutable[] = "python3";
constexpr char kDefaultBridgeScript[] = "utilities/vast_bridge.py";

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

bool matches_family(RemoteGpuFamily family, std::string_view normalized_gpu_name) {
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
        return normalized_gpu_name.rfind("L4", 0) == 0 ||
               normalized_gpu_name.rfind("L20", 0) == 0 ||
               normalized_gpu_name.rfind("L40", 0) == 0;
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

std::string build_vast_search_query(int min_gpus) {
    return "rentable=True rented=False verified=True external=False num_gpus>=" + std::to_string(std::max(1, min_gpus));
}

std::string read_child_stdout(const int fd) {
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t bytes_read = ::read(fd, buffer.data(), buffer.size());
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("failed to read Vast bridge output: ") + std::strerror(errno));
        }
        output.append(buffer.data(), static_cast<size_t>(bytes_read));
    }
    return output;
}

std::string run_vast_bridge(const std::filesystem::path& python_executable,
                            const std::filesystem::path& bridge_script_path,
                            const std::string& api_key,
                            const std::string& query,
                            std::size_t limit) {
    int stdout_pipe[2];
    if (::pipe(stdout_pipe) != 0) {
        throw std::runtime_error(std::string("failed to create Vast bridge pipe: ") + std::strerror(errno));
    }

    std::vector<std::string> argv_strings;
    argv_strings.push_back(python_executable.empty() ? kDefaultPythonExecutable : python_executable.string());
    argv_strings.push_back(bridge_script_path.empty() ? kDefaultBridgeScript : bridge_script_path.string());
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

    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        throw std::runtime_error(std::string("failed to fork Vast bridge: ") + std::strerror(errno));
    }
    if (child_pid == 0) {
        ::close(stdout_pipe[0]);
        if (::dup2(stdout_pipe[1], STDOUT_FILENO) < 0 || ::dup2(stdout_pipe[1], STDERR_FILENO) < 0) {
            std::fprintf(stderr, "dup2 failed for Vast bridge: %s\n", std::strerror(errno));
            std::_Exit(127);
        }
        ::close(stdout_pipe[1]);
        if (!api_key.empty()) {
            ::setenv("VAST_API_KEY", api_key.c_str(), 1);
        }
        ::execv(argv.front(), argv.data());
        std::fprintf(stderr, "execv failed for Vast bridge: %s\n", std::strerror(errno));
        std::_Exit(127);
    }

    ::close(stdout_pipe[1]);
    std::string output;
    try {
        output = read_child_stdout(stdout_pipe[0]);
    } catch (...) {
        ::close(stdout_pipe[0]);
        int status = 0;
        ::waitpid(child_pid, &status, 0);
        throw;
    }
    ::close(stdout_pipe[0]);

    int status = 0;
    if (::waitpid(child_pid, &status, 0) < 0) {
        throw std::runtime_error(std::string("failed to wait for Vast bridge: ") + std::strerror(errno));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error(output.empty() ? "Vast bridge failed" : output);
    }
    return output;
}

} // namespace

const char* remote_gpu_family_label(RemoteGpuFamily family) {
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
                                               std::size_t result_limit,
                                               int min_gpus) {
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

} // namespace fastloader::gui
