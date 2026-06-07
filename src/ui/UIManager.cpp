#include "ui/UIManager.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#include "util/AppPaths.h"
#include "util/Log.h"

bool UIManager::Init(SDL_Window* window, SDL_GLContext glContext) {
    (void)glContext;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Load both embedded default faces and let SetUiScale pick between them by
    // scale: the bitmap ProggyClean is pixel-perfect at integer scales (1x, 2x);
    // the scalable ProggyForever stays smooth at fractional scales (1.25x, 1.5x),
    // where the bitmap would blur. style.FontScaleDpi re-rasterizes whichever face
    // is active. The first-added face is the initial default (m_uiScale = 1.0).
    m_fontBitmap = io.Fonts->AddFontDefaultBitmap();
    m_fontVector = io.Fonts->AddFontDefaultVector();
    io.FontDefault = m_fontBitmap;

    // Subtitle overlay fonts, loaded from files bundled next to the executable
    // (see CMakeLists). The monospace UI fonts above are poor for prose; Roboto
    // (Apache-2.0) covers Latin/Cyrillic/Greek and a merged DroidSansFallback
    // (Apache-2.0) adds Chinese/Japanese/Korean. Rendered at an explicit pixel
    // size via ImDrawList::AddText; ImGui 1.92 + FreeType rasterizes glyphs on
    // demand, so no glyph-range table is needed. If a file is missing,
    // GetSubtitleFont() falls back to the default UI font.
    {
        constexpr float kSubtitleBaseSize = 32.0f;
        const char* base = SDL_GetBasePath();
        std::filesystem::path dir(base ? base : "");
        auto loadFont = [&](const char* file, bool merge) -> ImFont* {
            std::filesystem::path p = dir / file;
            std::error_code ec;
            if (!std::filesystem::exists(p, ec)) {
                LOG_WARN("Subtitle font missing: %s", p.string().c_str());
                return nullptr;
            }
            ImFontConfig cfg;
            cfg.MergeMode = merge;
            return io.Fonts->AddFontFromFileTTF(p.string().c_str(), kSubtitleBaseSize, &cfg);
        };
        m_fontSubtitle = loadFont("Roboto-Medium.ttf", /*merge=*/false);
        if (m_fontSubtitle)
            loadFont("DroidSansFallback.ttf", /*merge=*/true); // CJK fallback
    }

    m_iniPath = (GetAppDataDir() / "imgui.ini").string();
    io.IniFilename = m_iniPath.c_str();

    ImGui::StyleColorsDark();

    // Apply translucency to title bars so windows blend with the video behind them
    ImGuiStyle& style = ImGui::GetStyle();
    auto fade = [](ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); };
    style.Colors[ImGuiCol_TitleBg]          = fade(style.Colors[ImGuiCol_TitleBg],          0.85f);
    style.Colors[ImGuiCol_TitleBgActive]    = fade(style.Colors[ImGuiCol_TitleBgActive],    0.85f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = fade(style.Colors[ImGuiCol_TitleBgCollapsed], 0.85f);

    // Use DelayNormal (0.4s) instead of DelayShort (0.15s) for mouse-driven
    // tooltips — short feels jumpy when scanning over the toolbar.
    style.HoverFlagsForTooltipMouse =
        (style.HoverFlagsForTooltipMouse & ~ImGuiHoveredFlags_DelayShort) | ImGuiHoveredFlags_DelayNormal;

    // Snapshot the unscaled style so SetUiScale can re-apply it cleanly.
    m_baseStyle = style;

    if (!ImGui_ImplSDL3_InitForOpenGL(window, glContext))
        return false;
    if (!ImGui_ImplOpenGL3_Init("#version 150"))
        return false;

    return true;
}

