#include "core/utils.h"

#include <ctime>
#include <cstring>
#include <chrono>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <random>
#include <regex>
#include <filesystem>
#include <thread>
#include <mutex>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace fs = std::filesystem;

namespace hunter {
namespace utils {

double nowTimestamp() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

uint64_t nowMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

// ─── SHA1 (minimal) ───
static void sha1_transform(uint32_t st[5], const uint8_t buf[64]) {
    uint32_t a,b,c,d,e,w[80];
    for(int i=0;i<16;i++)
        w[i]=(uint32_t)buf[i*4]<<24|(uint32_t)buf[i*4+1]<<16|
              (uint32_t)buf[i*4+2]<<8|(uint32_t)buf[i*4+3];
    for(int i=16;i<80;i++){uint32_t t=w[i-3]^w[i-8]^w[i-14]^w[i-16];w[i]=(t<<1)|(t>>31);}
    a=st[0];b=st[1];c=st[2];d=st[3];e=st[4];
    for(int i=0;i<80;i++){
        uint32_t f,k;
        if(i<20){f=(b&c)|((~b)&d);k=0x5A827999;}
        else if(i<40){f=b^c^d;k=0x6ED9EBA1;}
        else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
        else{f=b^c^d;k=0xCA62C1D6;}
        uint32_t t=((a<<5)|(a>>27))+f+e+k+w[i];
        e=d;d=c;c=(b<<30)|(b>>2);b=a;a=t;
    }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;st[4]+=e;
}

std::string sha1Hex(const std::string& input) {
    uint32_t state[5]={0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    size_t len=input.size();
    std::vector<uint8_t> buf(input.begin(),input.end());
    buf.push_back(0x80);
    while(buf.size()%64!=56) buf.push_back(0);
    uint64_t bits=len*8;
    for(int i=7;i>=0;i--) buf.push_back((uint8_t)(bits>>(i*8)));
    for(size_t i=0;i<buf.size();i+=64) sha1_transform(state,&buf[i]);
    char hex[41];
    snprintf(hex,sizeof(hex),"%08x%08x%08x%08x%08x",state[0],state[1],state[2],state[3],state[4]);
    return std::string(hex);
}

std::string toHex(uint64_t value) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%llx", (unsigned long long)value);
    return std::string(buf);
}

// ─── Base64 ───
static const std::string B64="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Decode(const std::string& enc) {
    std::string clean;
    for(char c:enc) if(c=='='||B64.find(c)!=std::string::npos) clean+=c;
    while(clean.size()%4!=0) clean+='=';
    std::string out; out.reserve(clean.size()*3/4);
    uint32_t val=0; int bits=-8;
    for(char c:clean){
        if(c=='=') break;
        size_t p=B64.find(c); if(p==std::string::npos) continue;
        val=(val<<6)|(uint32_t)p; bits+=6;
        if(bits>=0){out+=(char)((val>>bits)&0xFF);bits-=8;}
    }
    return out;
}

std::string base64Encode(const std::string& data) {
    std::string out; uint32_t val=0; int bits=-6;
    for(unsigned char c:data){val=(val<<8)|c;bits+=8;
        while(bits>=0){out+=B64[(val>>bits)&0x3F];bits-=6;}}
    if(bits>-6) out+=B64[((val<<8)>>(bits+8))&0x3F];
    while(out.size()%4!=0) out+='=';
    return out;
}

// ─── URL encoding ───
std::string urlDecode(const std::string& enc) {
    std::string out;
    for(size_t i=0;i<enc.size();i++){
        if(enc[i]=='%'&&i+2<enc.size()){
            int v=0;
            if(std::sscanf(enc.substr(i+1,2).c_str(),"%x",&v)==1){out+=(char)v;i+=2;}
            else out+=enc[i];
        } else if(enc[i]=='+') out+=' ';
        else out+=enc[i];
    }
    return out;
}

std::string urlEncode(const std::string& data) {
    std::string out; char buf[4];
    for(unsigned char c:data){
        if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') out+=(char)c;
        else{std::snprintf(buf,sizeof(buf),"%%%02X",c);out+=buf;}
    }
    return out;
}

// ─── URI extraction ───
std::set<std::string> extractRawUrisFromText(const std::string& text) {
    std::set<std::string> uris;
    static const std::regex re(
        R"(((?:vmess|vless|trojan|ss|ssr|hysteria2|hy2|tuic)://[^\s\r\n<>"']+))",
        std::regex::optimize);
    auto begin=std::sregex_iterator(text.begin(),text.end(),re);
    for(auto it=begin;it!=std::sregex_iterator();++it){
        std::string u=trim((*it)[1].str());
        if(u.size()>10) uris.insert(u);
    }
    return uris;
}

std::set<std::string> tryDecodeAndExtract(const std::string& text) {
    auto uris=extractRawUrisFromText(text);
    if(!uris.empty()) return uris;
    try{
        std::string dec=base64Decode(text);
        if(dec.find("://")!=std::string::npos) return extractRawUrisFromText(dec);
    }catch(...){}
    return uris;
}

// ─── File I/O ───
std::vector<std::string> readLines(const std::string& fp) {
    std::vector<std::string> lines;
    std::ifstream f(fp); if(!f) return lines;
    std::string line;
    while(std::getline(f,line)){line=trim(line);if(!line.empty()) lines.push_back(line);}
    return lines;
}

bool writeLines(const std::string& fp, const std::vector<std::string>& lines) {
    try{mkdirRecursive(dirName(fp));}catch(...){}
    std::ofstream f(fp); if(!f) return false;
    for(auto& l:lines) f<<l<<"\n";
    return true;
}

int appendUniqueLines(const std::string& fp, const std::vector<std::string>& lines) {
    std::set<std::string> ex;
    {std::ifstream f(fp);std::string l;while(std::getline(f,l)){l=trim(l);if(!l.empty())ex.insert(l);}}
    int added=0;
    std::ofstream f(fp,std::ios::app); if(!f) return 0;
    for(auto& l:lines){if(!l.empty()&&ex.find(l)==ex.end()){f<<l<<"\n";ex.insert(l);added++;}}
    return added;
}

std::string loadJsonFile(const std::string& fp) {
    std::ifstream f(fp); if(!f) return "{}";
    std::stringstream ss; ss<<f.rdbuf(); return ss.str();
}

bool saveJsonFile(const std::string& fp, const std::string& json) {
    try{mkdirRecursive(dirName(fp));}catch(...){}
    std::ofstream f(fp); if(!f) return false;
    f<<json; return true;
}

// ─── String helpers ───
std::string trim(const std::string& s) {
    auto start=s.find_first_not_of(" \t\r\n");
    if(start==std::string::npos) return "";
    auto end=s.find_last_not_of(" \t\r\n");
    return s.substr(start,end-start+1);
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::istringstream ss(s); std::string token;
    while(std::getline(ss,token,delim)) parts.push_back(token);
    return parts;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for(size_t i=0;i<parts.size();i++){if(i>0) out+=sep; out+=parts[i];}
    return out;
}

bool iequals(const std::string& a, const std::string& b) {
    if(a.size()!=b.size()) return false;
    return std::equal(a.begin(),a.end(),b.begin(),
        [](char x,char y){return std::tolower(x)==std::tolower(y);});
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size()>=prefix.size()&&s.compare(0,prefix.size(),prefix)==0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size()>=suffix.size()&&s.compare(s.size()-suffix.size(),suffix.size(),suffix)==0;
}

// ─── Filesystem ───
bool mkdirRecursive(const std::string& path) {
    try{fs::create_directories(path);return true;}catch(...){return false;}
}

bool fileExists(const std::string& path) {
    try{return fs::exists(path);}catch(...){return false;}
}

std::string dirName(const std::string& path) {
    try{return fs::path(path).parent_path().string();}catch(...){
        auto pos=path.find_last_of("/\\");
        return pos!=std::string::npos?path.substr(0,pos):".";
    }
}

// ─── Network helpers ───
void ensureSocketLayer() {
#ifdef _WIN32
    static std::once_flag s_wsa_once;
    std::call_once(s_wsa_once, []() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    });
#endif
}

