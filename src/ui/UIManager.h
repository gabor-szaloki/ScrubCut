#pragma once

#include <SDL3/SDL.h>
#include <imgui.h>
#include <string>

class UIManager {
public:
    bool Init(SDL_Window* window, SDL_GLContext glContext);
    void Shutdown();

    void BeginFrame();
    void EndFrame();
    void ResetLayout();
    void DeleteLayoutFile();
    bool IsLayoutResetPending();

    // Apply the effective UI scale to style and fonts. Call on init, when the
    // window moves to a display with different DPI, or when the user changes
    // the UI scale. The value already folds in DPI (see App::GetEffectiveUiScale).
    void SetUiScale(float scale);
    float GetUiScale() const { return m_uiScale; }

private:
    void SetupDockspace();
    bool m_layoutInitialized = false;
    bool m_layoutResetPending = false;

    std::string m_iniPath;

    // Original (1.0x) style, used as a base when rescaling.
    ImGuiStyle m_baseStyle;
    float m_uiScale = 1.0f;

    // Two default faces, swapped by SetUiScale: the bitmap ProggyClean stays
    // pixel-perfect at integer scales, the scalable ProggyForever stays smooth
    // at fractional scales.
    ImFont* m_fontBitmap = nullptr;
    ImFont* m_fontVector = nullptr;
};
