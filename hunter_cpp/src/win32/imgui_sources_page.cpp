#include "win32/imgui_app.h"
#include "win32/font_awesome.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/seed_data.h"
#include "third_party/imgui/imgui.h"
#include <sstream>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cctype>

namespace hunter {
namespace win32 {

// Color constants (matching main app)
static const ImVec4 COL_ACCENT   = {0.22f, 0.47f, 0.95f, 1.0f};
static const ImVec4 COL_GREEN    = {0.18f, 0.80f, 0.44f, 1.0f};
static const ImVec4 COL_RED      = {0.92f, 0.34f, 0.34f, 1.0f};
static const ImVec4 COL_YELLOW   = {1.00f, 0.78f, 0.24f, 1.0f};
static const ImVec4 COL_DIM      = {0.50f, 0.50f, 0.50f, 1.0f};

// Helper functions for formatting
std::string formatSize(double bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = bytes;
    
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        unit++;
    }
    
    char buffer[64];
    if (unit == 0) {
        snprintf(buffer, sizeof(buffer), "%.0f %s", size, units[unit]);
    } else {
        snprintf(buffer, sizeof(buffer), "%.1f %s", size, units[unit]);
    }
    
    return std::string(buffer);
}

std::string formatTime(double seconds) {
    if (seconds < 60) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.0fs", seconds);
        return std::string(buffer);
    } else if (seconds < 3600) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.0fm %.0fs", seconds / 60, fmod(seconds, 60));
        return std::string(buffer);
    } else {
        char buffer[32];
        double hours = seconds / 3600;
        double minutes = fmod(seconds / 60, 60);
        snprintf(buffer, sizeof(buffer), "%.0fh %.0fm", hours, minutes);
        return std::string(buffer);
    }
}

