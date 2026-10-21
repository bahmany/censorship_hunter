#include "ui/theme.hpp"

namespace UI {
namespace Theme {

// Define colour constants – modern dark theme with accent #4F8CFF
const ImVec4 Colors::AccentBlue   = ImVec4(0.31f, 0.55f, 1.00f, 1.0f);  // #4F8CFF
const ImVec4 Colors::SuccessGreen = ImVec4(0.30f, 0.80f, 0.40f, 1.0f);
const ImVec4 Colors::WarnAmber    = ImVec4(1.00f, 0.75f, 0.15f, 1.0f);  // #FFBF26
const ImVec4 Colors::DangerRed    = ImVec4(0.95f, 0.30f, 0.30f, 1.0f);  // #F24D4D

const ImVec4 Colors::TextPrimary   = ImVec4(0.92f, 0.94f, 0.96f, 1.0f);
const ImVec4 Colors::TextSecondary = ImVec4(0.55f, 0.60f, 0.65f, 1.0f);
const ImVec4 Colors::TextDisabled  = ImVec4(0.35f, 0.40f, 0.45f, 1.0f);

const ImVec4 Colors::BgPrimary    = ImVec4(0.05f, 0.06f, 0.08f, 1.0f);  // near-black
const ImVec4 Colors::BgSecondary  = ImVec4(0.07f, 0.08f, 0.10f, 1.0f);
const ImVec4 Colors::BgTertiary   = ImVec4(0.09f, 0.10f, 0.12f, 1.0f);

const ImVec4 Colors::BorderLight  = ImVec4(0.15f, 0.18f, 0.22f, 0.40f);
const ImVec4 Colors::BorderStrong = ImVec4(0.18f, 0.22f, 0.26f, 1.0f);

} // namespace Theme
} // namespace UI
