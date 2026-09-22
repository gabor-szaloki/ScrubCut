#pragma once

#ifdef __APPLE__

struct SDL_Window;

// Borderless fullscreen on Cocoa: borderless style mask, screen-sized frame,
// auto-hidden menu bar and Dock. No Space, no animation. Exit restores the
// exact pre-entry frame, style and title. Synchronous resize — call between
// frames only. Windows counterpart: the Win32 branch of App::SetFullscreen.
void MacSetBorderlessFullscreen(SDL_Window* window, bool fullscreen);

// True while the window is in (or transitioning to / from) a native fullscreen
// Space — the green button's fullscreen, managed by AppKit.
bool MacWindowInFullscreenSpace(SDL_Window* window);

#endif
