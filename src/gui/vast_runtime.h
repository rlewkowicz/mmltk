#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::gui {

enum class RemoteGpuFamily : std::uint8_t {
    A100 = 0,
    B200 = 1,
    H100 = 2,
    H200 = 3,
    LSeries = 4,
};

struct VastRawOffer {
    int offer_id = 0;
    std::string gpu_name;
    int num_gpus = 0;
    double gpu_ram = 0.0;
    double dph = 0.0;
    double dlperf = 0.0;
    double dlperf_usd = 0.0;
    double reliability = 0.0;
    double inet_down = 0.0;
    double disk_space = 0.0;
    std::string geolocation;
};

struct VastOfferSummary : VastRawOffer {
    RemoteGpuFamily family = RemoteGpuFamily::A100;
};

struct VastQueryConfig {
    std::filesystem::path python_executable;
    std::filesystem::path bridge_script_path;
    std::string api_key;
    int min_gpus = 4;
    std::size_t result_limit = 2;
};

const char* remote_gpu_family_label(RemoteGpuFamily family);
std::string summarize_selected_remote_gpu_families(const std::vector<RemoteGpuFamily>& families);
std::vector<VastRawOffer> parse_vast_offer_payload(std::string_view payload);
std::vector<VastOfferSummary> rank_vast_offers(const std::vector<VastRawOffer>& offers,
                                               const std::vector<RemoteGpuFamily>& selected_families,
                                               std::size_t result_limit,
                                               int min_gpus);
std::vector<VastOfferSummary> query_vast_offers(const VastQueryConfig& config,
                                                const std::vector<RemoteGpuFamily>& selected_families);

} // namespace mmltk::gui
