#include "wr64_window.hpp"

#ifdef WR64_ENABLE_RT64

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdio>
#include <cstdint>

#ifdef __SWITCH__
extern "C" void wr64_switch_trace(const char* message);
#define WR64_SWITCH_TRACE(message) wr64_switch_trace(message)
#else
#define WR64_SWITCH_TRACE(message) do { } while (0)
#endif

namespace wr64::window {
namespace {

bool enabled = false;

struct WindowState {
    SDL_Window* window = nullptr;
};

} // namespace

void set_enabled(bool value) {
    enabled = value;
}

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    return new WindowState{};
}

ultramodern::renderer::WindowHandle create_window(
    ultramodern::gfx_callbacks_t::gfx_data_t data) {
    auto* state = static_cast<WindowState*>(data);
    if (!enabled) {
        return nullptr;
    }

    SDL_SetMainReady();
    // A Switch SDL video window is backed by the same default NWindow that
    // vkCreateViSurfaceNN uses. Creating one here makes SDL the first
    // producer of that NWindow, and the later Vulkan swapchain creation then
    // fails with VK_ERROR_INITIALIZATION_FAILED (-3). RT64's Switch Vulkan
    // path does not read an SDL_Window at all; it presents directly to the
    // default NWindow. Keep SDL's event subsystem for the normal event pump,
    // but leave the native display exclusively to Vulkan.
    if (SDL_Init(SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "[switch-window] SDL_Init failed: %s\n", SDL_GetError());
        WR64_SWITCH_TRACE("[switch-window] SDL_Init failed");
        return nullptr;
    }

    // The renderer only needs a non-null opaque handle to select the supplied
    // window path. Do not pass a real SDL_Window here: that would claim the
    // display NWindow before RT64 creates its Vulkan VI surface.
    state->window = reinterpret_cast<SDL_Window*>(uintptr_t(1));
    WR64_SWITCH_TRACE("[switch-window] SDL events ready; Vulkan owns default NWindow");

    return state->window;
}

void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t data) {
    auto* state = static_cast<WindowState*>(data);
    if ((state == nullptr) || (state->window == nullptr)) {
        return;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if ((event.type == SDL_QUIT) ||
            ((event.type == SDL_WINDOWEVENT) &&
             (event.window.event == SDL_WINDOWEVENT_CLOSE))) {
            ultramodern::quit();
        }
    }
}

} // namespace wr64::window

#endif
