#include "ui/theme.hpp"

namespace UI {
namespace Theme {

// Define colour constants – using the same values that were previously hard‑coded in gui/app.cpp
const ImVec4 Colors::AccentBlue   = ImVec4(0.20f, 0.36f, 0.60f, 1.0f);
const ImVec4 Colors::SuccessGreen = ImVec4(0.30f, 0.80f, 0.40f, 1.0f);
const ImVec4 Colors::WarnAmber    = ImVec4(0.90f, 0.78f, 0.25f, 1.0f);
const ImVec4 Colors::DangerRed    = ImVec4(0.85f, 0.35f, 0.35f, 1.0f);

const ImVec4 Colors::TextPrimary   = ImVec4(0.90f, 0.92f, 0.95f, 1.0f);
const ImVec4 Colors::TextSecondary = ImVec4(0.50f, 0.54f, 0.58f, 1.0f);
const ImVec4 Colors::TextDisabled  = ImVec4(0.30f, 0.34f, 0.38f, 1.0f);

const ImVec4 Colors::BgPrimary    = ImVec4(0.06f, 0.07f, 0.09f, 1.0f);
const ImVec4 Colors::BgSecondary  = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
const ImVec4 Colors::BgTertiary   = ImVec4(0.10f, 0.11f, 0.13f, 1.0f);

const ImVec4 Colors::BorderLight  = ImVec4(0.18f, 0.20f, 0.24f, 0.50f);
const ImVec4 Colors::BorderStrong = ImVec4(0.18f, 0.20f, 0.24f, 1.0f);

} // namespace Theme
} // namespace UI
