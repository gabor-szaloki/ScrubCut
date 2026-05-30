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

    // Apply DPI scale to style and fonts. Call on init and when the window
    // moves to a display with different DPI.
    void SetDpiScale(float scale);
    float GetDpiScale() const { return m_dpiScale; }

private:
    void SetupDockspace();
    bool m_layoutInitialized = false;
    bool m_layoutResetPending = false;

    std::string m_iniPath;

    // Original (1.0x) style, used as a base when rescaling for DPI.
    ImGuiStyle m_baseStyle;
    float m_dpiScale = 1.0f;

    // Two default faces, swapped by SetDpiScale: the bitmap ProggyClean stays
    // pixel-perfect at integer scales, the scalable ProggyForever stays smooth
    // at fractional scales.
    ImFont* m_fontBitmap = nullptr;
    ImFont* m_fontVector = nullptr;
};
