#include "gui/update_panel.h"
#include "core/updater.h"
#include "core/constants.h"
#include "imgui.h"

#include <thread>

namespace hunter { namespace gui {

void renderUpdatePanel() {
    using namespace hunter::core;

    SelfUpdateManager& updater = SelfUpdateManager::instance();
    SelfUpdateManager::UpdateState state = updater.state();
    std::string status = updater.statusMessage();
    float progress = updater.downloadProgress();
    ReleaseInfo release = updater.latestRelease();

    // Begin a child window for the panel with some padding and background
    ImGui::BeginChild("##update_panel", ImVec2(0, 0), false);
    {
        // Current version line
        ImGui::Text("Installed: v%s", constants::HUNTER_VERSION);
        ImGui::Separator();

        // Status and action area
        switch (state) {
        case SelfUpdateManager::UpdateState::Idle:
        case SelfUpdateManager::UpdateState::Checking: {
            ImGui::Text("Checking for updates...");
            // Simple spinner: we can use an animated text or a progress bar in indeterminate mode
            // We'll use a rotating text for simplicity
            static int spinner_counter = 0;
            const char* spinner_chars = "|/-\\";
            ImGui::SameLine();
            ImGui::Text("%c", spinner_chars[spinner_counter / 8 % 4]);
            spinner_counter++;

            // Check again button (small text button)
            if (ImGui::SmallButton("Check again")) {
                std::thread([&updater]() {
                    updater.checkForUpdate();
                }).detach();
            }
            break;
        }
        case SelfUpdateManager::UpdateState::UpToDate: {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "You're up to date");
            if (!release.tag_name.empty()) {
                ImGui::SameLine();
                ImGui::Text("(v%s)", release.tag_name.c_str());
            }
            // Check again button
            if (ImGui::SmallButton("Check again")) {
                std::thread([&updater]() {
                    updater.checkForUpdate();
                }).detach();
            }
            break;
        }
        case SelfUpdateManager::UpdateState::UpdateAvailable: {
            ImGui::Text("Update available: %s", release.tag_name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("View release notes")) {
                // We'll implement a simple popup or collapsible header for release notes
                // For now, we just open a popup
                ImGui::OpenPopup("release_notes");
            }
            if (ImGui::BeginPopup("release_notes")) {
                ImGui::TextUnformatted(release.body.c_str());
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            // Update Now button (primary)
            if (ImGui::Button("Update Now")) {
                std::thread([&updater]() {
                    updater.downloadAndApply();
                }).detach();
            }
            break;
        }
        case SelfUpdateManager::UpdateState::Downloading: {
            ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), "");
            ImGui::SameLine();
            ImGui::Text("%.0f%%", progress * 100.0f);
            ImGui::TextUnformatted(status.c_str());
            break;
        }
        case SelfUpdateManager::UpdateState::DownloadComplete:
        case SelfUpdateManager::UpdateState::Applying: {
            ImGui::TextUnformatted("Installing...");
            ImGui::SameLine();
            ImGui::TextUnformatted(status.c_str());
            break;
        }
        case SelfUpdateManager::UpdateState::UpdateComplete: {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Update installed — restart required");
            // Note: The manager already handles relaunch, so we just inform the user.
            break;
        }
        case SelfUpdateManager::UpdateState::Failed: {
            // Check if the status message indicates a network error
            bool is_network_error = false;
            const std::string& msg = status;
            // Convert to lowercase for case-insensitive check
            std::string lower_msg;
            lower_msg.reserve(msg.size());
            for (char c : msg) {
                lower_msg += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (lower_msg.find("network") != std::string::npos ||
                lower_msg.find("connect") != std::string::npos ||
                lower_msg.find("resolve") != std::string::npos ||
                lower_msg.find("timeout") != std::string::npos ||
                lower_msg.find("failed") != std::string::npos) {
                is_network_error = true;
            }

            if (is_network_error) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Offline / update server unreachable");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Update failed");
            }
            ImGui::TextUnformatted(status.c_str());

            // Retry button
            if (ImGui::SmallButton("Retry")) {
                std::thread([&updater]() {
                    updater.checkForUpdate();
                }).detach();
            }
            // Check again button (small text button)
            ImGui::SameLine();
            if (ImGui::SmallButton("Check again")) {
                std::thread([&updater]() {
                    updater.checkForUpdate();
                }).detach();
            }
            break;
        }
        default:
            ImGui::TextUnformatted("Unknown state");
            break;
        }
    }
    ImGui::EndChild();
}

}} // namespace hunter::gui