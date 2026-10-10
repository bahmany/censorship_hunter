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
    static const ImVec4 AccentBlue;      // originally kAccentBlue
    // Status colours
    static const ImVec4 SuccessGreen;    // originally kSuccessGreen
    static const ImVec4 WarnAmber;       // originally kWarnAmber
    static const ImVec4 DangerRed;       // originally kDangerRed
    // Text hierarchy
    static const ImVec4 TextPrimary;     // originally kTextPrimary
    static const ImVec4 TextSecondary;   // originally kTextSecondary
    static const ImVec4 TextDisabled;    // originally kTextDisabled
    // Background / surface colours
    static const ImVec4 BgPrimary;       // originally kBgPrimary
    static const ImVec4 BgSecondary;     // originally kBgSecondary
    static const ImVec4 BgTertiary;      // originally kBgTertiary
    // Borders
    static const ImVec4 BorderLight;     // originally kBorderLight
    static const ImVec4 BorderStrong;    // originally kBorderStrong
};

// ----- Breakpoint thresholds -----
constexpr float NarrowThreshold = 720.0f; // originally kNarrowThreshold

} // namespace Theme
} // namespace UI
