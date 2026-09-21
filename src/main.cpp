#include "wr64_diagnostic_policy.h"
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <exception>
#include <mutex>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <cstdarg>

#ifdef WR64_SWITCH
#include <switch.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "wr64_runtime.hpp"
#include "wr64_composer_window.hpp"
#include "wr64_io.hpp"
#include "wr64_renderer.hpp"
#include "wr64_switch_menu.hpp"
#include "wr64_rom_patch.hpp"
#include "wr64_window.hpp"
#include "wr64_forensic.h"
#include "wr64_frontend.hpp"
#include "wr64_input.hpp"
#include "wr64_editions.hpp"
#include "wr64_game_rumble.hpp"
#include "wr64_controller_smoke.hpp"
#include "wr64_ghost.hpp"
#include "wr64_ghost_native.h"
#include "wr64_achievements.hpp"
#include "wr64_achievement_badges.hpp"
#include "wr64_achievements_native.hpp"

#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
extern RspUcodeFunc aspMain;
extern std::atomic_bool exited;
gpr get_entrypoint_address();

namespace {

#ifdef WR64_SWITCH
// stderr from an NRO is not normally visible after hbmenu hands control to
// the application. Keep a tiny append-only boot trace on the SD card so a
// crash can be located without a debugger attached to the console.
FILE* switch_boot_log = nullptr;
std::mutex switch_boot_log_mutex;
// Keep the constructor probe side-effect free.  libnx's filesystem services
// are not a safe dependency during the earliest .init_array callbacks.
volatile bool switch_early_boot_reached = false;

void switch_boot_log_open() {
    if (switch_boot_log != nullptr) return;
    // Try the package path first, then two broad fallbacks. The latter make
    // the trace useful even when the user renamed the application folder.
    switch_boot_log = std::fopen("sdmc:/switch/RaceWave46/boot.log", "ab");
    if (switch_boot_log == nullptr) switch_boot_log = std::fopen("sdmc:/switch/boot.log", "ab");
    if (switch_boot_log == nullptr) switch_boot_log = std::fopen("boot.log", "ab");
    if (switch_boot_log != nullptr) {
        std::setvbuf(switch_boot_log, nullptr, _IONBF, 0);
    }
}

void switch_boot_logf(const char* format, ...) {
    // Multiple runtime workers write here. Serialize the entire record, not
    // just individual stdio calls (boot (11).log lost overlapping records).
    std::lock_guard lock(switch_boot_log_mutex);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    if (switch_boot_log != nullptr) {
        std::fprintf(switch_boot_log, "[%llu ms] ",
            static_cast<unsigned long long>(armTicksToNs(armGetSystemTick()) / 1000000));
        va_start(args, format);
        std::vfprintf(switch_boot_log, format, args);
        va_end(args);
        std::fflush(switch_boot_log);
    }
}

extern "C" void wr64_switch_trace(const char* message) {
    switch_boot_logf("%s\n", message);
}

extern "C" bool wr64_switch_applet_main_loop() {
    return appletMainLoop();
}

#define SWITCH_BOOT_LOG(...) switch_boot_logf(__VA_ARGS__)

// This constructor runs before C++ static initialization in most of the
// runtime and helps distinguish "main was never reached" from a later crash.
__attribute__((constructor(101))) static void switch_early_boot_log() {
    // Do not touch stdio/SD here.  This callback can run before the runtime
    // has finished registering fsdev; only record that it was reached.
    switch_early_boot_reached = true;
}
#else
#define SWITCH_BOOT_LOG(...) do { } while (0)
#endif

// EXP-005: IPS-native ROM with the static safe-area clear DL at 0xF60F0
// widened to full frame (scissor (0,0)-(424,240), fillrect (0,0)-(423,239)).
// EXP-012: additionally widens the IPS-missed transition-fade fill at ROM
// 0xF6270 from (8,20)-(310,218) to full frame (0,0)-(423,239), mirroring the
// sibling full-frame DL at 0xF6188.
// EXP-013: two resident inset scissor builds widened to full frame
// (func_80093DBC ROM 0x4E69C.., func_801FC4D4-A ROM 0xCAB9C..) - hook-redundant.
// EXP-014: THE TRANSITION FADE/DIM QUAD. Layer trace identified the fade as an
// orthographic 2-triangle quad at exactly (8,20)-(415,219) (pass role
// orthographic-overlay). func_801E4440 passes that geometry as immediates to
// the shaded-quad helper func_801E7C58. Both call sites widened to
// (0,0)-(423,239): ROM 0xB304C/0xB3060/0xB3064/0xB3068 (site 1, was
// (8,20)-(415,219)) and 0xB3084/0xB3094/0xB3098/0xB309C (site 2, was
// (8,12)-(415,229)).
// EXP-015: func_801E7908 is the actual TRANSITION fade drawer (four calls to
// the shaded-quad helper func_801E7C58 with animated RGBA from globals; the
// IPS widened x1 310->414 in three calls, kept the 8/20 insets, and missed
// the fourth call entirely). All four call sites widened to (0,0)-(423,239):
// ROM 0xB5FCC 0xB5FF0 0xB5FFC 0xB6000 0xB6004 / 0xB6044 0xB604C 0xB6050 /
// 0xB6084 0xB60A8 0xB60B4 0xB60B8 0xB60BC / 0xB60FC 0xB6104 0xB6108.
// EXP-024: content-edge + separator widen on top of the EXP-015 ROM.
// (a) All six static callers of the shaded-quad helper func_801E7C58
//     (func_801E4440 pause/dim sites 0xB304C/68/84/9C; func_801E7908 fade
//     sites 0xB5FF0/0xB6004/44/50/A8/BC/FC/0x6108) widened from the inclusive
//     (0,0)-(423,239) to exclusive-full (0,0)-(424,240): li 423->424,
//     li 239->240, 12 instruction words. The dynamic caller at 0x1BD698 is
//     untouched.
// (b) Four separator fillrects the IPS missed (static DLs 0x165230/38/80/88)
//     widened from 320-space (8,y)-(311,y+1) to the IPS idiom (8,y)-(415,y+1)
//     (lrx 0x4DC->0x67C).
// EXP-028: the shaded-quad helper func_801E7C58 (ROM ~0xB6338) internally
// CLAMPS x to <=423 and lets y pass at 239 - the inclusive idiom inside the
// shared builder itself, which silently undid the EXP-024 caller widen and
// bounds every quad (including the steady menu backdrops via the dynamic
// caller) to 423x239. Three words convert the clamps into promotions:
//   0xB632C sltiu imm 0x1A8->0x1A7 (X >= 423 enters the clamp branch)
//   0xB6338 addiu imm 0x1A7->0x1A8 (clamp value 423 -> 424)
//   0xB634C sltiu imm 0x0F0->0x0EF (Y >= 239 enters the clamp branch; value
//           already 240). Interior quads (<=422/238) are untouched.
// WIPE-FULLFRAME-R1 (2026-07-29): the screen-change wipe left the top ~2.6 and
// bottom ~6.3 native rows uncovered. Measured cause: the wipe's own transform
// (perspective proj @seg7+0x1300, modelview @seg7+0x1340, uniform scale
// 0.04998779, z -224) maps the cover geometry to screen rows 2.621..233.711 -
// an overscan margin present in the original 320-wide game, not a widescreen
// regression. The cover is 8 quad strips, x 0..3200, y 0..-2560, with a
// textured leading edge at x 3200..3840; four copies of the asset live in the
// segment-1 block (a flat copy of ROM 0xF6090):
//   cover 0x150838 0x151468 0x152098 0x152CC8   edge = cover + 0x120
// The outermost strip edges are extended so the same transform reaches past
// both frame edges (the scissor clips the surplus):
//   y     0 ->    64  (screen row  2.621 -> -3.156)
//   y -2560 -> -2688  (screen row 233.711 -> 245.265)
// Edge-block s/t are extrapolated along each block's own linear fit, so the
// step pattern continues rather than stretching. 56 fields, 80 bytes.
// 2P-FRAME-R1 (2026-07-30): the split-screen play area does not fill the window.
// Each player's viewport is a static Vp struct whose vscale.y is already 120 -
// the full 240 lines - so the letterbox was always the scissor, never the
// viewport. With the two-player scissors opened to a clean 120 lines each
// (corrected in RT64, see lib/rt64/src/common/rt64_wr64_2p_frame.h), the
// viewport centres move so each player's camera axis sits at the centre of its
// own half and both see +/-0.5 of the vertical field of view. Four big-endian
// s16 halfwords at struct offset +10, in pixels * 4; each struct is unique in
// the image, and the one-player structs at 0x0950C0 / 0x10A4E0 are different
// structs in the same tables and are NOT touched:
//   0x0950DA  P1 main       264 -> 240   ( 66 -> 60)
//   0x0950EA  P2 main       704 -> 720   (176 -> 180)
//   0x10A53A  P1 secondary  128 -> 104   ( 32 -> 26)
//   0x10A54A  P2 secondary  568 -> 584   (142 -> 146)
// The secondaries keep their -34 line relationship to their own main.
constexpr uint64_t kRomXxh3 = 0x50D5B6046F0C8115ULL;
constexpr std::u8string_view kGameId = u8"waverace64.n64.us.rev1";
std::binary_semaphore first_vi_ready{0};
std::once_flag first_vi_once;

void on_vi() {
#if WR64_DIAGNOSTIC_ENABLED
    static std::atomic<uint64_t> forensic_native_frame{0};
    const uint64_t native_frame = forensic_native_frame.fetch_add(1) + 1;
    wr64_forensic_set_native_frame(native_frame);
    wr64_forensic_record(WR64_FORENSIC_VI, native_frame, 0, 0, 0, 0, 0, 0, 0);
#endif
    std::call_once(first_vi_once, [] {
        SWITCH_BOOT_LOG("[vi-thread] first VI callback\n");
        first_vi_ready.release();
    });
#if WR64_DIAGNOSTIC_ENABLED
    wr64::composer::wait_if_frozen();
#endif
}

void show_error(const char* message) {
#ifdef WR64_SWITCH
    SWITCH_BOOT_LOG("[runtime] ERROR: %s\n", message);
#else
    std::fprintf(stderr, "[runtime] %s\n", message);
#endif
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    if (task->t.type == M_AUDTASK) {
        return aspMain;
    }
    std::fprintf(stderr, "[runtime] unsupported RSP task type: %" PRIu32 "\n", task->t.type);
    return nullptr;
}

const char* validation_error_name(recomp::RomValidationError error) {
    // Do not use a pointer-valued switch table here.  GCC places that table
    // in .rodata, which creates RELR writes into the Switch R-- segment and
    // crashes libnx's _dynProcessRelr before main().
    if (error == recomp::RomValidationError::Good) return "good";
    if (error == recomp::RomValidationError::FailedToOpen) return "failed to open";
    if (error == recomp::RomValidationError::NotARom) return "not an N64 ROM";
    if (error == recomp::RomValidationError::IncorrectRom) return "incorrect game";
    if (error == recomp::RomValidationError::NotYet) return "unsupported game";
    if (error == recomp::RomValidationError::IncorrectVersion) return "incorrect revision";
    if (error == recomp::RomValidationError::OtherError) return "runtime error";
    return "unknown error";
}

}  // namespace