SOCKET createTcpSocket(const std::string& host, int port, double timeout_sec) {
#ifdef _WIN32
    ensureSocketLayer();
#endif
    SOCKET fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0) return INVALID_SOCKET;
    
    // Set non-blocking
#ifdef _WIN32
    u_long mode=1; ioctlsocket(fd,FIONBIO,&mode);
#else
    int flags=fcntl(fd,F_GETFL,0); fcntl(fd,F_SETFL,flags|O_NONBLOCK);
#endif
    
    // Resolve hostname
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_port=htons((uint16_t)port);
    
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        // Need DNS resolution
        struct hostent* he = gethostbyname(host.c_str());
        if (!he) {
#ifdef _WIN32
            closesocket(fd);
#else
            close(fd);
#endif
            return INVALID_SOCKET;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    // Connect with timeout
    ::connect(fd,(sockaddr*)&addr,sizeof(addr));
    
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd,&wset);
    timeval tv;
    tv.tv_sec=(int)timeout_sec;
    tv.tv_usec=(int)((timeout_sec - (int)timeout_sec) * 1000000);
    
    bool ok = select(fd+1,nullptr,&wset,nullptr,&tv) > 0;
    
    if (!ok) {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        return INVALID_SOCKET;
    }
    
    // Set back to blocking
