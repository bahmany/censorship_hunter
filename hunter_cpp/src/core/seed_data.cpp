#include "core/seed_data.h"
#include "core/utils.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace hunter {

// Default seed sources for initial setup and fallback
static const char* DEFAULT_SOURCES_SEED =
"# enabled\tpriority\tcategory\tadded_ts\tlast_success_ts\ttotal_configs_found\tsuccess_rate\tname\tdescription\turl\n"
"1\t1\tpremium\t0\t0\t0\t0\tBarry Far Configs\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/barry-far/V2ray-config/main/All_Configs_Sub.txt\n"
"1\t1\tpremium\t0\t0\t0\t0\tEbrasha Free List\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/ebrasha/free-v2ray-public-list/refs/heads/main/all_extracted_configs.txt\n"
"1\t1\tpremium\t0\t0\t0\t0\tMilad Tahanian Dumper\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/miladtahanian/V2RayCFGDumper/refs/heads/main/sub.txt\n"
"1\t1\tpremium\t0\t0\t0\t0\tEpodonios Configs\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/Epodonios/v2ray-configs/main/All_Configs_Sub.txt\n"
"1\t1\tpremium\t0\t0\t0\t0\tMahdi Bland Aggregator\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge.txt\n"
"1\t0\tprimary\t0\t0\t0\t0\tColdwater Lite\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/coldwater-10/V2ray-Config-Lite/main/All_Configs_Sub.txt\n"
"1\t0\tprimary\t0\t0\t0\t0\tMatin Ghanbari\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/all_sub.txt\n"
"1\t0\tprimary\t0\t0\t0\t0\tMashreghi Collector\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/M-Mashreghi/Free-V2ray-Collector/main/All_Configs_Sub.txt\n"
"1\t0\tprimary\t0\t0\t0\t0\tNiREvil VLESS\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/NiREvil/vless/main/subscription.txt\n"
"1\t0\tprimary\t0\t0\t0\t0\tALIILA V2rayNG\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/sub.txt\n"
"1\t0\tprimary\t0\t0\t0\t0\tSkyWRT Configs\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/skywrt/v2ray-configs/main/All_Configs_Sub.txt\n"
"1\t0\tprimary\t0\t0\t0\t0\tLonglon Configs\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/longlon/v2ray-config/main/All_Configs_Sub.txt\n"
"1\t0\tprimary\t0\t0\t0\t0\tYebekhe Normal\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/mix\n"
"1\t0\tprimary\t0\t0\t0\t0\tYebekhe Base64\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/mix\n"
"1\t0\tprimary\t0\t0\t0\t0\tMFUU V2ray\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/mfuu/v2ray/master/v2ray\n"
"1\t0\tprimary\t0\t0\t0\t0\tPeasoft NoMoreWalls\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt\n"
"1\t0\tprimary\t0\t0\t0\t0\tFreeFQ\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/freefq/free/master/v2\n"
"1\t0\tprimary\t0\t0\t0\t0\tAiboboxx Free\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/aiboboxx/v2rayfree/main/v2\n"
"1\t0\tprimary\t0\t0\t0\t0\tErmaozi Subscribe\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/ermaozi/get_subscribe/main/subscribe/v2ray.txt\n"
"1\t0\tprimary\t0\t0\t0\t0\tPawdroid Free Servers\tHigh-quality V2ray configuration source\thttps://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub\n";

// Default seed configs for initial testing
static const char* DEFAULT_CONFIGS_SEED = 
"# Hunter Censorship - Default Seed Configs\n"
"# Basic working configurations for testing connectivity\n"
"# These are minimal configs to verify the system works\n"
"#\n"
"# Example vmess config (replace with real working configs)\n"
"vmess://eyJhZGQiOiJleGFtcGxlLmNvbSIsImFpZCI6IjAiLCJob3N0IjoiIiwiaWQiOiJleGFtcGxlLWlkIiwibmV0Ijoid3MiLCJwYXRoIjoiLyIsInBvcnQiOjQ0MywicHMiOiJleGFtcGxlIiwic2N5IjoiYXV0byIsInRscyI6IiIsInR5cGUiOiIvIn0=\n"
"#\n"
"# Example vless config (replace with real working configs)\n"
"vless://example-id@example.com:443?encryption=none&security=tls&type=ws&host=example.com&path=%2f#example-vless\n"
"#\n"
"# Example trojan config (replace with real working configs)\n"
"trojan://example-password@example.com:443?security=tls&type=tcp&headerType=none#example-trojan\n"
"#\n"
"# Example ss config (replace with real working configs)\n"
"ss://YWVzLTI1Ni1nY206ZXhhbXBsZS1wYXNzd29yZA@example.com:8388#example-ss\n"
;

