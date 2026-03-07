#include "web/dashboard.h"
#include "orchestrator/orchestrator.h"
#include "orchestrator/thread_manager.h"
#include "core/utils.h"
#include "core/constants.h"

#include <sstream>
#include <algorithm>

namespace hunter {
namespace web {

Dashboard::Dashboard(HunterOrchestrator* orch, int port)
    : orch_(orch), server_(std::make_unique<HttpServer>(port)) {}

Dashboard::~Dashboard() { stop(); }

bool Dashboard::start() {
    registerRoutes();
    return server_->start();
}

void Dashboard::stop() {
    if (server_) server_->stop();
}

void Dashboard::pushLog(const std::string& level, const std::string& message) {
    if (server_) server_->pushLog(level, message);
}

bool Dashboard::isRunning() const {
    return server_ && server_->isRunning();
}

void Dashboard::registerRoutes() {
    server_->get("/", [this](const HttpServer::Request& req) { return handleIndex(req); });
    server_->get("/api/status", [this](const HttpServer::Request& req) { return handleApiStatus(req); });
    server_->get("/api/threads/status", [this](const HttpServer::Request& req) { return handleApiThreadsStatus(req); });
    server_->get("/api/balancer/details", [this](const HttpServer::Request& req) { return handleApiBalancerDetails(req); });
    server_->post("/api/balancer/force", [this](const HttpServer::Request& req) { return handleApiBalancerForce(req); });
    server_->post("/api/balancer/unforce", [this](const HttpServer::Request& req) { return handleApiBalancerUnforce(req); });
    server_->post("/api/command", [this](const HttpServer::Request& req) { return handleApiCommand(req); });
    server_->get("/api/logs/stream", [this](const HttpServer::Request& req) { return handleApiLogStream(req); });
    server_->get("/api/telegram/report-status", [this](const HttpServer::Request& req) { return handleApiTelegramStatus(req); });
}

HttpServer::Response Dashboard::handleIndex(const HttpServer::Request&) {
    return HttpServer::Response::html(generateDashboardHtml());
}

HttpServer::Response Dashboard::handleApiStatus(const HttpServer::Request&) {
    if (!orch_) return HttpServer::Response::error("Orchestrator not available", 503);
    auto& ds = orch_->dashboardState();
    utils::JsonBuilder jb;
    jb.add("ok", true);
    jb.add("cycle", ds.cycle);
    jb.add("phase", ds.phase);
    jb.add("mode", ds.mode);
    jb.add("memory_pct", (double)ds.memory_pct);
    jb.add("scraped", ds.scraped);
    jb.add("tested", ds.tested);
    jb.add("gold", ds.gold);
    jb.add("silver", ds.silver);
    jb.add("failed", ds.failed);
    jb.add("backends", ds.backends);
    jb.add("uptime", (double)(utils::nowTimestamp() - ds.uptime_start));
    return HttpServer::Response::json(jb.build());
}

HttpServer::Response Dashboard::handleApiThreadsStatus(const HttpServer::Request&) {
    if (!orch_) return HttpServer::Response::error("Orchestrator not available", 503);

    // Build hardware JSON
    auto hw = HunterTaskManager::instance().getHardware();
    std::ostringstream ss;
    ss << "{\"ok\":true,\"hardware\":{";
    ss << "\"cpu_count\":" << hw.cpu_count;
    ss << ",\"cpu_percent\":" << hw.cpu_percent;
    ss << ",\"ram_total_gb\":" << hw.ram_total_gb;
    ss << ",\"ram_used_gb\":" << hw.ram_used_gb;
    ss << ",\"ram_percent\":" << hw.ram_percent;
    static const char* mode_names[] = {"NORMAL","MODERATE","SCALED","CONSERVATIVE","REDUCED","MINIMAL","ULTRA-MINIMAL"};
    ss << ",\"mode\":\"" << mode_names[std::min((int)hw.mode, 6)] << "\"";
    ss << ",\"thread_count\":0";
    ss << "},\"workers\":{";

    // Note: thread_manager is private in orchestrator, so we return minimal info
    // In full implementation, orchestrator would expose thread_manager status
    ss << "}}";
    return HttpServer::Response::json(ss.str());
}

HttpServer::Response Dashboard::handleApiBalancerDetails(const HttpServer::Request&) {
    if (!orch_) return HttpServer::Response::error("Orchestrator not available", 503);

    std::ostringstream ss;
    ss << "{\"main\":";
    auto* bal = orch_->balancer();
    if (bal) {
        auto st = bal->getStatus();
        ss << "{\"status\":{\"running\":" << (st.running ? "true" : "false")
           << ",\"backends\":" << st.backend_count
           << ",\"healthy\":" << st.healthy_count
           << ",\"port\":" << st.port << "}}";
    } else {
        ss << "null";
    }

    ss << ",\"gemini\":";
    auto* gbal = orch_->geminiBalancer();
    if (gbal) {
        auto st = gbal->getStatus();
        ss << "{\"status\":{\"running\":" << (st.running ? "true" : "false")
           << ",\"backends\":" << st.backend_count
           << ",\"port\":" << st.port << "}}";
    } else {
        ss << "null";
    }
    ss << "}";
    return HttpServer::Response::json(ss.str());
}

HttpServer::Response Dashboard::handleApiBalancerForce(const HttpServer::Request& req) {
    // Minimal: parse URI from JSON body
    // In production, use a proper JSON parser
    auto pos = req.body.find("\"uri\"");
    if (pos == std::string::npos)
        return HttpServer::Response::error("No URI provided", 400);
    // Extract value
    auto colon = req.body.find(':', pos);
    auto quote1 = req.body.find('"', colon + 1);
    auto quote2 = req.body.find('"', quote1 + 1);
    if (quote1 == std::string::npos || quote2 == std::string::npos)
        return HttpServer::Response::error("Invalid JSON", 400);
    std::string uri = req.body.substr(quote1 + 1, quote2 - quote1 - 1);

    auto* bal = orch_->balancer();
    if (!bal) return HttpServer::Response::error("Balancer not available", 503);

    if (bal->setForcedBackend(uri)) {
        return HttpServer::Response::json("{\"ok\":true,\"message\":\"Backend forced\"}");
    }
    return HttpServer::Response::error("Invalid URI", 400);
}

HttpServer::Response Dashboard::handleApiBalancerUnforce(const HttpServer::Request&) {
    auto* bal = orch_->balancer();
    if (bal) bal->clearForcedBackend();
    return HttpServer::Response::json("{\"ok\":true,\"message\":\"Force cleared\"}");
}

HttpServer::Response Dashboard::handleApiCommand(const HttpServer::Request& req) {
    // Extract command from JSON body
    auto pos = req.body.find("\"command\"");
    if (pos == std::string::npos)
        return HttpServer::Response::json("{\"ok\":false,\"message\":\"No command\"}");
    auto colon = req.body.find(':', pos);
    auto quote1 = req.body.find('"', colon + 1);
    auto quote2 = req.body.find('"', quote1 + 1);
    std::string cmd = (quote1 != std::string::npos && quote2 != std::string::npos)
                      ? req.body.substr(quote1 + 1, quote2 - quote1 - 1) : "";

    if (cmd == "status") {
        return HttpServer::Response::json("{\"ok\":true,\"message\":\"Running\"}");
    } else if (cmd == "force_cycle") {
        // Would trigger cycle in background thread
        return HttpServer::Response::json("{\"ok\":true,\"message\":\"Force cycle requested\"}");
    }
    return HttpServer::Response::json("{\"ok\":false,\"message\":\"Unknown command: " + cmd + "\"}");
}

HttpServer::Response Dashboard::handleApiLogStream(const HttpServer::Request& req) {
    int since = 0;
    auto it = req.query_params.find("since");
    if (it != req.query_params.end()) {
        try { since = std::stoi(it->second); } catch (...) {}
    }
    return HttpServer::Response::json(server_->getLogStreamJson(since));
}

HttpServer::Response Dashboard::handleApiTelegramStatus(const HttpServer::Request&) {
    auto* reporter = orch_->botReporter();
    if (!reporter) {
        return HttpServer::Response::json("{\"bot_configured\":false,\"user_configured\":false}");
    }
    auto rs = reporter->getReportStatus();
    utils::JsonBuilder jb;
    jb.add("bot_configured", rs.bot_configured);
    jb.add("bot_token_preview", rs.bot_token_preview);
    jb.add("publish_count", rs.publish_count);
    jb.add("last_publish", rs.last_publish);
    jb.add("user_configured", false);
    return HttpServer::Response::json(jb.build());
}

std::string Dashboard::generateDashboardHtml() {
    // Minimal dashboard HTML — in production, embed the full dashboard.html
    return R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Hunter Dashboard</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0d1117;color:#c9d1d9}
nav{background:#161b22;padding:12px 20px;display:flex;align-items:center;gap:12px;border-bottom:1px solid #30363d}
.brand{font-weight:700;font-size:16px;color:#58a6ff;display:flex;align-items:center;gap:8px}
.shell{max-width:1200px;margin:0 auto;padding:16px}
.card{background:#161b22;border:1px solid #30363d;border-radius:10px;margin-bottom:12px;overflow:hidden}
.card-head{padding:10px 14px;border-bottom:1px solid #30363d;font-size:12px;font-weight:600;color:#8b949e;display:flex;justify-content:space-between}
.card-body{padding:14px}
.pill{display:inline-block;padding:2px 8px;border-radius:12px;font-size:10px;font-weight:600;background:rgba(88,166,255,.12);color:#58a6ff}
.stat{font-size:12px;color:#8b949e}
.stat b{color:#c9d1d9}
.btn{padding:6px 14px;border:1px solid #30363d;border-radius:6px;background:transparent;color:#c9d1d9;cursor:pointer;font-size:11px}
.btn:hover{background:#21262d}
#status{padding:20px;font-size:14px;text-align:center;color:#8b949e}
</style></head><body>
<nav>
  <div class="brand">&#128269; Hunter C++</div>
  <span class="pill" id="phase">STARTING</span>
  <div style="flex:1"></div>
  <span class="stat">RAM <b id="ram">-</b></span>
  <span class="stat">Mode <b id="mode">-</b></span>
</nav>
<div class="shell">
  <div class="card">
    <div class="card-head"><span>Status</span><button class="btn" onclick="refresh()">Refresh</button></div>
    <div class="card-body" id="status">Loading...</div>
  </div>
  <div class="card">
    <div class="card-head"><span>Workers</span></div>
    <div class="card-body" id="workers">Loading...</div>
  </div>
  <div class="card">
    <div class="card-head"><span>Logs</span></div>
    <div class="card-body" style="max-height:300px;overflow-y:auto;font-family:monospace;font-size:11px" id="logs"></div>
  </div>
</div>
<script>
function refresh(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    if(!d.ok)return;
    document.getElementById('phase').textContent=d.phase||'IDLE';
    document.getElementById('ram').textContent=(d.memory_pct||0).toFixed(0)+'%';
    document.getElementById('mode').textContent=d.mode||'-';
    document.getElementById('status').innerHTML=
      `Cycle: ${d.cycle} | Scraped: ${d.scraped} | Gold: ${d.gold} | Silver: ${d.silver} | Backends: ${d.backends}`;
  }).catch(()=>{});
  fetch('/api/threads/status').then(r=>r.json()).then(d=>{
    if(!d.ok)return;
    const hw=d.hardware||{};
    let html=`<div style="margin-bottom:8px;font-size:11px">CPU: ${hw.cpu_count} cores | RAM: ${(hw.ram_percent||0).toFixed(0)}% (${(hw.ram_used_gb||0).toFixed(1)}/${(hw.ram_total_gb||0).toFixed(1)} GB) | Mode: ${hw.mode||'-'}</div>`;
    const w=d.workers||{};
    for(const[name,s]of Object.entries(w)){
      html+=`<div style="display:inline-block;margin:4px;padding:6px 10px;background:#0d1117;border:1px solid #30363d;border-radius:6px;font-size:11px"><b>${name}</b> <span class="pill">${s.state}</span> runs:${s.runs}</div>`;
    }
    document.getElementById('workers').innerHTML=html;
  }).catch(()=>{});
  fetch('/api/logs/stream?since=0').then(r=>r.json()).then(d=>{
    let html='';
    for(const e of d){
      const t=new Date(e.timestamp*1000).toLocaleTimeString('en-US',{hour12:false});
      const c=e.level==='ERROR'?'#f85149':e.level==='WARNING'?'#d29922':'#8b949e';
      html+=`<div style="color:${c}">${t} [${e.level}] ${e.message}</div>`;
    }
    document.getElementById('logs').innerHTML=html||'<span style="color:#8b949e">No logs yet</span>';
  }).catch(()=>{});
}
refresh();
setInterval(refresh,5000);
</script></body></html>)HTML";
}

} // namespace web
} // namespace hunter
