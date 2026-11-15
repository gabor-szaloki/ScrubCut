#pragma once
#include <algorithm>

// Visible part of the video frame, in normalized texture coords.
//
// Zoom is relative to the "fitted" frame (the whole video letter/pillar-boxed
// into the canvas): 1 = fitted, 16 = 16x that. The shown area always has the
// proportions of the CANVAS (the video panel), not of the video: zooming in
// first widens the part of the video that is visible until it fills the canvas
// (the bars disappear), then crops. Tall video in a wide panel therefore shows
// a full-width band that narrows as you zoom.
struct ViewCrop {
    static constexpr float kMinZoom = 1.0f;
    static constexpr float kMaxZoom = 16.0f;

    float cx = 0.5f;    // centre of the visible area, UV [0..1]
    float cy = 0.5f;
    float zoom = 1.0f;

    // Canvas size / fitted-frame size at zoom 1 (>= 1; one of them is exactly 1).
    // Set every frame by the video panel via SetCanvasRatio().
    float rx = 1.0f;
    float ry = 1.0f;

    // Half-size of the visible area in UV. Capped at 0.5: when the canvas is
    // larger than the zoomed frame on an axis, that axis shows all of the video.
    float HalfX() const { return std::min(0.5f, 0.5f * rx / zoom); }
    float HalfY() const { return std::min(0.5f, 0.5f * ry / zoom); }

    bool IsZoomed() const { return zoom > 1.0001f; }

    void SetCanvasRatio(float x, float y) {
        rx = std::max(1.0f, x);
        ry = std::max(1.0f, y);
        Clamp();
    }

    // Keep the visible area inside the frame. When a change makes it poke out,
    // the centre is pushed back in; on an axis where all of the video is shown
    // the centre is forced to 0.5.
    void Clamp() {
        zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
        const float hx = HalfX(), hy = HalfY();
        cx = std::clamp(cx, hx, 1.0f - hx);
        cy = std::clamp(cy, hy, 1.0f - hy);
    }

    // Scale around the centre. factor > 1 zooms in, < 1 zooms out.
    void ZoomBy(float factor) { zoom *= factor; Clamp(); }

    void PanTo(float x, float y) { cx = x; cy = y; Clamp(); }

    float U0() const { return cx - HalfX(); }
    float V0() const { return cy - HalfY(); }
    float U1() const { return cx + HalfX(); }
    float V1() const { return cy + HalfY(); }

    // Back to the fitted frame. rx / ry belong to the panel, not the view.
    void Reset() { cx = 0.5f; cy = 0.5f; zoom = 1.0f; }
};
