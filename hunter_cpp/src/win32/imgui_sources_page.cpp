#include "win32/imgui_app.h"
#include "win32/font_awesome.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/seed_data.h"
#include "third_party/imgui/imgui.h"
#include <sstream>
#include <iostream>
#include <cmath>

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
    std::cout << "[UI] DrawSourcesPage called!" << std::endl;
    
    ImGui::TextColored(COL_ACCENT, "Source Management & Config Downloads");
    ImGui::Spacing();
    
    ImGui::TextColored(COL_DIM, "All sources are auto-selected. Only allowed premium URLs are shown below.");
    ImGui::TextColored(COL_DIM, "Total sources: %d", (int)source_manager_.sources.size());

    if (ImGui::BeginChild("##sources_actions", ImVec2(0, 0), false)) {
        ImGui::TextColored(COL_YELLOW, "Download Controls");
        ImGui::Separator();
        ImGui::SetNextItemWidth(320 * dpi_scale_);
        ImGui::InputTextWithHint("##proxy", "Optional proxy for downloads (host:port)", config_download_proxy_.data(), config_download_proxy_.size());
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_DOWNLOAD " Download All", ImVec2(0, 0))) {
            std::cout << "[UI] Download All Sources button clicked!" << std::endl;
            DownloadConfigsFromSources();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_REFRESH " Export Seed Data", ImVec2(0, 0))) {
            ExportSeedDataForPackaging();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_SEARCH " Test Proxy", ImVec2(0, 0))) {
            if (!config_download_proxy_[0]) {
                SetToast("Enter proxy address first", ToastKind::Warning);
            } else {
                RunCommandAsync("{\"command\":\"test_proxy\",\"proxy\":\"" + JsonEscape(config_download_proxy_.data()) + "\"}");
                SetToast("Testing proxy connectivity...", ToastKind::Info);
            }
        }

        ImGui::Spacing();
        if (download_progress_.active) {
            ImGui::TextColored(COL_ACCENT, "Current Download");
            ImGui::ProgressBar(download_progress_.progress, ImVec2(-1, 18 * dpi_scale_));
            ImGui::Text("%s", download_progress_.status.c_str());
            if (!download_progress_.current_source.empty()) {
                ImGui::TextWrapped("Source: %s", download_progress_.current_source.c_str());
            }
            ImGui::Text("%d/%d sources | %.1f%%",
                        download_progress_.downloaded_count,
                        download_progress_.total_count,
                        download_progress_.progress * 100.0f);
            if (download_progress_.total_size > 0) {
                ImGui::Text("Downloaded: %s / %s",
                            formatSize(download_progress_.downloaded_size).c_str(),
                            formatSize(download_progress_.total_size).c_str());
            }
            if (download_progress_.speed > 0.0f) {
                ImGui::Text("Speed: %s/s", formatSize(download_progress_.speed).c_str());
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(COL_YELLOW, "Allowed Sources");
        ImGui::Separator();
        if (ImGui::BeginTable("SourcesTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 270 * dpi_scale_))) {
            ImGui::TableSetupColumn("URL", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110.0f * dpi_scale_);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f * dpi_scale_);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpi_scale_);
            ImGui::TableHeadersRow();

            size_t visible_sources = 0;
            for (size_t i = 0; i < source_manager_.sources.size(); ++i) {
                const auto& source = source_manager_.sources[i];
                const bool is_allowed = source.category == "primary" || source.category == "premium" || source.enabled;
                if (!is_allowed) continue;
                ++visible_sources;

                ImGui::PushID((int)i);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextWrapped("%s", source.url.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", source.url.c_str());

                ImGui::TableSetColumnIndex(1);
                ImVec4 cat_color = source.category == "primary" ? COL_GREEN : (source.category == "premium" ? COL_ACCENT : COL_YELLOW);
                ImGui::TextColored(cat_color, "%s", source.category.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(COL_GREEN, "%s", source.enabled ? "Selected" : "Auto");

                ImGui::TableSetColumnIndex(3);
                const char* test_label = FontAwesome::IsLoaded() ? (ICON_FA_EYE "##test") : "Test##test";
                const char* edit_label = FontAwesome::IsLoaded() ? (ICON_FA_EDIT "##edit") : "Edit##edit";
                const char* del_label  = FontAwesome::IsLoaded() ? (ICON_FA_TRASH "##delete") : "Del##delete";
                if (ImGui::Button(test_label, ImVec2(40 * dpi_scale_, 24 * dpi_scale_))) {
                    SetToast("Testing source: " + source.name, ToastKind::Info);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Test source");
                ImGui::SameLine(0, 4 * dpi_scale_);
                if (ImGui::Button(edit_label, ImVec2(40 * dpi_scale_, 24 * dpi_scale_))) {
                    SetToast("Edit source: " + source.name, ToastKind::Info);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edit source");
                ImGui::SameLine(0, 4 * dpi_scale_);
                if (ImGui::Button(del_label, ImVec2(40 * dpi_scale_, 24 * dpi_scale_))) {
                    SetToast("Delete source: " + source.name, ToastKind::Warning);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete source");

                ImGui::PopID();
            }

            if (visible_sources == 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(COL_DIM, "No allowed sources found. Use seed data or import sources.");
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextColored(COL_YELLOW, "Download Log");
        ImGui::Separator();
        static bool show_download_logs_only = true;
        ImGui::Checkbox("Only download messages", &show_download_logs_only);
        ImGui::SameLine();
        if (ImGui::Button("Clear")) download_logs_.clear();

        if (ImGui::BeginTable("##download_logs_table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 160 * dpi_scale_))) {
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90 * dpi_scale_);
            ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& log : download_logs_) {
                if (show_download_logs_only && log.find("[Download]") == std::string::npos) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(log.find("from ") != std::string::npos ? "Source" : "System");
                ImGui::TableSetColumnIndex(1);
                const bool failed = log.find("ERROR") != std::string::npos || log.find("Failed") != std::string::npos;
                const bool success = log.find("SUCCESS") != std::string::npos || log.find("completed") != std::string::npos;
                ImGui::TextColored(failed ? COL_RED : (success ? COL_GREEN : COL_DIM), failed ? "FAILED" : (success ? "OK" : "INFO"));
                ImGui::TableSetColumnIndex(2);
                std::string short_log = log;
                if (short_log.size() > 220) {
                    short_log = short_log.substr(0, 217) + "...";
                }
                ImGui::TextWrapped("%s", short_log.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
}

} // namespace win32
} // namespace hunter