bool SeedData::ensureSeedData(const std::string& runtime_dir) {
    utils::mkdirRecursive(runtime_dir);
    
    bool created_sources = false;
    bool created_configs = false;
    
    // Create/repair seed sources file if missing or invalid
    std::string sources_path = runtime_dir + "/sources_manager.tsv";
    bool needs_sources_reset = !utils::fileExists(sources_path);
    if (!needs_sources_reset) {
        std::ifstream check(sources_path);
        std::string line;
        bool has_valid_url = false;
        while (std::getline(check, line)) {
            line = utils::trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line.find("http://") != std::string::npos || line.find("https://") != std::string::npos) {
                has_valid_url = true;
                break;
            }
        }
        needs_sources_reset = !has_valid_url;
    }
    if (needs_sources_reset) {
        std::ofstream sources_file(sources_path);
        if (sources_file.is_open()) {
            sources_file << DEFAULT_SOURCES_SEED;
            sources_file.close();
            created_sources = true;
            std::cout << "[SeedData] Wrote default sources seed: " << sources_path << std::endl;
        } else {
            std::cerr << "[SeedData] Failed to create sources seed file: " << sources_path << std::endl;
            return false;
        }
    }
    
    // Create seed configs file if it doesn't exist
    std::string configs_path = runtime_dir + "/seed_configs.txt";
    if (!utils::fileExists(configs_path)) {
        std::ofstream configs_file(configs_path);
        if (configs_file.is_open()) {
            configs_file << DEFAULT_CONFIGS_SEED;
            configs_file.close();
            created_configs = true;
            std::cout << "[SeedData] Created default configs seed: " << configs_path << std::endl;
        } else {
            std::cerr << "[SeedData] Failed to create configs seed file: " << configs_path << std::endl;
            return false;
        }
    }
    
    // Create seed history file if it doesn't exist
    std::string history_path = runtime_dir + "/source_history.tsv";
    if (!utils::fileExists(history_path)) {
        std::ofstream history_file(history_path);
        if (history_file.is_open()) {
            history_file << "# Hunter Censorship - Source Download History\n"
                        << "# Format: url\tlast_download_ts\tlast_success\ttotal_downloads\tsuccessful_downloads\tconfigs_found\tunique_configs\tlast_error\n"
                        << "#\n";
            history_file.close();
            std::cout << "[SeedData] Created source history seed: " << history_path << std::endl;
        } else {
            std::cerr << "[SeedData] Failed to create history seed file: " << history_path << std::endl;
            return false;
        }
    }
    
    return created_sources || created_configs;
}

std::vector<std::string> SeedData::loadSeedConfigs(const std::string& runtime_dir) {
    std::vector<std::string> configs;
    std::string configs_path = runtime_dir + "/seed_configs.txt";
    
    std::ifstream file(configs_path);
    if (!file.is_open()) {
        std::cerr << "[SeedData] Failed to open seed configs: " << configs_path << std::endl;
        return configs;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        line = utils::trim(line);
        if (line.empty() || line[0] == '#') continue;
        
        // Basic validation for config formats
        if (line.find("vmess://") == 0 || 
            line.find("vless://") == 0 || 
            line.find("trojan://") == 0 ||
            line.find("ss://") == 0) {
            configs.push_back(line);
        }
    }
    
    file.close();
    std::cout << "[SeedData] Loaded " << configs.size() << " seed configs" << std::endl;
    return configs;
}

bool SeedData::exportForPackaging(const std::string& runtime_dir, const std::string& export_dir) {
    utils::mkdirRecursive(export_dir);
    
    bool success = true;
    
    // Copy sources manager
    std::string sources_src = runtime_dir + "/sources_manager.tsv";
    std::string sources_dst = export_dir + "/sources_manager.tsv";
    if (utils::fileExists(sources_src)) {
        std::ifstream src(sources_src, std::ios::binary);
        std::ofstream dst(sources_dst, std::ios::binary);
        if (src && dst) {
            dst << src.rdbuf();
            std::cout << "[SeedData] Exported sources manager for packaging" << std::endl;
        } else {
            std::cerr << "[SeedData] Failed to export sources manager" << std::endl;
            success = false;
        }
    }
    
    // Copy source history
    std::string history_src = runtime_dir + "/source_history.tsv";
    std::string history_dst = export_dir + "/source_history.tsv";
    if (utils::fileExists(history_src)) {
        std::ifstream src(history_src, std::ios::binary);
        std::ofstream dst(history_dst, std::ios::binary);
        if (src && dst) {
            dst << src.rdbuf();
            std::cout << "[SeedData] Exported source history for packaging" << std::endl;
        } else {
            std::cerr << "[SeedData] Failed to export source history" << std::endl;
            success = false;
        }
    }
    
    // Export current working configs if available
    std::string configs_src = runtime_dir + "/HUNTER_config_db_export.txt";
    std::string configs_dst = export_dir + "/seed_configs.txt";
    if (utils::fileExists(configs_src)) {
        std::ifstream src(configs_src, std::ios::binary);
        std::ofstream dst(configs_dst, std::ios::binary);
        if (src && dst) {
            dst << src.rdbuf();
            std::cout << "[SeedData] Exported working configs as seed" << std::endl;
        } else {
            std::cerr << "[SeedData] Failed to export working configs" << std::endl;
            success = false;
        }
    }

    // Copy full config DB snapshot for offline setup bundles
    std::string db_src = runtime_dir + "/HUNTER_config_db.tsv";
    std::string db_dst = export_dir + "/HUNTER_config_db.tsv";
    if (utils::fileExists(db_src)) {
        std::ifstream src(db_src, std::ios::binary);
        std::ofstream dst(db_dst, std::ios::binary);
        if (src && dst) {
            dst << src.rdbuf();
            std::cout << "[SeedData] Exported runtime DB snapshot for packaging" << std::endl;
        } else {
            std::cerr << "[SeedData] Failed to export runtime DB snapshot" << std::endl;
            success = false;
        }
    }
    
    return success;
}

} // namespace hunter
