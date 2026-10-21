#pragma once

#include "imgui.h"

namespace UI {
namespace Theme {

// ----- Spacing tokens (in logical pixels) -----
struct Spacing {
    static constexpr float Small  = 6.0f;   // originally kSpaceS
    static constexpr float Medium = 12.0f; // originally kSpaceM
    static constexpr float Large  = 20.0f; // originally kSpaceL
};

// ----- Corner radius token -----
constexpr float CornerRadius = 6.0f; // originally kRadius

// ----- Semantic colour palette (ImGui style uses float RGBA) -----
struct Colors {
    // Primary accent used for interactive elements
    static constexpr ImVec4 AccentBlue{0.31f, 0.55f, 1.00f, 1.0f};      // originally kAccentBlue
    // Status colours
    static constexpr ImVec4 SuccessGreen{0.30f, 0.80f, 0.40f, 1.0f};    // originally kSuccessGreen
    static constexpr ImVec4 WarnAmber{1.00f, 0.75f, 0.15f, 1.0f};       // originally kWarnAmber
    static constexpr ImVec4 DangerRed{0.95f, 0.30f, 0.30f, 1.0f};       // originally kDangerRed
    // Text hierarchy
    static constexpr ImVec4 TextPrimary{0.92f, 0.94f, 0.96f, 1.0f};     // originally kTextPrimary
    static constexpr ImVec4 TextSecondary{0.55f, 0.60f, 0.65f, 1.0f};   // originally kTextSecondary
    static constexpr ImVec4 TextDisabled{0.35f, 0.40f, 0.45f, 1.0f};    // originally kTextDisabled
    // Background / surface colours
    static constexpr ImVec4 BgPrimary{0.05f, 0.06f, 0.08f, 1.0f};       // originally kBgPrimary
    static constexpr ImVec4 BgSecondary{0.07f, 0.08f, 0.10f, 1.0f};     // originally kBgSecondary
    static constexpr ImVec4 BgTertiary{0.09f, 0.10f, 0.12f, 1.0f};      // originally kBgTertiary
    // Borders
    static constexpr ImVec4 BorderLight{0.15f, 0.18f, 0.22f, 0.40f};     // originally kBorderLight
    static constexpr ImVec4 BorderStrong{0.18f, 0.22f, 0.26f, 1.0f};    // originally kBorderStrong
};

// ----- Breakpoint thresholds -----
constexpr float NarrowThreshold = 720.0f; // originally kNarrowThreshold

} // namespace Theme
} // namespace UI
