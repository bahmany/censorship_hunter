#include "http_client.h"
#include "core/utils.h"

#include <algorithm>
#include <future>
#include <thread>
#include <chrono>
#include <android/log.h>

#define LOG_TAG "HunterHTTP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace hunter {

const std::vector<std::string> GITHUB_REPOS = {
    "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/Epodonios/v2ray-configs/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge.txt",
    "https://raw.githubusercontent.com/coldwater-10/V2ray-Config-Lite/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/all_sub.txt",
    "https://raw.githubusercontent.com/M-Mashreghi/Free-V2ray-Collector/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/NiREvil/vless/main/subscription.txt",
    "https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/sub.txt",
    "https://raw.githubusercontent.com/skywrt/v2ray-configs/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/longlon/v2ray-config/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/ebrasha/free-v2ray-public-list/main/all_extracted_configs.txt",
    "https://raw.githubusercontent.com/hamed1124/port-based-v2ray-configs/main/all.txt",
    "https://raw.githubusercontent.com/mostafasadeghifar/v2ray-config/main/configs.txt",
    "https://raw.githubusercontent.com/Ashkan-m/v2ray/main/Sub.txt",
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_iOS.txt",
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_STARTER.txt",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/mix",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/mix",
    "https://raw.githubusercontent.com/mfuu/v2ray/master/v2ray",
    "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
    "https://raw.githubusercontent.com/freefq/free/master/v2",
    "https://raw.githubusercontent.com/aiboboxx/v2rayfree/main/v2",
    "https://raw.githubusercontent.com/ermaozi/get_subscribe/main/subscribe/v2ray.txt",
    "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub",
    "https://raw.githubusercontent.com/vveg26/get_proxy/main/dist/v2ray.txt",
};

const std::vector<std::string> ANTI_CENSORSHIP_SOURCES = {
    "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge_base64.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub1.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub2.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub3.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/reality",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/reality",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vmess",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vless",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/trojan",
    "https://raw.githubusercontent.com/Surfboardv2ray/TGParse/main/configtg.txt",
    "https://raw.githubusercontent.com/Surfboardv2ray/TGParse/main/reality.txt",
    "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/vless",
    "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/trojan",
    "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/vmess",
    "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/all.txt",
    "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
    "https://raw.githubusercontent.com/freefq/free/master/v2",
    "https://raw.githubusercontent.com/aiboboxx/v2rayfree/main/v2",
    "https://raw.githubusercontent.com/mfuu/v2ray/master/v2ray",
    "https://raw.githubusercontent.com/ermaozi/get_subscribe/main/subscribe/v2ray.txt",
    "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub",
    "https://raw.githubusercontent.com/Leon406/SubCrawler/master/sub/share/vless",
    "https://raw.githubusercontent.com/Leon406/SubCrawler/master/sub/share/ss",
};

const std::vector<std::string> IRAN_PRIORITY_SOURCES = {
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/reality",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/reality",
    "https://raw.githubusercontent.com/Surfboardv2ray/TGParse/main/reality.txt",
    "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/reality",
    "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/reality.txt",
    "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/vless.txt",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vless",
    "https://raw.githubusercontent.com/mahdibland/SSAggregator/master/sub/sub_merge.txt",
    "https://raw.githubusercontent.com/sarinaesmailzadeh/V2Hub/main/merged_base64",
    "https://raw.githubusercontent.com/LalatinaHub/Starter/main/Starter",
    "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
    "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub",
    "https://raw.githubusercontent.com/Leon406/SubCrawler/master/sub/share/vless",
};

const std::vector<std::string> NAPSTERV_SUBSCRIPTION_URLS = {
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_iOS.txt",
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/V2Ray.txt",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vmess",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vless",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/trojan",
};

// ---------- HTTPClientManager ----------

HTTPClientManager::HTTPClientManager() = default;

void HTTPClientManager::set_http_callback(HttpCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    http_callback_ = std::move(callback);
}

std::string HTTPClientManager::random_user_agent() const {
    return hunter::random_user_agent();
}

std::string HTTPClientManager::fetch_url(const std::string& url, int timeout,
                                          const std::string& proxy) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!http_callback_) {
        return "";
    }
    try {
        return http_callback_(url, hunter::random_user_agent(), timeout, proxy);
    } catch (...) {
        return "";
    }
}

// ---------- ConfigFetcher ----------

ConfigFetcher::ConfigFetcher(HTTPClientManager& http_manager)
    : http_manager_(http_manager) {}

