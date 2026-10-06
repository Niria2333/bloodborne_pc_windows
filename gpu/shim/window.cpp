// Windows port modifications by yaonikaixin999999, 2026-10-05.
// bbport: SDL3 window for the Vulkan swapchain (Win32, X11 or Wayland).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"

namespace Frontend {

namespace {

void SetFullscreenRefresh(SDL_Window* window) {
    const char* requested = std::getenv("BB_FULLSCREEN_REFRESH_HZ");
    if (!requested || !requested[0]) {
        return;
    }
    char* end = nullptr;
    const float refresh = std::strtof(requested, &end);
    if (end == requested || *end || !std::isfinite(refresh) || refresh <= 0.0f) {
        LOG_WARNING(Frontend, "Invalid fullscreen refresh rate {}; retaining desktop mode", requested);
        return;
    }

    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* current = SDL_GetCurrentDisplayMode(display);
    if (!current) {
        LOG_WARNING(Frontend, "Cannot query fullscreen display mode: {}", SDL_GetError());
        return;
    }
    const SDL_DisplayMode desktop = *current;
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
    const SDL_DisplayMode* closest = nullptr;
    float distance = 0.5f;
    for (int i = 0; modes && i < count; ++i) {
        const SDL_DisplayMode* mode = modes[i];
        const float difference = std::abs(mode->refresh_rate - refresh);
        if (mode->w == desktop.w && mode->h == desktop.h && difference <= distance &&
            (!closest || difference < distance || mode->format == desktop.format)) {
            closest = mode;
            distance = difference;
        }
    }
    if (!closest) {
        SDL_free(modes);
        LOG_WARNING(Frontend, "Fullscreen {}x{} at {} Hz is unsupported; retaining desktop mode",
                    desktop.w, desktop.h, refresh);
        return;
    }
    const SDL_DisplayMode selected = *closest;
    const bool applied = SDL_SetWindowFullscreenMode(window, &selected);
    SDL_free(modes);
    if (!applied) {
        LOG_WARNING(Frontend, "Cannot select fullscreen refresh rate: {}", SDL_GetError());
        if (!SDL_SetWindowFullscreenMode(window, nullptr) || !SDL_SyncWindow(window)) {
            LOG_WARNING(Frontend, "Cannot restore desktop fullscreen mode: {}", SDL_GetError());
        }
        return;
    }
    if (!SDL_SyncWindow(window)) {
        LOG_WARNING(Frontend, "Cannot synchronize fullscreen refresh rate: {}", SDL_GetError());
        if (!SDL_SetWindowFullscreenMode(window, nullptr) || !SDL_SyncWindow(window)) {
            LOG_WARNING(Frontend, "Cannot restore desktop fullscreen mode: {}", SDL_GetError());
        }
        return;
    }
    const SDL_DisplayMode* actual = SDL_GetCurrentDisplayMode(display);
    if (actual) {
        LOG_INFO(Frontend, "Exclusive fullscreen display {}x{} at {} Hz (requested {} Hz)",
                 actual->w, actual->h, actual->refresh_rate, refresh);
    }
}

} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen && fullscreen[0] == '1');
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
    if (driver && !std::strcmp(driver, "windows")) {
        if (fullscreen && fullscreen[0] == '1') {
            SetFullscreenRefresh(window);
        }
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        ASSERT_MSG(window_info.render_surface, "SDL did not provide a Win32 window handle");
    } else if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    if (SDL_GetWindowFullscreenMode(window)) {
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        if (SDL_SetWindowFullscreenMode(window, nullptr) && SDL_SyncWindow(window)) {
            const SDL_DisplayMode* restored = SDL_GetCurrentDisplayMode(display);
            if (restored) {
                LOG_INFO(Frontend, "Restored desktop display {}x{} at {} Hz",
                         restored->w, restored->h, restored->refresh_rate);
            }
        } else {
            LOG_WARNING(Frontend, "Cannot synchronize desktop display restoration: {}", SDL_GetError());
        }
    }
    SDL_DestroyWindow(window);
}

bool WindowSDL::PollEvents() {
    BbOverlay::UpdateTextInput(window);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    return is_open;
}

} // namespace Frontend
