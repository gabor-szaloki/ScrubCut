// macOS borderless fullscreen on NSWindow. Built with ARC like every .mm in src/.
#include "util/MacFullscreen.h"
#include "util/Log.h"

#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>

namespace {

// Pre-fullscreen state of the (single) app window, restored on exit.
struct SavedWindowState {
    bool valid = false;
    NSRect frame = NSZeroRect;
    NSWindowStyleMask styleMask = 0;
    BOOL hasShadow = YES;
    NSWindowCollectionBehavior collectionBehavior = NSWindowCollectionBehaviorDefault;
    NSString* title = nil;
};
SavedWindowState g_saved;

NSWindow* CocoaWindow(SDL_Window* window) {
    if (!window) return nil;
    return (__bridge NSWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
}

// SDL routes events through the content view's next responder; a style-mask
// change can reset that link, so detach and reattach it (as SDL itself does).
void SetStyleMaskKeepingResponderChain(NSWindow* window, NSWindowStyleMask mask) {
    NSView* content = window.contentView;
    NSResponder* listener = content.nextResponder;
    const bool detach = listener && ![listener isKindOfClass:[NSView class]];
    if (detach) content.nextResponder = nil;
    window.styleMask = mask;
    if (detach && content.nextResponder != listener) content.nextResponder = listener;
}

}  // namespace

bool MacWindowInFullscreenSpace(SDL_Window* sdlWindow) {
    @autoreleasepool {
        NSWindow* window = CocoaWindow(sdlWindow);
        return window && (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    }
}

void MacSetBorderlessFullscreen(SDL_Window* sdlWindow, bool fullscreen) {
    @autoreleasepool {
        NSWindow* window = CocoaWindow(sdlWindow);
        if (!window) return;
        // Changing the style mask of a window in a fullscreen Space throws;
        // App::SetFullscreen routes those through SDL, this is the backstop.
        if (window.styleMask & NSWindowStyleMaskFullScreen) {
            LOG_WARN("Window is in a fullscreen Space; borderless fullscreen %s skipped",
                     fullscreen ? "entry" : "exit");
            return;
        }
        if (fullscreen == g_saved.valid) return;

        if (fullscreen) {
            g_saved.frame = window.frame;
            g_saved.styleMask = window.styleMask;
            g_saved.hasShadow = window.hasShadow;
            g_saved.collectionBehavior = window.collectionBehavior;
            g_saved.title = [window.title copy];
            g_saved.valid = true;

            // Auto-hide before setting the frame: with the menu bar showing,
            // AppKit would constrain the window below it.
            NSApp.presentationOptions =
                NSApplicationPresentationAutoHideMenuBar | NSApplicationPresentationAutoHideDock;
            SetStyleMaskKeepingResponderChain(window, NSWindowStyleMaskBorderless);
            window.hasShadow = NO;  // draws a 1px halo around a screen-sized window on Tahoe
            // Keeps Ctrl+Cmd+F (SDL's Toggle Full Screen menu item) from pulling
            // the borderless window into a Space.
            window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;

            NSScreen* screen = window.screen ?: NSScreen.mainScreen;
            [window setFrame:(screen ? screen.frame : g_saved.frame) display:YES];
            [window makeKeyAndOrderFront:nil];
        } else {
            // Presentation options first: SDL derives MAXIMIZED / RESTORED from
            // AppKit's zoom check, which compares against the normal visible
            // area — restoring a zoomed frame under an auto-hidden menu bar
            // would read as not zoomed.
            NSApp.presentationOptions = NSApplicationPresentationDefault;
            SetStyleMaskKeepingResponderChain(window, g_saved.styleMask);
            if (g_saved.title) window.title = g_saved.title;  // cleared by the style change
            window.hasShadow = g_saved.hasShadow;
            window.collectionBehavior = g_saved.collectionBehavior;
            [window setFrame:g_saved.frame display:YES];
            [window makeKeyAndOrderFront:nil];
            g_saved = SavedWindowState{};
        }
    }
}
