#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hunter {
namespace constants {

// ─── Version ───
constexpr const char* HUNTER_VERSION = "1.0.0";
constexpr const char* HUNTER_NAME = "Hunter C++";

// ─── Network Defaults ───
constexpr int DEFAULT_SOCKS_PORT = 10808;
constexpr int DEFAULT_GEMINI_PORT = 10809;
constexpr int DEFAULT_BENCHMARK_BASE_PORT = 11808;

// ─── Timeouts (milliseconds) ───
constexpr int HTTP_CONNECT_TIMEOUT_MS = 4000;
constexpr int HTTP_FETCH_TIMEOUT_MS = 8000;
constexpr int CURL_OVERALL_TIMEOUT_MS = 12000;
constexpr int SOCKS_CONNECT_TIMEOUT_MS = 2000;
constexpr int XRAY_START_TIMEOUT_MS = 5000;
constexpr int BENCHMARK_TIMEOUT_MS = 15000;
constexpr int TELEGRAM_CONNECT_TIMEOUT_MS = 20000;
constexpr int KEEPALIVE_INTERVAL_MS = 25000;

// ─── Worker Intervals (seconds) ───
constexpr int SCANNER_INTERVAL_S = 1800;       // 30 min
constexpr int PUBLISHER_INTERVAL_S = 1800;     // 30 min
constexpr int BALANCER_CHECK_INTERVAL_S = 60;  // 1 min
constexpr int HEALTH_MONITOR_INTERVAL_S = 30;  // 30 sec
constexpr int VALIDATOR_INTERVAL_S = 30;       // 30 sec
constexpr int HARVESTER_INTERVAL_S = 2700;     // 45 min
constexpr int GITHUB_BG_INTERVAL_S = 1800;     // 30 min
constexpr int IRAN_ASSETS_INTERVAL_S = 3600;   // 60 min
constexpr int DPI_PRESSURE_INTERVAL_S = 300;   // 5 min
constexpr int GITHUB_BG_INITIAL_DELAY_S = 5;
constexpr int HARVESTER_INITIAL_DELAY_S = 900;

// ─── Limits ───
constexpr int DEFAULT_MAX_CONFIGS = 1000;
constexpr int DEFAULT_MAX_WORKERS = 12;
constexpr int DEFAULT_SCAN_LIMIT = 50;
constexpr int DEFAULT_GITHUB_BG_CAP = 150000;
constexpr int DEFAULT_BG_VALIDATION_BATCH = 120;
constexpr int DEFAULT_TELEGRAM_PUBLISH_MAX_LINES = 50;
constexpr int CONFIG_DB_MAX_SIZE = 150000;
constexpr int BALANCER_MAX_BACKENDS = 20;

// ─── Config Tiers ───
constexpr int GOLD_LATENCY_MS = 2000;
constexpr int SILVER_LATENCY_MS = 5000;

// ─── Supported URI Schemes ───
inline const std::vector<std::string>& supportedSchemes() {
    static const std::vector<std::string> schemes = {
        "vmess", "vless", "trojan", "ss", "ssr",
        "hysteria2", "hy2", "tuic"
    };
    return schemes;
}

// ─── GitHub Config Sources ───
inline const std::vector<std::string>& githubRepos() {
    static const std::vector<std::string> repos = {
        "https://raw.githubusercontent.com/barry-far/V2ray-config/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/ebrasha/free-v2ray-public-list/refs/heads/main/all_extracted_configs.txt",
        "https://raw.githubusercontent.com/miladtahanian/V2RayCFGDumper/refs/heads/main/sub.txt",
        "https://raw.githubusercontent.com/Epodonios/v2ray-configs/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge.txt",
        "https://raw.githubusercontent.com/coldwater-10/V2ray-Config-Lite/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/all_sub.txt",
        "https://raw.githubusercontent.com/M-Mashreghi/Free-V2ray-Collector/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/NiREvil/vless/main/subscription.txt",
        "https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/sub.txt",
        "https://raw.githubusercontent.com/skywrt/v2ray-configs/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/longlon/v2ray-config/main/All_Configs_Sub.txt",
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/mix",
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/mix",
        "https://raw.githubusercontent.com/mfuu/v2ray/master/v2ray",
        "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
        "https://raw.githubusercontent.com/freefq/free/master/v2",
        "https://raw.githubusercontent.com/aiboboxx/v2rayfree/main/v2",
        "https://raw.githubusercontent.com/ermaozi/get_subscribe/main/subscribe/v2ray.txt",
        "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub",
    };
    return repos;
}

// ─── Anti-Censorship Sources ───
inline const std::vector<std::string>& antiCensorshipSources() {
    static const std::vector<std::string> sources = {
        "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge_base64.txt",
        "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub1.txt",
        "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub2.txt",
        "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub3.txt",
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/reality",
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/reality",
        "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/vless",
        "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/trojan",
        "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/vmess",
        "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/all.txt",
        "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
    };
    return sources;
}

// ─── Iran Priority Sources ───
inline const std::vector<std::string>& iranPrioritySources() {
    static const std::vector<std::string> sources = {
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/reality",
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/reality",
        "https://raw.githubusercontent.com/Surfboardv2ray/TGParse/main/reality.txt",
        "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/reality",
        "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/reality.txt",
        "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vless",
        "https://raw.githubusercontent.com/mahdibland/SSAggregator/master/sub/sub_merge.txt",
        "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
        "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub",
    };
    return sources;
}

// ─── Browser User Agents ───
inline const std::vector<std::string>& userAgents() {
    static const std::vector<std::string> agents = {
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:122.0) Gecko/20100101 Firefox/122.0",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.2 Safari/605.1.15",
    };
    return agents;
}

} // namespace constants
} // namespace hunter