#ifdef _WIN32
    mode=0; ioctlsocket(fd,FIONBIO,&mode);
#else
    flags=fcntl(fd,F_GETFL,0); fcntl(fd,F_SETFL,flags&~O_NONBLOCK);
#endif
    
    return fd;
}

void closeSocket(SOCKET fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

namespace {

bool setSocketTimeoutMs(SOCKET fd, int timeout_ms) {
    if (fd == INVALID_SOCKET) return false;
    const int safe_timeout = timeout_ms > 0 ? timeout_ms : 1;
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(safe_timeout);
    const char* opt = reinterpret_cast<const char*>(&tv);
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, opt, sizeof(tv)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, opt, sizeof(tv)) == 0;
#else
    timeval tv{};
    tv.tv_sec = safe_timeout / 1000;
    tv.tv_usec = (safe_timeout % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool sendAll(SOCKET fd, const char* data, size_t size) {
    size_t sent_total = 0;
    while (sent_total < size) {
        const int sent = send(fd, data + sent_total, static_cast<int>(size - sent_total), 0);
        if (sent <= 0) return false;
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

bool readAtLeast(SOCKET fd, char* data, size_t size) {
    size_t received_total = 0;
    while (received_total < size) {
        const int received = recv(fd, data + received_total, static_cast<int>(size - received_total), 0);
        if (received <= 0) return false;
        received_total += static_cast<size_t>(received);
    }
    return true;
}

SOCKET connectLocalPort(int port, int timeout_ms) {
    SOCKET fd = createTcpSocket("127.0.0.1", port, std::max(timeout_ms, 1) / 1000.0);
    if (fd == INVALID_SOCKET) return INVALID_SOCKET;
    if (!setSocketTimeoutMs(fd, timeout_ms)) {
        closeSocket(fd);
        return INVALID_SOCKET;
    }
    return fd;
}

} // namespace

std::string execCommand(const std::string& cmd) {
    std::string result;
#ifdef _WIN32
    std::string shell_cmd = "cmd /c " + cmd;
    FILE* pipe = _popen(shell_cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    
    if (!pipe) return result;
    
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    
    // Remove trailing newlines
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    
    return result;
}

bool isPortAlive(int port, int timeout_ms) {
#ifdef _WIN32
    ensureSocketLayer();
#endif
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0) return false;
#ifdef _WIN32
    u_long mode=1; ioctlsocket(fd,FIONBIO,&mode);
#else
    int flags=fcntl(fd,F_GETFL,0); fcntl(fd,F_SETFL,flags|O_NONBLOCK);
#endif
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons((uint16_t)port);
    inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
    ::connect(fd,(sockaddr*)&addr,sizeof(addr));
    fd_set wset; FD_ZERO(&wset); FD_SET(fd,&wset);
    timeval tv; tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000;
    bool ok=select(fd+1,nullptr,&wset,nullptr,&tv)>0;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    return ok;
}

bool waitForPortAlive(int port, int timeout_ms, int probe_interval_ms) {
    if (timeout_ms <= 0) return isPortAlive(port, 200);
    if (probe_interval_ms <= 0) probe_interval_ms = 100;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (isPortAlive(port, std::min(probe_interval_ms, 250))) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(probe_interval_ms));
    }
    return isPortAlive(port, std::min(timeout_ms, 300));
}

bool probeLocalSocks5(int port, int timeout_ms) {
    SOCKET fd = connectLocalPort(port, timeout_ms);
    if (fd == INVALID_SOCKET) return false;
    const char handshake[] = {0x05, 0x01, 0x00};
    char response[2] = {0, 0};
    const bool ok = sendAll(fd, handshake, sizeof(handshake)) &&
                    readAtLeast(fd, response, sizeof(response)) &&
                    static_cast<unsigned char>(response[0]) == 0x05 &&
                    static_cast<unsigned char>(response[1]) != 0xFF;
    closeSocket(fd);
    return ok;
}

bool probeLocalHttpProxy(int port, int timeout_ms) {
    SOCKET fd = connectLocalPort(port, timeout_ms);
    if (fd == INVALID_SOCKET) return false;
    const std::string request =
        "CONNECT 127.0.0.1:1 HTTP/1.1\r\n"
        "Host: 127.0.0.1:1\r\n"
        "Proxy-Connection: keep-alive\r\n"
        "\r\n";
    char response[64] = {0};
    const bool ok = sendAll(fd, request.data(), request.size()) &&
                    recv(fd, response, static_cast<int>(sizeof(response) - 1), 0) > 0 &&
                    std::string(response).find("HTTP/") != std::string::npos;
    closeSocket(fd);
    return ok;
}

LocalProxyProbeResult probeLocalMixedPort(int port, int timeout_ms) {
    LocalProxyProbeResult result;
    result.tcp_alive = isPortAlive(port, timeout_ms);
    if (!result.tcp_alive) return result;
    result.socks_ready = probeLocalSocks5(port, timeout_ms);
    result.http_ready = probeLocalHttpProxy(port, timeout_ms);
    return result;
}

bool tcpConnect(const std::string& host, int port, int timeout_ms) {
#ifdef _WIN32
    ensureSocketLayer();
#endif
    // Only pre-screen IP addresses, skip domains (DNS is censored locally)
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return true; // Domain name - can't pre-screen, assume reachable
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
#ifdef _WIN32
    u_long mode = 1; ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    ::connect(fd, (sockaddr*)&addr, sizeof(addr));
    fd_set wset; FD_ZERO(&wset); FD_SET(fd, &wset);
    timeval tv; tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
    bool ok = select(fd + 1, nullptr, &wset, nullptr, &tv) > 0;
    if (ok) {
        // Check for connect error
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        ok = (err == 0);
    }
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
    return ok;
}

float portLatency(int port, int timeout_ms) {
    auto t0=std::chrono::steady_clock::now();
    if(!isPortAlive(port,timeout_ms)) return -1.0f;
    auto t1=std::chrono::steady_clock::now();
    return std::chrono::duration<float,std::milli>(t1-t0).count();
}

// ─── System info ───
float getMemoryPercent() {
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength=sizeof(ms);
    if(GlobalMemoryStatusEx(&ms)) return (float)ms.dwMemoryLoad;
    return 50.0f;
#else
    struct sysinfo si; if(sysinfo(&si)==0){
        float total=(float)si.totalram*si.mem_unit;
        float used=total-(float)si.freeram*si.mem_unit;
        return (used/total)*100.0f;
    }
    return 50.0f;
#endif
}

int getCpuCount() {
    int n=std::thread::hardware_concurrency();
    return n>0?n:4;
}

// ─── JsonBuilder ───
JsonBuilder& JsonBuilder::add(const std::string& key, const std::string& value) {
    std::string escaped;
    for(char c:value){
        if(c=='"') escaped+="\\\"";
        else if(c=='\\') escaped+="\\\\";
        else if(c=='\n') escaped+="\\n";
        else if(c=='\r') escaped+="\\r";
        else if(c=='\t') escaped+="\\t";
        else escaped+=c;
    }
    pairs_.push_back("\""+key+"\":\""+escaped+"\"");
    return *this;
}

JsonBuilder& JsonBuilder::add(const std::string& key, const char* value) {
    return add(key, std::string(value ? value : ""));
}

JsonBuilder& JsonBuilder::add(const std::string& key, int value) {
    pairs_.push_back("\""+key+"\":"+std::to_string(value));
    return *this;
}

JsonBuilder& JsonBuilder::add(const std::string& key, double value) {
    char buf[64]; std::snprintf(buf,sizeof(buf),"%.2f",value);
    pairs_.push_back("\""+key+"\":"+buf);
    return *this;
}

JsonBuilder& JsonBuilder::add(const std::string& key, bool value) {
    pairs_.push_back("\""+key+"\":"+(value?"true":"false"));
    return *this;
}

JsonBuilder& JsonBuilder::addRaw(const std::string& key, const std::string& raw) {
    pairs_.push_back("\""+key+"\":"+raw);
    return *this;
}

std::string JsonBuilder::build() const {
    return "{"+join(pairs_,",")+"}";
}

// ─── HTTP Download via SOCKS5 (using libcurl) ───

#include <curl/curl.h>

struct DownloadProgress {
    size_t total_bytes;
    std::chrono::steady_clock::time_point start;
    int max_bytes;
};

static size_t downloadWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* prog = static_cast<DownloadProgress*>(userp);
    size_t bytes = size * nmemb;
    prog->total_bytes += bytes;
    // Stop after enough data to measure speed (512KB)
    if (prog->total_bytes > (size_t)prog->max_bytes) return 0; // abort transfer
    return bytes;
}

static void ensureCurlGlobalInit() {
    static bool initialized = false;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_ALL);
        initialized = true;
    }
}

