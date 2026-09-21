#pragma once

#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace wr64::renderer {

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
    uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode);

// Switch-only pre-game launcher settings. Harmless no-ops on desktop.
void switch_menu_get_settings(int* scale, int* refresh, int* filtering, int* downsample);
void switch_menu_apply_settings(int scale, int refresh, int filtering, int downsample);

} // namespace wr64::renderer