extern "C" void wr64_switch_stage_log(const char *message) {
#ifdef WR64_SWITCH
    switch_boot_logf("[stage] %s\n", message != nullptr ? message : "(null)");
#else
    (void)message;
#endif
}

int main(int argc, char** argv) {
#ifdef WR64_SWITCH
    switch_boot_log_open();
    SWITCH_BOOT_LOG("[boot] entered main argc=%d early=%d\n", argc,
        switch_early_boot_reached ? 1 : 0);
    SWITCH_BOOT_LOG("[boot] build=boot33-rom-before-menu compiled=%s %s\n", __DATE__, __TIME__);
    // Optional loader probe. Creating an empty BOOT_PROBE file beside the
    // NRO makes the program log one line and exit before SDL/RT64. This is a
    // deterministic way to verify that the NRO itself reaches main().
    if (FILE* probe = std::fopen("sdmc:/switch/RaceWave46/BOOT_PROBE", "rb")) {
        std::fclose(probe);
        SWITCH_BOOT_LOG("[boot] BOOT_PROBE reached main successfully\n");
        return EXIT_SUCCESS;
    }
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::atexit([] {
#ifdef WR64_SWITCH
        SWITCH_BOOT_LOG("[runtime] std::exit handler reached\n");
#else
        WR64_DIAGNOSTIC_LOG(std::fprintf(stderr, "[runtime] std::exit handler reached\n"));
#endif
    });
#if defined(__SWITCH__)
    ::at_quick_exit([] { WR64_DIAGNOSTIC_LOG(std::fprintf(stderr, "[runtime] quick_exit handler reached\n")); });
#else
    std::at_quick_exit([] { WR64_DIAGNOSTIC_LOG(std::fprintf(stderr, "[runtime] std::quick_exit handler reached\n")); });
#endif
    std::set_terminate([] {
        SWITCH_BOOT_LOG("[runtime] std::terminate handler reached\n");
        std::abort();
    });
#ifdef _WIN32
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* exception) -> LONG {
        const auto image_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto exception_address = reinterpret_cast<uintptr_t>(
            exception->ExceptionRecord->ExceptionAddress);
        std::fprintf(
            stderr,
            "[runtime] unhandled Windows exception 0x%08lX at %p (image RVA 0x%llX)\n",
            exception->ExceptionRecord->ExceptionCode,
            exception->ExceptionRecord->ExceptionAddress,
            static_cast<unsigned long long>(exception_address - image_base));
        return EXCEPTION_CONTINUE_SEARCH;
    });