float downloadSpeedViaSocks5(const std::string& url, const std::string& proxy_host, 
                             int proxy_port, int timeout_seconds) {
    ensureCurlGlobalInit();
    
    CURL* curl = curl_easy_init();
    if (!curl) return -1.0f;
    
    DownloadProgress prog = {0, std::chrono::steady_clock::now(), 512 * 1024};
    std::string proxy_str = "socks5h://" + proxy_host + ":" + std::to_string(proxy_port);
    
    // Log the equivalent curl command for debugging
    { std::ostringstream _ls; _ls << "    [curl] curl --socks5-hostname " << proxy_str 
              << " --max-time " << timeout_seconds 
              << " --connect-timeout " << std::min(timeout_seconds, 10)
              << " -L -H \"Range: bytes=0-524287\" \"" << url << "\"";
      LogRingBuffer::instance().push(_ls.str()); }
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, downloadWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &prog);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)std::min(timeout_seconds, 10));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    // Only set Range for actual download URLs, not for 204-type connectivity checks
    if (url.find("generate_204") == std::string::npos && url.find("/cdn-cgi/") == std::string::npos) {
        curl_easy_setopt(curl, CURLOPT_RANGE, "0-524287"); // 512KB
    }
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    
    prog.start = std::chrono::steady_clock::now();
    CURLcode res = curl_easy_perform(curl);
    auto end = std::chrono::steady_clock::now();
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &prog.total_bytes);
    curl_easy_cleanup(curl);
    
    double elapsed = std::chrono::duration<double>(end - prog.start).count();
    
    // CURLE_WRITE_ERROR is expected when we abort after enough data
    if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
        { std::ostringstream _ls; _ls << "    [curl:" << proxy_port << "] error=" << (int)res 
                  << " (" << curl_easy_strerror(res) << ") bytes=" << prog.total_bytes
                  << " elapsed=" << std::fixed << std::setprecision(3) << elapsed << "s";
          LogRingBuffer::instance().push(_ls.str()); }
        return -1.0f;
    }
    if (http_code >= 400) {
        { std::ostringstream _ls; _ls << "    [curl:" << proxy_port << "] HTTP " << http_code
                  << " bytes=" << prog.total_bytes
                  << " elapsed=" << std::fixed << std::setprecision(3) << elapsed << "s";
          LogRingBuffer::instance().push(_ls.str()); }
        return -1.0f;
    }
    // HTTP 204 No Content = connectivity check passed (generate_204 endpoints)
    if (http_code == 204 || (http_code >= 200 && http_code < 300 && prog.total_bytes == 0)) {
        double latency = (elapsed > 0.001) ? (1.0 / elapsed) : 1.0f;
        { std::ostringstream _ls; _ls << "    [curl:" << proxy_port << "] HTTP " << http_code
                  << " latency=" << std::fixed << std::setprecision(3) << (elapsed * 1000) << "ms";
          LogRingBuffer::instance().push(_ls.str()); }
        return (elapsed > 0.001) ? (float)(1.0 / elapsed) : 1.0f; // Return latency-based score
    }
    if (prog.total_bytes < 100) {
        { std::ostringstream _ls; _ls << "    [curl:" << proxy_port << "] too few bytes: " << prog.total_bytes
                  << " HTTP " << http_code;
          LogRingBuffer::instance().push(_ls.str()); }
        return -1.0f;
    }
    
    if (elapsed < 0.001) elapsed = 0.001;
    
    float speed = (float)(prog.total_bytes / 1024.0 / elapsed);
    { std::ostringstream _ls; _ls << "    [curl:" << proxy_port << "] OK bytes=" << prog.total_bytes
              << " speed=" << std::fixed << std::setprecision(1) << speed << "KB/s"
              << " elapsed=" << std::setprecision(3) << elapsed << "s"
              << " HTTP " << http_code;
      LogRingBuffer::instance().push(_ls.str()); }
    
    return speed;
}

bool testProxyDownload(const std::string& url, const std::string& proxy_host, 
                       int proxy_port, int timeout_seconds) {
    float speed = downloadSpeedViaSocks5(url, proxy_host, proxy_port, timeout_seconds);
    return speed > 0.0f;
}

} // namespace utils
} // namespace hunter
