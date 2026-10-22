#include "gui/update_panel.h"
#include "core/updater.h"
#include "core/constants.h"
#include "imgui.h"
#include "ui/theme.hpp"

#include <thread>

namespace hunter { namespace gui {

using UI::Theme::Colors;
using UI::Theme::Spacing;

void renderUpdatePanel() {
    using namespace hunter::core;

    SelfUpdateManager& updater = SelfUpdateManager::instance();
    SelfUpdateManager::UpdateState state = updater.state();
    std::string status = updater.statusMessage();
    float progress = updater.downloadProgress();
    ReleaseInfo release = updater.latestRelease();

    ImGui::BeginChild("##update_panel", ImVec2(0, 0), false);
    {
        ImGui::Text("Installed: v%s", constants::HUNTER_VERSION);
        ImGui::Separator();
        ImGui::Spacing();

        switch (state) {
        case SelfUpdateManager::UpdateState::Idle:
        case SelfUpdateManager::UpdateState::Checking: {
            ImGui::TextDisabled("Checking for updates...");
            static int spinner_counter = 0;
            const char* spinner_chars = "|/-\\";
            ImGui::SameLine();
            ImGui::TextColored(Colors::AccentBlue, "%c", spinner_chars[spinner_counter / 8 % 4]);
            spinner_counter++;

            if (ImGui::SmallButton("Check again")) {
                std::thread([&updater]() { updater.checkForUpdate(); }).detach();
            }
            break;
        }
        case SelfUpdateManager::UpdateState::UpToDate: {
            ImGui::TextColored(Colors::SuccessGreen, "You're up to date");
            if (!release.tag_name.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(v%s)", release.tag_name.c_str());
            }
            if (ImGui::SmallButton("Check again")) {
                std::thread([&updater]() { updater.checkForUpdate(); }).detach();
            }
            break;
        }
        case SelfUpdateManager::UpdateState::UpdateAvailable: {
            ImGui::Text("Update available: %s", release.tag_name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("View release notes")) {
                ImGui::OpenPopup("release_notes");
            }
            if (ImGui::BeginPopup("release_notes")) {
                ImGui::TextUnformatted(release.body.c_str());
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Update Now", ImVec2(120 * ImGui::GetStyle().FontScaleDpi, 0))) {
                std::thread([&updater]() { updater.downloadAndApply(); }).detach();
            }
            break;
        }
        case SelfUpdateManager::UpdateState::Downloading: {
            ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), "");
            ImGui::SameLine();
            ImGui::Text("%.0f%%", progress * 100.0f);
            ImGui::TextDisabled("%s", status.c_str());
            break;
        }
        case SelfUpdateManager::UpdateState::DownloadComplete:
        case SelfUpdateManager::UpdateState::Applying: {
            ImGui::TextColored(Colors::WarnAmber, "Installing...");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status.c_str());
            break;
        }
        case SelfUpdateManager::UpdateState::UpdateComplete: {
            ImGui::TextColored(Colors::SuccessGreen, "Update installed — restart required");
            break;
        }
        case SelfUpdateManager::UpdateState::Failed: {
            bool is_network_error = false;
            std::string lower_msg;
            lower_msg.reserve(status.size());
            for (char c : status) lower_msg += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower_msg.find("network") != std::string::npos ||
                lower_msg.find("connect") != std::string::npos ||
                lower_msg.find("resolve") != std::string::npos ||
                lower_msg.find("timeout") != std::string::npos ||
                lower_msg.find("failed") != std::string::npos) {
                is_network_error = true;
            }

            if (is_network_error) {
                ImGui::TextColored(Colors::WarnAmber, "Offline / update server unreachable");
            } else {
                ImGui::TextColored(Colors::DangerRed, "Update failed");
            }
            ImGui::TextDisabled("%s", status.c_str());

            if (ImGui::SmallButton("Retry")) {
                std::thread([&updater]() { updater.checkForUpdate(); }).detach();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Check again")) {
                std::thread([&updater]() { updater.checkForUpdate(); }).detach();
            }
            break;
        }
        default:
            ImGui::TextDisabled("Unknown state");
            break;
        }
    }
    ImGui::EndChild();
}

}} // namespace hunter::gui