std::set<std::string> ConfigFetcher::fetch_single_url(const std::string& url,
                                                        const std::vector<int>& proxy_ports,
                                                        int timeout) {
    // Try direct fetch first
    std::string response = http_manager_.fetch_url(url, std::min(timeout, 8));
    if (!response.empty()) {
        // Try base64 decoding
        if (response.find("://") == std::string::npos) {
            try {
                std::string decoded = safe_b64decode(trim(response));
                if (decoded.find("://") != std::string::npos) {
                    response = decoded;
                }
            } catch (...) {}
        }
        auto found = extract_raw_uris_from_text(response);
        if (!found.empty()) return found;
    }

    // Try with SOCKS proxies
    for (size_t i = 0; i < std::min(proxy_ports.size(), static_cast<size_t>(3)); ++i) {
        std::string proxy = "socks5://127.0.0.1:" + std::to_string(proxy_ports[i]);
        response = http_manager_.fetch_url(url, timeout, proxy);
        if (!response.empty()) {
            if (response.find("://") == std::string::npos) {
                try {
                    std::string decoded = safe_b64decode(trim(response));
                    if (decoded.find("://") != std::string::npos) {
                        response = decoded;
                    }
                } catch (...) {}
            }
            auto found = extract_raw_uris_from_text(response);
            if (!found.empty()) return found;
        }
    }

    return {};
}

std::set<std::string> ConfigFetcher::fetch_urls_parallel(
    const std::vector<std::string>& urls,
    const std::vector<int>& proxy_ports,
    int max_workers, int timeout, int global_timeout_sec) {

    std::set<std::string> configs;
    std::mutex configs_mutex;

    int workers = std::max(1, std::min(max_workers, static_cast<int>(urls.size())));
    std::vector<std::future<void>> futures;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(global_timeout_sec);

    size_t index = 0;
    std::mutex index_mutex;

    auto worker_fn = [&]() {
        while (true) {
            size_t my_index;
            {
                std::lock_guard<std::mutex> lock(index_mutex);
                if (index >= urls.size()) return;
                my_index = index++;
            }

            if (std::chrono::steady_clock::now() >= deadline) return;

            try {
                auto found = fetch_single_url(urls[my_index], proxy_ports, timeout);
                if (!found.empty()) {
                    std::lock_guard<std::mutex> lock(configs_mutex);
                    configs.insert(found.begin(), found.end());
                }
            } catch (...) {}
        }
    };

    for (int i = 0; i < workers; ++i) {
        futures.push_back(std::async(std::launch::async, worker_fn));
    }

    for (auto& f : futures) {
        try {
            auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining.count() > 0) {
                f.wait_for(remaining);
            }
        } catch (...) {}
    }

    return configs;
}

std::set<std::string> ConfigFetcher::fetch_github_configs(const std::vector<int>& proxy_ports,
                                                            int max_workers) {
    LOGI("Fetching from %zu GitHub sources...", GITHUB_REPOS.size());
    auto configs = fetch_urls_parallel(GITHUB_REPOS, proxy_ports, max_workers, 10, 90);
    LOGI("GitHub sources: %zu configs", configs.size());
    return configs;
}

std::set<std::string> ConfigFetcher::fetch_anti_censorship_configs(
    const std::vector<int>& proxy_ports, int max_workers) {
    LOGI("Fetching from %zu anti-censorship sources...", ANTI_CENSORSHIP_SOURCES.size());
    auto configs = fetch_urls_parallel(ANTI_CENSORSHIP_SOURCES, proxy_ports, max_workers, 15, 120);
    LOGI("Anti-censorship sources: %zu configs", configs.size());
    return configs;
}

std::set<std::string> ConfigFetcher::fetch_iran_priority_configs(
    const std::vector<int>& proxy_ports, int max_workers) {
    LOGI("Fetching from %zu Iran priority sources (Reality-focused)...", IRAN_PRIORITY_SOURCES.size());
    auto configs = fetch_urls_parallel(IRAN_PRIORITY_SOURCES, proxy_ports, max_workers, 20, 90);
    LOGI("Iran priority sources: %zu configs (Reality-focused)", configs.size());
    return configs;
}

std::set<std::string> ConfigFetcher::fetch_napsterv_configs(const std::vector<int>& proxy_ports) {
    return fetch_urls_parallel(NAPSTERV_SUBSCRIPTION_URLS, proxy_ports, 8, 12, 45);
}

} // namespace hunter