void UIManager::SetUiScale(float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    if (scale == m_uiScale) return;

    float prevScale = m_uiScale;
    m_uiScale = scale;

    // ImGui 1.92+ uses dynamic font rasterization. Final font size is
    //   FontSizeBase * FontScaleMain * FontScaleDpi * ...
    // Fonts are re-rasterized on demand at the exact rendered size, so text
    // stays crisp when FontScaleDpi changes. ScaleAllSizes() handles
    // padding/spacing/thickness but explicitly does NOT scale fonts — we use
    // FontScaleDpi for that.
    ImGuiStyle& style = ImGui::GetStyle();
    style = m_baseStyle;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    // Pick the face that stays sharp at this scale: the bitmap ProggyClean is
    // pixel-perfect only at integer scales; otherwise use the scalable
    // ProggyForever, which the dynamic rasterizer keeps smooth.
    bool integerScale = std::fabs(scale - std::round(scale)) < 0.01f;
    ImFont* desired = integerScale ? m_fontBitmap : m_fontVector;
    if (desired)
        ImGui::GetIO().FontDefault = desired;

    // Rescale open floating windows' sizes by the UI-scale ratio so they keep
    // the same visual footprint. Keep each window centered on its previous center
    // so it grows/shrinks in place rather than off to one corner.
    float ratio = scale / prevScale;
    if (ratio != 1.0f) {
        const char* floatingWindows[] = { "Timeline", "Marks", "Help" };
        for (const char* name : floatingWindows) {
            ImGuiWindow* w = ImGui::FindWindowByName(name);
            if (!w || w->DockId != 0) continue;
            ImVec2 center(w->Pos.x + w->SizeFull.x * 0.5f,
                          w->Pos.y + w->SizeFull.y * 0.5f);
            // The Timeline's width tracks the viewport (App's proportional
            // repositioning owns it), so only its height scales with the UI here.
            // Scaling its width too would fight that and shrink its relative
            // width whenever the scale drops without a matching window resize.
            bool scaleWidth = (strcmp(name, "Timeline") != 0);
            if (scaleWidth)
                w->SizeFull.x *= ratio;
            w->SizeFull.y *= ratio;
            w->Size = w->SizeFull;
            w->Pos = ImVec2(center.x - w->SizeFull.x * 0.5f,
                            center.y - w->SizeFull.y * 0.5f);
        }
    }
}

void UIManager::Shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void UIManager::BeginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    SetupDockspace();
}

void UIManager::EndFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void UIManager::SetupDockspace() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGuiID dockspaceId = ImGui::GetID("MainDockspace");

    // Build default layout on first run (no saved layout yet)
    if (!m_layoutInitialized) {
        m_layoutInitialized = true;

        if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

            // Viewport fills the entire dockspace, no splits
            ImGui::DockBuilderDockWindow("Viewport", dockspaceId);
            ImGui::DockBuilderFinish(dockspaceId);
        }
    }

    // Suppress the tab bar on the Viewport's node — we never want a
    // "Viewport" tab. Other docked windows show their tab bar by default
    // (user can right-click a tab → "Hide Tab Bar" to hide; that choice
    // persists in imgui.ini per dock node).
    if (ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId)) {
        centralNode->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
    }

    // When non-viewport windows are docked, the root node is split; reserve
    // the menu bar's height so docked windows don't get clipped under it.
    // When only the viewport is docked, cover the full viewport so video
    // renders behind the (overlay) menu bar — the intended look.
    ImGuiDockNode* rootNode = ImGui::DockBuilderGetNode(dockspaceId);
    bool hasDockedSiblings = rootNode && !rootNode->IsLeafNode();

    ImVec2 dockPos  = hasDockedSiblings ? viewport->WorkPos  : viewport->Pos;
    ImVec2 dockSize = hasDockedSiblings ? viewport->WorkSize : viewport->Size;
    ImGui::SetNextWindowPos(dockPos);
    ImGui::SetNextWindowSize(dockSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("DockSpace", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f),
        ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();
}

void UIManager::ResetLayout() {
    DeleteLayoutFile();
    ImGui::ClearIniSettings();
    ImGuiID dockspaceId = ImGui::GetID("MainDockspace");
    ImGui::DockBuilderRemoveNode(dockspaceId);

    // Don't free ImGuiWindow objects as several internal containers still
    // reference them. m_layoutResetPending forces ImGuiCond_Always for one
    // frame so SetNextWindowPos/Size in App::Render() reposition them.
    m_layoutInitialized = false;
    m_layoutResetPending = true;
}

void UIManager::DeleteLayoutFile() {
    std::filesystem::remove(m_iniPath);
}

bool UIManager::IsLayoutResetPending() {
    if (m_layoutResetPending) {
        m_layoutResetPending = false;
        return true;
    }
    return false;
}
