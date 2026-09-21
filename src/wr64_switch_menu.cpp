#include "wr64_switch_menu.hpp"

#include "wr64_renderer.hpp"

#ifdef WR64_SWITCH

#include <algorithm>
#include <cstdio>
#include <cstdint>

#include <switch.h>

namespace wr64::switch_menu {
namespace {

struct MenuState {
    int selected = 0;
    int scale = 0;
    int refresh = 2;
    int filtering = 0;
    int downsample = 0;
};

MenuState state;

template <typename T>
void cycle(T &value, int delta, int count) {
    value = (value + delta + count) % count;
}

void load_settings(const std::filesystem::path& path) {
    if (FILE* file = std::fopen((path / "switch-menu.cfg").string().c_str(), "rb")) {
        int scale = 0;
        int refresh = 2;
        int filtering = 0;
        int downsample = 0;
        if (std::fscanf(file, "%d %d %d %d", &scale, &refresh, &filtering, &downsample) == 4) {
            state.scale = std::clamp(scale, 0, 2);
            state.refresh = std::clamp(refresh, 0, 2);
            state.filtering = std::clamp(filtering, 0, 2);
            state.downsample = std::clamp(downsample, 0, 1);
        }
        std::fclose(file);
    }
}

void save_settings(const std::filesystem::path& path) {
    if (FILE* file = std::fopen((path / "switch-menu.cfg").string().c_str(), "wb")) {
        std::fprintf(file, "%d %d %d %d\n",
            state.scale, state.refresh, state.filtering, state.downsample);
        std::fclose(file);
    }
}

void draw_console() {
    static constexpr const char *scale_labels[] = { "1x (faster)", "2x", "3x" };
    static constexpr const char *refresh_labels[] = { "20 Hz (native)", "30 Hz", "60 Hz" };
    static constexpr const char *filter_labels[] = { "Nearest", "Linear", "Pixel Scaling" };
    static constexpr const char *downsample_labels[] = { "1x", "2x" };
    const char *values[] = {
        scale_labels[state.scale], refresh_labels[state.refresh],
        filter_labels[state.filtering], downsample_labels[state.downsample], "", ""
    };
    static constexpr const char *rows[] = {
        "Resolution", "Frame rate", "Filtering", "Downsample", "START GAME", "Exit"
    };

    std::printf("\x1b[2J\x1b[H");
    std::printf("\x1b[36;1m                 WAVE RACE 64\x1b[0m\n");
    std::printf("                 RaceWave46\n\n");
    std::printf(" Configure the port before loading the game.\n\n");
    for (int row = 0; row < 6; ++row) {
        const bool selected = state.selected == row;
        std::printf(selected ? "\x1b[43;30m > %-18s %-22s \x1b[0m\n"
                             : "   %-18s %-22s\n",
            rows[row], values[row]);
    }
    std::printf("\n D-Pad: navigate/change    A: select\n");
    std::printf(" Plus: start game          B: exit\n");
    consoleUpdate(nullptr);
}

} // namespace

bool run_preboot(const std::filesystem::path& config_path) {
    load_settings(config_path);
    state.selected = 0;
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad{};
    padInitializeDefault(&pad);

    bool running = true;
    bool start = false;
    draw_console();
    while (running && appletMainLoop()) {
        padUpdate(&pad);
        const uint64_t pressed = padGetButtonsDown(&pad);
        if (pressed & HidNpadButton_Up) state.selected = (state.selected + 5) % 6;
        if (pressed & HidNpadButton_Down) state.selected = (state.selected + 1) % 6;

        const int direction = (pressed & HidNpadButton_Right) ? 1 :
            ((pressed & HidNpadButton_Left) ? -1 : 0);
        if (direction != 0) {
            switch (state.selected) {
            case 0: cycle(state.scale, direction, 3); break;
            case 1: cycle(state.refresh, direction, 3); break;
            case 2: cycle(state.filtering, direction, 3); break;
            case 3: cycle(state.downsample, direction, 2); break;
            default: break;
            }
        }

        if (pressed & HidNpadButton_A) {
            if (state.selected < 4) {
                switch (state.selected) {
                case 0: cycle(state.scale, 1, 3); break;
                case 1: cycle(state.refresh, 1, 3); break;
                case 2: cycle(state.filtering, 1, 3); break;
                case 3: cycle(state.downsample, 1, 2); break;
                }
            }
            else {
                start = state.selected == 4;
                running = false;
            }
        }
        if (pressed & HidNpadButton_Plus) {
            start = true;
            running = false;
        }
        if (pressed & HidNpadButton_B) running = false;

        if (running) draw_console();
        svcSleepThread(16'666'667L);
    }
    if (start) save_settings(config_path);
    consoleExit(nullptr);
    return start;
}

void apply_selected() {
    wr64::renderer::switch_menu_apply_settings(
        state.scale, state.refresh, state.filtering, state.downsample);
}

} // namespace wr64::switch_menu

#else

namespace wr64::switch_menu {
bool run_preboot(const std::filesystem::path&) { return true; }
void apply_selected() {}
} // namespace wr64::switch_menu

#endif