void ImGuiApp::DrawSourcesPage() {
    static char source_url_input[1024] = "";
    static char source_name_input[128] = "";
    static char source_filter_input[128] = "";
    static bool show_only_enabled = true;

    int enabled_count = 0;
    for (const auto& source : source_manager_.sources) {
        if (source.enabled) enabled_count++;
    }

    ImGui::TextColored(COL_ACCENT, "Sources");
    ImGui::TextColored(COL_DIM, "Simple source workflow: add URL -> enable/disable -> download.");
    ImGui::Spacing();

    ImGui::Text("Total: %d", static_cast<int>(source_manager_.sources.size()));
    ImGui::SameLine();
    ImGui::TextColored(COL_GREEN, "Enabled: %d", enabled_count);
    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "Disabled: %d", static_cast<int>(source_manager_.sources.size()) - enabled_count);

    ImGui::Separator();
    ImGui::SetNextItemWidth(360 * dpi_scale_);
    ImGui::InputTextWithHint("##source_url_input", "https://example.com/sub.txt", source_url_input, sizeof(source_url_input));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * dpi_scale_);
    ImGui::InputTextWithHint("##source_name_input", "Source name (optional)", source_name_input, sizeof(source_name_input));
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS " Add Source")) {
        const std::string url = utils::trim(source_url_input);
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
            SetToast("Invalid URL: must start with http:// or https://", ToastKind::Warning);
        } else {
            AddSource(url, utils::trim(source_name_input), "custom");
            source_url_input[0] = '\0';
            source_name_input[0] = '\0';
        }
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(300 * dpi_scale_);
    ImGui::InputTextWithHint("##proxy", "Proxy (optional): 127.0.0.1:11808", config_download_proxy_.data(), config_download_proxy_.size());
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_DOWNLOAD " Download Enabled")) {
        DownloadConfigsFromSources();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_REFRESH " Reset Defaults")) {
        InitializeDefaultSources();
        SaveSourceManager();
        SetToast("Default sources restored", ToastKind::Info);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_SEARCH " Test Proxy")) {
        if (!config_download_proxy_[0]) {
            SetToast("Enter proxy address first", ToastKind::Warning);
        } else {
            RunCommandAsync("{\"command\":\"test_proxy\",\"proxy\":\"" + JsonEscape(config_download_proxy_.data()) + "\"}");
            SetToast("Testing proxy...", ToastKind::Info);
        }
    }

    if (download_progress_.active) {
        ImGui::Spacing();
        ImGui::TextColored(COL_ACCENT, "Download Progress");
        ImGui::ProgressBar(download_progress_.progress, ImVec2(-1, 18 * dpi_scale_));
        ImGui::Text("%d / %d sources | %.1f%% | %s",
                    download_progress_.downloaded_count,
                    download_progress_.total_count,
                    download_progress_.progress * 100.0f,
                    download_progress_.status.c_str());
        if (!download_progress_.current_source.empty()) {
            ImGui::TextWrapped("Current: %s", download_progress_.current_source.c_str());
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(COL_YELLOW, "Source List");
    ImGui::SetNextItemWidth(300 * dpi_scale_);
    ImGui::InputTextWithHint("##source_filter", "Filter by name/url/category", source_filter_input, sizeof(source_filter_input));
    ImGui::SameLine();
    ImGui::Checkbox("Only enabled", &show_only_enabled);

    if (ImGui::BeginTable("SourcesTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 280 * dpi_scale_))) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 45 * dpi_scale_);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 150 * dpi_scale_);
        ImGui::TableSetupColumn("URL", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 100 * dpi_scale_);
        ImGui::TableSetupColumn("Last Result", ImGuiTableColumnFlags_WidthFixed, 95 * dpi_scale_);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 90 * dpi_scale_);
        ImGui::TableHeadersRow();

        auto lowerCopy = [](std::string v) {
            std::transform(v.begin(), v.end(), v.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return v;
        };
        const std::string filter = lowerCopy(utils::trim(source_filter_input));
        int shown = 0;
        for (int i = 0; i < static_cast<int>(source_manager_.sources.size()); ++i) {
            const auto& source = source_manager_.sources[i];
            if (show_only_enabled && !source.enabled) continue;

            std::string haystack = lowerCopy(source.name + " " + source.url + " " + source.category);
            if (!filter.empty() && haystack.find(filter) == std::string::npos) continue;

            ImGui::PushID(i);
            ImGui::TableNextRow();
            shown++;

            ImGui::TableSetColumnIndex(0);
            bool enabled = source.enabled;
            if (ImGui::Checkbox("##enabled", &enabled)) {
                ToggleSourceEnabled(i);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(source.name.empty() ? "(unnamed)" : source.name.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", source.url.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", source.url.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(source.category == "primary" ? COL_GREEN : COL_DIM, "%s", source.category.c_str());

            ImGui::TableSetColumnIndex(4);
            auto it = source_history_.find(source.url);
            if (it == source_history_.end()) {
                ImGui::TextColored(COL_DIM, "N/A");
            } else if (it->second.last_success) {
                ImGui::TextColored(COL_GREEN, "OK");
            } else {
                ImGui::TextColored(COL_RED, "Failed");
            }

            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton(ICON_FA_TRASH " Remove")) {
                RemoveSource(i);
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
        }

        if (shown == 0) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(COL_DIM, "No source matches this filter.");
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextColored(COL_YELLOW, "Download Log");
    static bool show_download_logs_only = true;
    ImGui::Checkbox("Only download messages", &show_download_logs_only);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) download_logs_.clear();

    if (ImGui::BeginTable("##download_logs_table", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 150 * dpi_scale_))) {
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 85 * dpi_scale_);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& log : download_logs_) {
            if (show_download_logs_only && log.find("[Download]") == std::string::npos) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool failed = log.find("ERROR") != std::string::npos || log.find("FAILED") != std::string::npos || log.find("Failed") != std::string::npos;
            const bool success = log.find("SUCCESS") != std::string::npos || log.find("COMPLETED") != std::string::npos || log.find("completed") != std::string::npos;
            ImGui::TextColored(failed ? COL_RED : (success ? COL_GREEN : COL_DIM), failed ? "FAILED" : (success ? "OK" : "INFO"));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", log.c_str());
        }
        ImGui::EndTable();
    }
}

} // namespace win32
} // namespace hunter