#endif
    wr64::editions::Options options;
    try { options = wr64::editions::parse_process_options(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "[launch] %s\n", error.what());
        SWITCH_BOOT_LOG("[boot] option parsing failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
    std::filesystem::path rom_path = options.rom.empty()
        ? options.runtime / (std::u8string{kGameId} + u8".z64") : options.rom;
#ifdef WR64_ENABLE_RT64
    const int run_seconds = options.seconds;
#else
#ifdef WR64_SWITCH
    // A Switch NRO is an interactive application even while this milestone
    // uses the headless renderer. The old desktop smoke-test default stopped
    // after ten seconds; some forwarder/title-takeover launch paths report
    // that intentional process exit as Atmosphere 2168-0002. Keep running
    // until Horizon requests application shutdown. --seconds remains useful
    // when an explicit timed diagnostic is wanted.
    const int run_seconds = options.seconds;
#else
    const int run_seconds = options.seconds > 0 ? options.seconds : 10;
#endif
#endif
#ifdef WR64_ENABLE_RT64
    const bool windowed = true;
#else
    const bool windowed = options.windowed;
#endif
    const std::filesystem::path runtime_path = options.runtime;

    SWITCH_BOOT_LOG("[boot] runtime=%s rom=%s seconds=%d windowed=%d\n",
        runtime_path.string().c_str(), rom_path.string().c_str(), run_seconds, int(windowed));

    SWITCH_BOOT_LOG("[boot] initializing forensic recorder\n");
    wr64_forensic_initialize();
    SWITCH_BOOT_LOG("[boot] forensic recorder initialized enabled=%d\n", wr64_forensic_enabled());
    wr64::window::set_enabled(windowed);
    SWITCH_BOOT_LOG("[boot] window mode configured\n");

    // Use the error-code overload on Switch. An exception escaping before the
    // runtime has installed its handlers is reported by Horizon only as the
    // generic "software closed due to an error" dialog.
    std::error_code runtime_error;
    SWITCH_BOOT_LOG("[boot] creating runtime directory\n");
    std::filesystem::create_directories(runtime_path, runtime_error);
    if (runtime_error) {
        SWITCH_BOOT_LOG("[boot] create runtime directory failed: %s\n", runtime_error.message().c_str());
    }
    else {
        SWITCH_BOOT_LOG("[boot] runtime directory ready\n");
    }

#ifdef WR64_SWITCH
    // The first Switch milestone did not ship a launcher. Accept the real
    // cartridge filename directly from the package directory, so users do
    // not have to rename it to the internal game id or create .runtime by
    // hand. Explicit --rom still wins.
    if (options.rom.empty()) {
        std::error_code rom_scan_error;
        const std::filesystem::path package_dir = std::filesystem::u8path("sdmc:/switch/RaceWave46");
        // The upstream recompilation targets the native big-endian .z64
        // image. Prefer it deterministically when users also keep converted
        // .n64/.v64 copies in the same folder.
        bool rom_found = false;
        for (const char* wanted_extension : {".z64", ".n64", ".v64"}) {
            for (const auto& entry : std::filesystem::directory_iterator(package_dir, rom_scan_error)) {
                if (rom_scan_error || !entry.is_regular_file()) continue;
                if (entry.path().extension().string() == wanted_extension) {
                    rom_path = entry.path();
                    rom_found = true;
                    break;
                }
            }
            if (rom_scan_error || rom_found) break;
        }
        if (rom_scan_error) {
            SWITCH_BOOT_LOG("[boot] ROM directory scan failed: %s\n", rom_scan_error.message().c_str());
        }
        else {
            SWITCH_BOOT_LOG("[boot] auto-selected ROM %s\n", rom_path.string().c_str());
        }
    }
#endif
    recomp::register_config_path(runtime_path);
    struct AchievementsShutdown {
        ~AchievementsShutdown() {
            wr64::achievements::shutdown();
            wr64::achievement_badges::shutdown();
        }
    } achievements_shutdown;
    wr64::achievements::initialize(recomp::get_config_path());
    wr64::achievement_badges::initialize(recomp::get_config_path());

    recomp::GameEntry game{
        .rom_hash = kRomXxh3,
        .internal_name = "WAVE RACE 64",
        // REQUIRED. recomp::register_game hard-exits on an empty
        // display_name - librecomp/src/recomp.cpp:77. This is the
        // human-facing game name the frontend will show later; the
        // project's own identity belongs in the window title and the UI,
        // not here.
        .display_name = "Wave Race 64",
        .game_id = std::u8string{kGameId},
        .mod_game_id = "waverace64",
        .save_type = recomp::SaveType::Eep4k,
        .is_enabled = true,
        .has_compressed_code = false,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
        .on_init_callback = [](uint8_t*, recomp_context*) {
            wr64::achievements::native::game_initialized(recomp::get_rom());
        },
    };

    recomp::register_game(game);
    // A player owns the cartridge image, not the widescreen ROM this port was
    // recompiled from. Both are accepted: the cartridge image is patched in
    // memory here, the already-patched ROM passes the hash check untouched.
    recomp::register_rom_patcher(wr64::rom_patch::apply_if_unpatched);
    // recompui's launcher reads this, and indexes element zero without
    // checking. A no-op when the frontend is not compiled in.
    wr64::frontend::register_game(game);
    wr64::register_overlays();

    std::u8string game_id{kGameId};

    // TWO WAYS TO RUN, and they want opposite things.
    //
    //   Diagnostic: a run script hands over a ROM and a duration, the game
    //   boots itself, captures, and quits. Every trace and census script in
    //   this tree works this way and none of them can sit at a menu.
    //
    //   Interactive: the launcher is the front door, exactly as it is in every
    //   other recomp project. It picks the ROM, validates it once through
    //   recomp::select_rom, remembers it, and starts the game when told to.
    //   Nothing here should validate a ROM or start anything.
#ifdef WR64_ENABLE_FRONTEND
    const bool diagnostic_run = options.launch;
#elif defined(WR64_SWITCH)
    // Timed diagnostics must remain unattended. Interactive launches use the
    // pre-Vulkan libnx menu by default; WR64_SWITCH_MENU=0 retains the direct
    // boot path for automated comparisons.
    const char *switch_menu_env = std::getenv("WR64_SWITCH_MENU");
    const bool switch_menu_requested = switch_menu_env == nullptr ||
        switch_menu_env[0] == '\0' || switch_menu_env[0] != '0';
    const bool diagnostic_run = options.seconds > 0 || !switch_menu_requested;
#else
    const bool diagnostic_run = true;
#endif

    // The desktop launcher selects and stores its ROM itself. The lightweight
    // Switch launcher only edits graphics settings, so the host must still
    // validate/register the package ROM before recomp::start_game() asks for
    // the stored image.
#ifdef WR64_SWITCH
    const bool select_rom_before_start = true;
#else
    const bool select_rom_before_start = diagnostic_run;
#endif
    if (select_rom_before_start) {
        std::error_code rom_error;
        const auto rom_size = std::filesystem::file_size(rom_path, rom_error);
        if (rom_error || rom_size < 64 || rom_size > 64 * 1024 * 1024) {
            std::fprintf(stderr, "ROM validation failed: select a complete USA Rev 1 ROM in the launcher\n");
            SWITCH_BOOT_LOG("[boot] ROM validation failed path=%s error=%s size=%llu\n",
                rom_path.string().c_str(), rom_error ? rom_error.message().c_str() : "none",
                static_cast<unsigned long long>(rom_error ? 0 : rom_size));
            return EXIT_FAILURE;
        }
        SWITCH_BOOT_LOG("[boot] ROM size=%llu; validating\n", static_cast<unsigned long long>(rom_size));
        const auto validation = recomp::select_rom(rom_path, game_id);
        if (validation != recomp::RomValidationError::Good) {
            std::fprintf(stderr, "ROM validation failed: %s\n", validation_error_name(validation));
            SWITCH_BOOT_LOG("[boot] ROM validation result=%s\n", validation_error_name(validation));
            return EXIT_FAILURE;
        }
        SWITCH_BOOT_LOG("[boot] ROM validation passed\n");
    }

#ifdef WR64_SWITCH
    if (!diagnostic_run) {
        // Validate/register the ROM before libnx takes temporary ownership of
        // the display. On Horizon, doing filesystem/hash work immediately
        // after consoleExit can overlap the console framebuffer teardown.
        // Once this menu closes, Vulkan is therefore the next display owner.
        SWITCH_BOOT_LOG("[launcher] opening pre-Vulkan menu\n");
        if (!wr64::switch_menu::run_preboot(recomp::get_config_path())) {
            SWITCH_BOOT_LOG("[launcher] exit selected\n");
            return EXIT_SUCCESS;
        }
        SWITCH_BOOT_LOG("[launcher] start selected\n");
    }
#endif

    recomp::rsp::callbacks_t rsp_callbacks{.get_rsp_microcode = get_rsp_microcode};
    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = wr64::renderer::create_render_context,
    };
    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = wr64::audio::queue_samples,
        .get_frames_remaining = wr64::audio::get_frames_remaining,
        .set_frequency = wr64::audio::set_frequency,
    };
    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = wr64::input::poll,
        .get_input = wr64::input::get,
        .set_rumble = wr64::input::set_rumble,
        .get_connected_device_info = wr64::input::get_connected_device_info,
    };
    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = wr64::window::create_gfx,
        .create_window = wr64::window::create_window,
        .update_gfx = wr64::window::update_gfx,
    };
    ultramodern::error_handling::callbacks_t error_callbacks{
        .message_box = show_error,
    };

    recomp::Configuration config{
        // recomp::Version has user-declared constructors in this runtime, so
        // it is not an aggregate and cannot take designated initialisers.
        .project_version = recomp::Version{1, 0, 0},
        .window_handle = {},
        .rsp_callbacks = rsp_callbacks,
        .renderer_callbacks = renderer_callbacks,
        .audio_callbacks = audio_callbacks,
        .input_callbacks = input_callbacks,
        .gfx_callbacks = gfx_callbacks,
        .events_callbacks = {
            .vi_callback = on_vi,
            .gfx_init_callback = nullptr,
        },
        .error_handling_callbacks = error_callbacks,
        .threads_callbacks = {},
        .message_queue_control = {},
    };

    if (run_seconds > 0) {
        WR64_DIAGNOSTIC_LOG(std::fprintf(
            stderr,
            "Starting %s Wave Race runtime for %d seconds\n",
            windowed ? "windowed" : "headless",
            run_seconds));
    }
    else {
        WR64_DIAGNOSTIC_LOG(std::fprintf(stderr, "Starting Wave Race; the launcher opens first\n"));
    }
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_wake;
    bool runtime_stopped = false;
    std::thread lifecycle{[game_id, run_seconds, diagnostic_run, &lifecycle_mutex, &lifecycle_wake, &runtime_stopped] {
        // Wait for one dummy VI before starting the game. This initializes the
        // graphics queue while game_status is still None; starting the game
        // earlier lets the VI thread consume null N64 message-queue pointers.
        SWITCH_BOOT_LOG("[runtime] waiting for first VI\n");
        while (!first_vi_ready.try_acquire_for(std::chrono::milliseconds{100})) {
            if (exited.load()) return;
            std::lock_guard lock(lifecycle_mutex);
            if (runtime_stopped) return;
        }
        SWITCH_BOOT_LOG("[runtime] first VI ready\n");

#ifdef WR64_SWITCH
        if (!diagnostic_run) {
            // The menu ran before Vulkan initialization. Apply and persist its
            // selection now that RT64 has loaded the native configuration,
            // while no game workload is running yet.
            wr64::switch_menu::apply_selected();
            SWITCH_BOOT_LOG("[launcher] settings applied\n");
        }
#endif
        {
            std::lock_guard lock(lifecycle_mutex);
            if (runtime_stopped) return;
        }
        SWITCH_BOOT_LOG("[runtime] starting game thread\n");
        WR64_DIAGNOSTIC_LOG(std::fprintf(stderr, "[runtime] starting game thread\n"));
        // The second argument is the game-mode id. Empty is the base game
        // with no game mode, which is what this project has always run.
        recomp::start_game(game_id, "");
        SWITCH_BOOT_LOG("[runtime] game thread started\n");
        WR64_DIAGNOSTIC_LOG(std::fprintf(stderr, "[runtime] game thread started\n"));
        if (run_seconds > 0) {
            std::unique_lock lock(lifecycle_mutex);
            if (lifecycle_wake.wait_for(lock, std::chrono::seconds{run_seconds}, [&] { return runtime_stopped; })) return;
            lock.unlock();
            WR64_DIAGNOSTIC_LOG(std::fprintf(stderr, "[runtime] diagnostic interval complete\n"));
            ultramodern::quit();
        }
    }};

    // Input FIRST: it registers the N64 game inputs that the Controls tab is
    // built from, and configure() is what creates that tab.
    wr64::input::initialize();

    // Program name/id, the primary font and the config tabs. This has to run
    // before recomp::start: start reaches RT64 setup, setup fires recompui's
    // init hook, and that hook throws if any of it is missing.
    wr64::frontend::configure();

    SWITCH_BOOT_LOG("[boot] entering recomp::start\n");
    try {
        recomp::start(config);
    }
    catch (const std::exception& error) {
        SWITCH_BOOT_LOG("[boot] recomp::start exception: %s\n", error.what());
        ultramodern::quit();
    }
    catch (...) {
        SWITCH_BOOT_LOG("[boot] recomp::start unknown exception\n");
        ultramodern::quit();
    }
    SWITCH_BOOT_LOG("[boot] recomp::start returned\n");
    // start() returns after the native/event workers have joined.
    wr64::achievements::game_stopped();
    wr64::achievements::shutdown();
    wr64::achievement_badges::shutdown();
    WR64_DIAGNOSTIC_LOG(std::fprintf(stderr, "[runtime] host event loop stopped\n"));
    {
        std::lock_guard lock(lifecycle_mutex);
        runtime_stopped = true;
    }
    lifecycle_wake.notify_all();

    // The window's event loop used to do this, because it was the only place
    // that saw the close. recompinput owns those events now, so it happens here.
    // quit() already releases diagnostic waits before the runtime joins its
    // workers. Repeat it here before the remaining application cleanup.
    wr64_forensic_force_resume();
    wr64::composer::shutdown();
    wr64_controller_smoke_shutdown();
    wr64::ghost::reset_session();
    wr64_ghost_native_forget();
    wr64_game_rumble_reset();
    wr64_game_rumble_update();
    wr64::input::shutdown();

    lifecycle.join();
    wr64_forensic_shutdown();
    return EXIT_SUCCESS;
}
