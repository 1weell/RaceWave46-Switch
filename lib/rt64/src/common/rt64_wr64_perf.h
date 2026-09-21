//
// Wave Race 64 Switch performance telemetry.
//
#pragma once

#include <cstdint>

struct Wr64SwitchPerfSnapshot {
    float presentedFps = 0.0f;
    float viFps = 0.0f;
    float frameMs = 0.0f;
    float presentMs = 0.0f;
    uint32_t presented = 0;
    uint32_t viUpdates = 0;
    uint32_t presentAttempts = 0;
    uint32_t presentFailures = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t nativeRate = 0;
    uint32_t targetRate = 0;
    uint32_t swapChainRate = 0;
};

extern "C" void wr64_switch_perf_vi_update();
extern "C" void wr64_switch_perf_present(double frameMs, double presentMs,
    uint32_t width, uint32_t height, uint32_t nativeRate,
    uint32_t targetRate, uint32_t swapChainRate, bool success);
extern "C" void wr64_switch_perf_display_list(double cpuMs);
extern "C" void wr64_switch_perf_screen_update(double cpuMs);
extern "C" void wr64_switch_perf_matching(double cpuMs);
extern "C" void wr64_switch_perf_workload(double cpuMs);
extern "C" void wr64_switch_perf_renderer(double cpuMs, double gpuMs);
extern "C" void wr64_switch_perf_get_snapshot(Wr64SwitchPerfSnapshot *snapshot);
