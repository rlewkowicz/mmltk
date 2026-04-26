#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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

struct VastBridgeConfig {
    std::filesystem::path python_executable;
    std::filesystem::path bridge_script_path;
    std::string api_key;
};

struct VastInstanceStateFields {
    int instance_id = 0;
    std::string actual_status;
    std::string current_state;
    std::string next_state;
    std::string intended_status;
    std::string image_uuid;
    std::string label;
    int num_gpus = 0;
    std::string gpu_name;
    std::string ssh_host;
    int ssh_port = 0;
    std::string public_ipaddr;
    std::string status_message;
    std::string jupyter_token;
    std::string ports;
    double duration_seconds = 0.0;
};

struct VastQueryConfig : VastBridgeConfig {
    int min_gpus = 4;
    std::size_t result_limit = 2;
};

struct VastLaunchTemplateOptions {
    std::optional<double> bid_price;
    std::optional<double> disk;
    std::string user;
    std::string login;
    std::string label;
    std::string onstart;
    std::string onstart_cmd;
    std::string entrypoint;
    bool ssh = false;
    bool jupyter = false;
    bool direct = false;
    std::string jupyter_dir;
    bool jupyter_lab = false;
    bool lang_utf8 = false;
    bool python_utf8 = false;
    std::string extra;
    std::string env;
    std::vector<std::string> args;
    bool force = false;
    bool cancel_unavail = false;
    std::string template_hash;
};

struct VastCreateInstanceResult {
    bool success = false;
    int offer_id = 0;
    int instance_id = 0;
    std::string instance_api_key;
};

struct VastInstanceInfo : VastInstanceStateFields {
    int machine_id = 0;
};

VastBridgeConfig make_vast_bridge_config(std::string_view api_key);
const char* remote_gpu_family_label(RemoteGpuFamily family);
std::string summarize_selected_remote_gpu_families(const std::vector<RemoteGpuFamily>& families);
std::vector<VastRawOffer> parse_vast_offer_payload(std::string_view payload);
VastLaunchTemplateOptions parse_vast_launch_template(std::string_view template_text);
VastCreateInstanceResult parse_vast_create_instance_payload(std::string_view payload);
VastInstanceInfo parse_vast_instance_payload(std::string_view payload);
std::vector<VastInstanceInfo> parse_vast_instances_payload(std::string_view payload);
std::vector<VastOfferSummary> rank_vast_offers(const std::vector<VastRawOffer>& offers,
                                               const std::vector<RemoteGpuFamily>& selected_families,
                                               std::size_t result_limit, int min_gpus);
VastCreateInstanceResult create_vast_instance(const VastBridgeConfig& config, int offer_id, std::string_view image,
                                              const VastLaunchTemplateOptions& options = {});
VastInstanceInfo show_vast_instance(const VastBridgeConfig& config, int instance_id);
std::vector<VastInstanceInfo> show_vast_instances(const VastBridgeConfig& config);
std::string fetch_vast_instance_logs(const VastBridgeConfig& config, int instance_id,
                                     std::optional<std::size_t> tail_lines = std::nullopt);
void start_vast_instance(const VastBridgeConfig& config, int instance_id);
void stop_vast_instance(const VastBridgeConfig& config, int instance_id);
void destroy_vast_instance(const VastBridgeConfig& config, int instance_id);
std::vector<VastOfferSummary> query_vast_offers(const VastQueryConfig& config,
                                                const std::vector<RemoteGpuFamily>& selected_families);

}  // namespace mmltk::gui
