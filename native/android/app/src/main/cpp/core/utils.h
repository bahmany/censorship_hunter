#pragma once

#include <string>
#include <vector>
#include <set>
#include <nlohmann/json.hpp>

namespace hunter {

extern const std::vector<std::string> BROWSER_USER_AGENTS;
extern const std::vector<std::string> CDN_WHITELIST_DOMAINS;
extern const std::vector<int> WHITELIST_PORTS;
extern const std::vector<std::string> ANTI_DPI_INDICATORS;
extern const std::vector<std::string> DPI_EVASION_FINGERPRINTS;
extern const std::vector<std::string> IRAN_BLOCKED_PATTERNS;

std::string base64_encode(const std::string& data);
std::string safe_b64decode(const std::string& data);
std::string clean_ps_string(const std::string& ps);
std::string to_lower(const std::string& s);
std::string trim(const std::string& s);
std::string url_decode(const std::string& encoded);
int64_t now_ts();
std::string tier_for_latency(double latency_ms);
std::string get_region(const std::string& country_code);
std::vector<std::string> read_lines(const std::string& path);
int write_lines(const std::string& filepath, const std::vector<std::string>& lines);
int append_unique_lines(const std::string& path, const std::vector<std::string>& lines);
nlohmann::json load_json(const std::string& path, const nlohmann::json& default_val);
void save_json(const std::string& path, const nlohmann::json& data);
std::set<std::string> extract_raw_uris_from_text(const std::string& text);
bool is_cdn_based(const std::string& uri);
int has_anti_dpi_features(const std::string& uri);
bool is_likely_blocked(const std::string& uri);
bool is_ipv4_preferred(const std::string& uri);
std::vector<std::string> prioritize_configs(const std::vector<std::string>& uris);
std::string random_user_agent();
int random_int(int min, int max);
bool ensure_directory(const std::string& path);

} // namespace hunter
