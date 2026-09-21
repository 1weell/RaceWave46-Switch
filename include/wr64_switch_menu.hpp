#pragma once

#include <filesystem>

namespace wr64::switch_menu {

// Runs before SDL/RT64 takes ownership of the Switch display. Keeping the
// launcher on libnx's simple console framebuffer avoids sharing Vulkan and
// ImGui state with RT64's present thread. Returns false when the user exits.
bool run_preboot(const std::filesystem::path& config_path);

// Applies the selection after RT64 has loaded its native configuration.
void apply_selected();

} // namespace wr64::switch_menu
