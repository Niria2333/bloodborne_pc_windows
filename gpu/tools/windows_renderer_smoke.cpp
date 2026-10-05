// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
// Initializes the actual Win32 window, Vulkan device and renderer without game assets.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "../bbgpu.h"
#include "sdl_window.h"

extern "C" void runtime_restart() { std::_Exit(2); }
extern Frontend::WindowSDL* g_window;

int main(int argc, char** argv) {
    const int width = argc > 1 ? std::atoi(argv[1]) : 1280;
    const int height = argc > 2 ? std::atoi(argv[2]) : 720;
    if (width < 320 || height < 240 || width > 7680 || height > 4320) return 2;
    BbGpuConfig config{};
    config.title = "Bloodborne Windows renderer check";
    config.serial = "BBTEST";
    config.width = width;
    config.height = height;
    bbgpu_register_kernel();
    if (bbgpu_init(&config)) return 1;
    const bool correct_size = g_window && g_window->GetWidth() == width && g_window->GetHeight() == height;
    std::printf("Windows renderer: Win32 window, Vulkan device and swapchain init PASS (%dx%d); no game assets\n",
                width, height);
    std::printf("Windows renderer: requested pixel size %s (actual %dx%d)\n",
                correct_size ? "PASS" : "FAIL", g_window->GetWidth(), g_window->GetHeight());
    std::fflush(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // Renderer workers are process-lifetime objects, as in bb-probe.
    std::_Exit(correct_size ? 0 : 1);
}
