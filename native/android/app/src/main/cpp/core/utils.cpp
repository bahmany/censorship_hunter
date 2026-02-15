#include "utils.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <sys/stat.h>

#include <android/log.h>

#define LOG_TAG "HunterUtils"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace hunter {

const std::vector<std::string> BROWSER_USER_AGENTS = {
    "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36",
    "Mozilla/5.0 (Linux; Android 14; SM-S918B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36",
    "Mozilla/5.0 (Linux; Android 12; SM-G991B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36",
};

const std::vector<std::string> CDN_WHITELIST_DOMAINS = {
    "cloudflare.com", "cdn.cloudflare.com", "cloudflare-dns.com",
    "fastly.net", "fastly.com", "global.fastly.net",
    "akamai.net", "akamaiedge.net", "akamaihd.net",
    "azureedge.net", "azure.com", "microsoft.com",
    "amazonaws.com", "cloudfront.net", "awsglobalaccelerator.com",
    "googleusercontent.com", "googleapis.com", "gstatic.com",
    "edgecastcdn.net", "stackpathdns.com",
    "cdn77.org", "cdnjs.cloudflare.com",
    "jsdelivr.net", "unpkg.com",
    "workers.dev", "pages.dev",
    "vercel.app", "netlify.app",
    "arvancloud.ir", "arvancloud.com", "r2.dev",
    "arvan.run", "arvanstorage.ir", "arvancdn.ir",
    "arvancdn.com", "cdn.arvancloud.ir",
};

const std::vector<int> WHITELIST_PORTS = {443, 8443, 2053, 2083, 2087, 2096, 80, 8080};

const std::vector<std::string> ANTI_DPI_INDICATORS = {
    "reality", "pbk=",
    "grpc", "gun",
    "h2", "http/2",
    "ws", "websocket",
    "splithttp", "httpupgrade",
    "quic", "kcp",
    "fp=chrome", "fp=firefox", "fp=safari", "fp=edge",
    "alpn=h2", "alpn=http",
};

const std::vector<std::string> DPI_EVASION_FINGERPRINTS = {
    "chrome", "firefox", "safari", "edge", "ios", "android", "random", "randomized"
};

const std::vector<std::string> IRAN_BLOCKED_PATTERNS = {
    "ir.", ".ir", "iran",
    "0.0.0.0", "127.0.0.1", "localhost",
    "10.10.34.", "192.168.",
};

// ---------- Base64 ----------

