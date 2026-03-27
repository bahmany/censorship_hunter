#include "core/seed_data.h"
#include "core/utils.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace hunter {

// Default seed sources for initial setup and fallback
static const char* DEFAULT_SOURCES_SEED = 
"# Hunter Censorship - Default Seed Sources\n"
"# This file provides initial sources when no sources_manager.tsv exists\n"
"# Format: name\turl\tdescription\tcategory\tpriority\tenabled\n"
"#\n"
"# Primary GitHub sources with high reliability\n"
"Hunter Main\thttps://raw.githubusercontent.com/bahmany/censorship_hunter/main/configs.txt\tMain hunter configuration repository\tprimary\t1\t1\n"
"Iran Configs\thttps://raw.githubusercontent.com/mahdibland/ShadowsocksAggregator/master/all/iran.txt\tIran-specific configurations\tprimary\t2\t1\n"
"Free SSR\thttps://raw.githubusercontent.com/Alvin9999/pac_nodes/master/ssr.txt\tFree SSR configurations\tsecondary\t3\t1\n"
"#\n"
"# Community-maintained sources\n"
"V2Ray External\thttps://raw.githubusercontent.com/v2fly/v2ray-core/master/release/config/config.json\tV2Ray official configurations\tcommunity\t4\t1\n"
"Clash Configs\thttps://raw.githubusercontent.com/learnhard-cn/free_proxy_ss/main/clash/config.yaml\tClash proxy configurations\tcommunity\t5\t0\n"
"#\n"
"# Backup sources (disabled by default)\n"
"Old Archive\thttps://raw.githubusercontent.com/racskasdf/free-proxy-list/main/proxies.txt\tArchive of free proxies\tbackup\t6\t0\n"
;

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
    
    // Create seed sources file if it doesn't exist
    std::string sources_path = runtime_dir + "/sources_manager.tsv";
    if (!utils::fileExists(sources_path)) {
        std::ofstream sources_file(sources_path);
        if (sources_file.is_open()) {
            sources_file << DEFAULT_SOURCES_SEED;
            sources_file.close();
            created_sources = true;
            std::cout << "[SeedData] Created default sources seed: " << sources_path << std::endl;
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
    
    return success;
}

} // namespace hunter
