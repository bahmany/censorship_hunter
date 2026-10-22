// UI probe: imgui test-engine hooks that dump every labelled item's screen
// rectangle to $HUNTER_UI_PROBE_OUT once per frame. Built only with
// -DHUNTER_UI_PROBE=ON; drives tools/ui_probe/click_all.py.
#include "imgui.h"
#include "imgui_internal.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace {
struct Item { ImRect bb; std::string label; std::string window; };
std::unordered_map<ImGuiID, Item> g_items;
int g_frame = -1;
double g_last_dump = 0.0;

void dumpIfDue(ImGuiContext* ctx) {
    if (ctx->Time - g_last_dump < 0.25) return;
    g_last_dump = ctx->Time;
    const char* path = std::getenv("HUNTER_UI_PROBE_OUT");
    if (!path) return;
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    for (const auto& kv : g_items) {
        const Item& it = kv.second;
        if (it.label.empty()) continue;
        std::fprintf(f, "%.0f\t%.0f\t%.0f\t%.0f\t%s\t%s\n", it.bb.Min.x, it.bb.Min.y,
                     it.bb.GetWidth(), it.bb.GetHeight(), it.window.c_str(), it.label.c_str());
    }
    std::fclose(f);
    std::rename(tmp.c_str(), path);
}
}  // namespace

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
    if (ctx->FrameCount != g_frame) {
        dumpIfDue(ctx);
        g_items.clear();
        g_frame = ctx->FrameCount;
    }
    Item& it = g_items[id];
    it.bb = bb;
    it.window = ctx->CurrentWindow ? ctx->CurrentWindow->Name : "";
    // Tab items never report a label through ItemInfo; resolve it here.
    if (ImGuiTabBar* bar = ctx->CurrentTabBar) {
        if (ImGuiTabItem* tab = ImGui::TabBarFindTabByID(bar, id)) it.label = ImGui::TabBarGetTabName(bar, tab);
    }
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    auto f = g_items.find(id);
    if (f != g_items.end() && label) f->second.label = label;
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