static const std::string B64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& data) {
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : data) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(B64_CHARS[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(B64_CHARS[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string safe_b64decode(const std::string& data) {
    // Add padding
    std::string input = data;
    while (input.size() % 4 != 0) {
        input += '=';
    }

    // Handle URL-safe base64
    for (auto& c : input) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }

    static const int T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (c == '=' || c == '\n' || c == '\r') break;
        int d = T[c];
        if (d == -1) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// ---------- String utilities ----------

std::string clean_ps_string(const std::string& ps) {
    std::string result;
    for (char c : ps) {
        if (static_cast<unsigned char>(c) <= 0x7F && c >= 0x20) {
            result += c;
        }
    }
    result = trim(result);
    return result.empty() ? "Unknown" : result;
}

std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string url_decode(const std::string& encoded) {
    std::string result;
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            int hex = 0;
            std::istringstream iss(encoded.substr(i + 1, 2));
            if (iss >> std::hex >> hex) {
                result += static_cast<char>(hex);
                i += 2;
            } else {
                result += encoded[i];
            }
        } else if (encoded[i] == '+') {
            result += ' ';
        } else {
            result += encoded[i];
        }
    }
    return result;
}

// ---------- Timestamp ----------

int64_t now_ts() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ---------- Tier classification ----------

std::string tier_for_latency(double latency_ms) {
    if (latency_ms < 200.0) return "gold";
    if (latency_ms < 800.0) return "silver";
    if (latency_ms > 2000.0) return "dead";
    return "silver";
}

// ---------- Region ----------

std::string get_region(const std::string& country_code) {
    static const std::set<std::string> european = {
        "AL","AD","AT","BY","BE","BA","BG","HR","CY","CZ","DK","EE",
        "FO","FI","FR","DE","GI","GR","HU","IS","IE","IT","XK","LV",
        "LI","LT","LU","MK","MT","MD","MC","ME","NL","NO","PL","PT",
        "RO","RU","SM","RS","SK","SI","ES","SE","CH","UA","GB","VA"
    };
    static const std::set<std::string> asian = {
        "AF","AM","AZ","BH","BD","BT","BN","KH","CN","GE","HK","IN",
        "ID","IR","IQ","IL","JP","JO","KZ","KW","KG","LA","LB","MO",
        "MY","MV","MN","MM","NP","KP","OM","PK","PS","PH","QA","SA",
        "SG","KR","LK","SY","TW","TJ","TH","TL","TR","TM","AE","UZ",
        "VN","YE"
    };
    static const std::set<std::string> african = {
        "DZ","AO","BJ","BW","BF","BI","CV","CM","CF","TD","KM","CD",
        "CG","DJ","EG","GQ","ER","SZ","ET","GA","GM","GH","GN","GW",
        "CI","KE","LS","LR","LY","MG","MW","ML","MR","MU","YT","MA",
        "MZ","NA","NE","NG","RE","RW","SH","ST","SN","SC","SL","SO",
        "ZA","SS","SD","TZ","TG","TN","UG","EH","ZM","ZW"
    };

    if (country_code == "US") return "USA";
    if (country_code == "CA") return "Canada";
    if (european.count(country_code)) return "Europe";
    if (asian.count(country_code)) return "Asia";
    if (african.count(country_code)) return "Africa";
    return "Other";
}

// ---------- File I/O ----------

std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream file(path);
    if (!file.is_open()) return lines;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

int write_lines(const std::string& filepath, const std::vector<std::string>& lines) {
    std::ofstream file(filepath);
    if (!file.is_open()) return 0;
    int count = 0;
    for (const auto& line : lines) {
        if (!line.empty()) {
            file << line << "\n";
            ++count;
        }
    }
    return count;
}

int append_unique_lines(const std::string& path, const std::vector<std::string>& lines) {
    std::set<std::string> existing;
    for (const auto& l : read_lines(path)) {
        existing.insert(l);
    }
    std::vector<std::string> new_lines;
    for (const auto& line : lines) {
        if (!line.empty() && existing.find(line) == existing.end()) {
            new_lines.push_back(line);
            existing.insert(line);
        }
    }
    if (new_lines.empty()) return 0;
    std::ofstream file(path, std::ios::app);
    if (!file.is_open()) return 0;
    for (const auto& line : new_lines) {
        file << line << "\n";
    }
    return static_cast<int>(new_lines.size());
}

nlohmann::json load_json(const std::string& path, const nlohmann::json& default_val) {
    try {
        std::ifstream file(path);
        if (!file.is_open()) return default_val;
        nlohmann::json data;
        file >> data;
        if (data.is_object()) return data;
        return default_val;
    } catch (...) {
        return default_val;
    }
}

void save_json(const std::string& path, const nlohmann::json& data) {
    try {
        std::ofstream file(path);
        if (file.is_open()) {
            file << data.dump(2);
        }
    } catch (...) {
        LOGE("Failed to save JSON to %s", path.c_str());
    }
}

// ---------- URI extraction ----------

std::set<std::string> extract_raw_uris_from_text(const std::string& text) {
    std::set<std::string> uris;
    if (text.empty()) return uris;

    static const std::regex uri_re(
        R"((?:vmess|vless|trojan|ss|shadowsocks)://[^\s"'<>\[\]]+)",
        std::regex::icase | std::regex::optimize
    );

    auto begin = std::sregex_iterator(text.begin(), text.end(), uri_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string uri = it->str();
        // Trim trailing punctuation
        while (!uri.empty()) {
            char last = uri.back();
            if (last == ')' || last == ']' || last == ',' || last == '.' ||
                last == ';' || last == ':' || last == '!' || last == '?') {
                uri.pop_back();
            } else {
                break;
            }
        }
        if (uri.size() > 10) {
            uris.insert(uri);
        }
    }

    // If no URIs found, try base64 decoding large blocks
    if (uris.empty()) {
        static const std::regex b64_re(R"([A-Za-z0-9+/=]{100,})");
        auto b_begin = std::sregex_iterator(text.begin(), text.end(), b64_re);
        int count = 0;
        for (auto it = b_begin; it != end && count < 20; ++it, ++count) {
            try {
                std::string decoded = safe_b64decode(it->str());
                if (decoded.find("://") != std::string::npos) {
                    auto sub = extract_raw_uris_from_text(decoded);
                    uris.insert(sub.begin(), sub.end());
                }
            } catch (...) {
                continue;
            }
        }
    }

    return uris;
}

// ---------- Config analysis ----------

bool is_cdn_based(const std::string& uri) {
    std::string lower = to_lower(uri);
    for (const auto& domain : CDN_WHITELIST_DOMAINS) {
        if (lower.find(to_lower(domain)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int has_anti_dpi_features(const std::string& uri) {
    std::string lower = to_lower(uri);
    int score = 0;

    for (const auto& indicator : ANTI_DPI_INDICATORS) {
        if (lower.find(indicator) != std::string::npos) {
            ++score;
        }
    }

    for (int port : WHITELIST_PORTS) {
        if (uri.find(":" + std::to_string(port)) != std::string::npos) {
            ++score;
            break;
        }
    }

    for (const auto& fp : DPI_EVASION_FINGERPRINTS) {
        if (lower.find("fp=" + fp) != std::string::npos) {
            score += 2;
            break;
        }
    }

    if (is_cdn_based(uri)) {
        score += 3;
    }

    return score;
}

bool is_likely_blocked(const std::string& uri) {
    std::string lower = to_lower(uri);
    for (const auto& pattern : IRAN_BLOCKED_PATTERNS) {
        if (lower.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool is_ipv4_preferred(const std::string& uri) {
    return !(uri.find('[') != std::string::npos && uri.find(']') != std::string::npos);
}

// ---------- Config prioritization ----------

std::vector<std::string> prioritize_configs(const std::vector<std::string>& uris) {
    std::vector<std::string> tier1, tier2, tier3, tier4, tier5, tier6, tier7, tier8;

    for (const auto& uri : uris) {
        if (is_likely_blocked(uri)) continue;

        std::string lower = to_lower(uri);
        bool ipv4 = is_ipv4_preferred(uri);
        bool cdn = is_cdn_based(uri);

        if (!ipv4) {
            tier7.push_back(uri);
            continue;
        }

        if (lower.substr(0, 8) == "vless://") {
            bool reality = (lower.find("reality") != std::string::npos || lower.find("pbk=") != std::string::npos);
            bool grpc = (lower.find("grpc") != std::string::npos || lower.find("gun") != std::string::npos);
            bool h2 = (lower.find("h2") != std::string::npos || lower.find("http/2") != std::string::npos);
            bool ws = (lower.find("ws") != std::string::npos || lower.find("websocket") != std::string::npos);
            bool tls443 = (uri.find(":443") != std::string::npos &&
                          (lower.find("security=tls") != std::string::npos || lower.find("tls") != std::string::npos));

            if (reality && cdn) tier1.push_back(uri);
            else if (reality) tier2.push_back(uri);
            else if (grpc || h2) tier3.push_back(uri);
            else if (ws && tls443) tier4.push_back(uri);
            else if (tls443) tier6.push_back(uri);
            else tier8.push_back(uri);
        }
        else if (lower.substr(0, 9) == "trojan://") {
            bool grpc = (lower.find("grpc") != std::string::npos || lower.find("gun") != std::string::npos);
            bool ws = (lower.find("ws") != std::string::npos || lower.find("websocket") != std::string::npos);
            bool port443 = (uri.find(":443") != std::string::npos);

            if (grpc) tier3.push_back(uri);
            else if (ws && port443) tier4.push_back(uri);
            else if (port443) tier6.push_back(uri);
            else tier8.push_back(uri);
        }
        else if (lower.substr(0, 8) == "vmess://") {
            try {
                std::string payload = uri.substr(8);
                std::string decoded = safe_b64decode(payload);
                std::string dl = to_lower(decoded);

                bool ws_net = (dl.find("\"net\":\"ws\"") != std::string::npos);
                bool tls_on = (dl.find("\"tls\":\"tls\"") != std::string::npos);
                bool grpc_net = (dl.find("\"net\":\"grpc\"") != std::string::npos ||
                                dl.find("\"net\":\"gun\"") != std::string::npos);
                bool p443 = (dl.find("\"port\":\"443\"") != std::string::npos ||
                            dl.find("\"port\":443") != std::string::npos);

                if (grpc_net && tls_on) tier3.push_back(uri);
                else if (ws_net && tls_on && cdn) tier5.push_back(uri);
                else if (ws_net && tls_on && p443) tier4.push_back(uri);
                else if (tls_on && p443) tier6.push_back(uri);
                else tier8.push_back(uri);
            } catch (...) {
                tier8.push_back(uri);
            }
        }
        else {
            tier8.push_back(uri);
        }
    }

    // Shuffle within each tier
    auto shuffle_vec = [](std::vector<std::string>& v) {
        static std::mt19937 rng(std::random_device{}());
        std::shuffle(v.begin(), v.end(), rng);
    };

    shuffle_vec(tier1); shuffle_vec(tier2); shuffle_vec(tier3);
    shuffle_vec(tier4); shuffle_vec(tier5); shuffle_vec(tier6);
    shuffle_vec(tier7); shuffle_vec(tier8);

    std::vector<std::string> result;
    result.reserve(tier1.size() + tier2.size() + tier3.size() + tier4.size() +
                   tier5.size() + tier6.size() + tier7.size() + tier8.size());

    auto append = [&result](const std::vector<std::string>& v) {
        result.insert(result.end(), v.begin(), v.end());
    };

    append(tier1); append(tier2); append(tier3); append(tier4);
    append(tier5); append(tier6); append(tier7); append(tier8);

    return result;
}

// ---------- Random ----------

std::string random_user_agent() {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, BROWSER_USER_AGENTS.size() - 1);
    return BROWSER_USER_AGENTS[dist(rng)];
}

int random_int(int min, int max) {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(min, max);
    return dist(rng);
}

// ---------- Directory ----------

bool ensure_directory(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
}

} // namespace hunter
