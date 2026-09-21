#include "wr64_diagnostic_policy.h"
#include "wr64_io.hpp"

#ifdef WR64_ENABLE_RT64

#include <switch.h>

#include "wr64_audio_resampler.h"
#include "wr64_audio_ring_copy.h"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <mutex>
#include <vector>

extern "C" void wr64_switch_trace(const char* message);

namespace wr64::audio {

namespace {

// The Switch audout service accepts one fixed 48 kHz stereo S16 stream. The
// N64 can request a different rate, so the same high-quality resampler used by
// the desktop backend converts the game's blocks before they enter this ring.
constexpr uint32_t kOutputRate = 48000;
constexpr size_t kBufferFrames = 960; // 20 ms at 48 kHz
constexpr size_t kBufferSamples = kBufferFrames * 2;
constexpr size_t kBufferCount = 6; // 120 ms of service-side queueing
constexpr size_t kRingSamples = kOutputRate * 2 * 2; // two seconds, stereo
constexpr uint32_t kCushionMs = 40;

struct OutputBuffer {
    AudioOutBuffer descriptor{};
    int16_t* samples = nullptr;
    bool queued = false;
};

struct AudioState {
    std::mutex mutex;
    bool initialized = false;
    bool started = false;
    bool resampler_live = false;
    bool shutdown_registered = false;
    bool failure_reported = false;
    uint32_t input_rate = kOutputRate;
    std::array<OutputBuffer, kBufferCount> buffers{};
    std::array<int16_t, kRingSamples> ring{};
    uint64_t ring_read = 0;
    uint64_t ring_write = 0;
    uint32_t queued_buffers = 0;
    std::vector<int16_t> swap_buffer;
    wr64::audio_resampler::Resampler resampler;
};

AudioState audio_state;

void shutdown_audio();

size_t ring_fill_samples() {
    return static_cast<size_t>(audio_state.ring_write - audio_state.ring_read);
}

bool ring_write(const int16_t* samples, size_t count) {
    if (!wr64::audio_ring_copy::try_write(audio_state.ring.data(), audio_state.ring.size(),
            audio_state.ring_read, audio_state.ring_write, samples, count)) {
        if (!audio_state.failure_reported) {
            audio_state.failure_reported = true;
            WR64_DIAGNOSTIC_LOG(std::fprintf(stderr,
                "[audio] output ring overflow; dropping incoming samples\n"));
        }
        return false;
    }
    return true;
}

void release_completed_buffers() {
    for (;;) {
        AudioOutBuffer* released = nullptr;
        u32 released_count = 0;
        const Result rc = audoutGetReleasedAudioOutBuffer(&released, &released_count);
        if (R_FAILED(rc) || released_count == 0 || released == nullptr) {
            return;
        }
        for (auto& buffer : audio_state.buffers) {
            if (&buffer.descriptor == released) {
                buffer.queued = false;
                if (audio_state.queued_buffers > 0) audio_state.queued_buffers--;
                break;
            }
        }
    }
}

void pump_output() {
    release_completed_buffers();
    for (auto& buffer : audio_state.buffers) {
        if (buffer.queued || ring_fill_samples() < kBufferSamples) continue;
        wr64::audio_ring_copy::read(audio_state.ring.data(), audio_state.ring.size(),
            audio_state.ring_read, buffer.samples, kBufferSamples);
        buffer.descriptor.next = nullptr;
        buffer.descriptor.data_size = kBufferSamples * sizeof(int16_t);
        buffer.descriptor.data_offset = 0;
        const Result rc = audoutAppendAudioOutBuffer(&buffer.descriptor);
        if (R_FAILED(rc)) {
            if (!audio_state.failure_reported) {
                audio_state.failure_reported = true;
                WR64_DIAGNOSTIC_LOG(std::fprintf(stderr,
                    "[audio] audoutAppendAudioOutBuffer failed: 0x%08" PRIx32 "\n",
                    static_cast<uint32_t>(rc)));
            }
            return;
        }
        buffer.queued = true;
        audio_state.queued_buffers++;
        audio_state.ring_read += kBufferSamples;
    }
}

bool open_audio_locked() {
    if (audio_state.initialized) return true;
    // Allocate a normal output block before streaming. resize() still preserves
    // the existing handling of arbitrary input lengths and odd trailing samples;
    // larger input blocks grow the vector normally and retain that capacity.
    audio_state.swap_buffer.reserve(kBufferSamples);
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        WR64_DIAGNOSTIC_LOG(std::fprintf(stderr,
            "[audio] audoutInitialize failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(rc)));
        return false;
    }

    const uint32_t rate = audoutGetSampleRate();
    const uint32_t channels = audoutGetChannelCount();
    const PcmFormat format = audoutGetPcmFormat();
    if (rate != kOutputRate || channels != 2 || format != PcmFormat_Int16) {
        WR64_DIAGNOSTIC_LOG(std::fprintf(stderr,
            "[audio] unsupported audout format: %" PRIu32 " Hz, %" PRIu32 " channels, format=%d\n",
            rate, channels, int(format)));
        audoutExit();
        return false;
    }

    const size_t data_size = kBufferSamples * sizeof(int16_t);
    const size_t buffer_size = (data_size + 0xFFFu) & ~size_t(0xFFFu);
    for (auto& buffer : audio_state.buffers) {
        buffer.samples = static_cast<int16_t*>(memalign(0x1000, buffer_size));
        if (buffer.samples == nullptr) {
            WR64_DIAGNOSTIC_LOG(std::fprintf(stderr, "[audio] audout buffer allocation failed\n"));
            for (auto& allocated : audio_state.buffers) {
                if (allocated.samples != nullptr) std::free(allocated.samples);
                allocated.samples = nullptr;
            }
            audoutExit();
            return false;
        }
        std::memset(buffer.samples, 0, buffer_size);
        buffer.descriptor.buffer = buffer.samples;
        buffer.descriptor.buffer_size = buffer_size;
    }

    audio_state.initialized = true;
    audio_state.failure_reported = false;
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        WR64_DIAGNOSTIC_LOG(std::fprintf(stderr,
            "[audio] audoutStartAudioOut failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(rc)));
        for (auto& allocated : audio_state.buffers) {
            if (allocated.samples != nullptr) std::free(allocated.samples);
            allocated.samples = nullptr;
        }
        audio_state.initialized = false;
        audoutExit();
        return false;
    }
    audio_state.started = true;
    if (!audio_state.shutdown_registered) {
        std::atexit(shutdown_audio);
        audio_state.shutdown_registered = true;
    }
    WR64_DIAGNOSTIC_LOG(std::fprintf(stderr,
        "[audio] Switch audout ready: %" PRIu32 " Hz, %" PRIu32 " channels, %zu x %zu frames\n",
        rate, channels, kBufferCount, kBufferFrames));
    wr64_switch_trace("[audio] Switch audout ready: 48 kHz stereo");
    return true;
}

void drain_resampler_locked() {
    auto& ready = audio_state.resampler.ready();
    if (!ready.empty()) {
        ring_write(ready.data(), ready.size());
        ready.clear();
    }
}

void shutdown_audio() {
    std::lock_guard lock(audio_state.mutex);
    if (!audio_state.initialized) return;
    if (audio_state.started) audoutStopAudioOut();
    for (auto& buffer : audio_state.buffers) {
        if (buffer.samples != nullptr) std::free(buffer.samples);
        buffer.samples = nullptr;
        buffer.queued = false;
    }
    audio_state.queued_buffers = 0;
    audio_state.initialized = false;
    audio_state.started = false;
    audoutExit();
}

} // namespace

void queue_samples(int16_t* samples, size_t sample_count) {
    if (samples == nullptr || sample_count == 0) return;
    std::lock_guard lock(audio_state.mutex);
    if (!audio_state.initialized || !audio_state.resampler_live) return;

    audio_state.swap_buffer.resize(sample_count);
    // The N64 audio ABI presents stereo pairs in host-swapped order.
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        audio_state.swap_buffer[i] = samples[i + 1];
        audio_state.swap_buffer[i + 1] = samples[i];
    }
    audio_state.resampler.put(audio_state.swap_buffer.data(), sample_count);
    drain_resampler_locked();
    pump_output();
}

size_t get_frames_remaining() {
    std::lock_guard lock(audio_state.mutex);
    if (!audio_state.initialized || !audio_state.resampler_live) return 0;
    pump_output();
    const uint64_t output_frames =
        static_cast<uint64_t>(ring_fill_samples() / 2) +
        static_cast<uint64_t>(audio_state.queued_buffers) * kBufferFrames;
    const uint64_t input_frames = output_frames * audio_state.input_rate / kOutputRate;
    const uint64_t cushion_frames = static_cast<uint64_t>(audio_state.input_rate) * kCushionMs / 1000;
    return input_frames > cushion_frames ? static_cast<size_t>(input_frames - cushion_frames) : 0;
}

void set_frequency(uint32_t frequency) {
    std::lock_guard lock(audio_state.mutex);
    WR64_DIAGNOSTIC_LOG(std::fprintf(stderr, "[audio] game frequency: %" PRIu32 " Hz\n", frequency));
    if (!open_audio_locked()) {
        audio_state.input_rate = frequency;
        return;
    }
    if (audio_state.resampler_live && audio_state.input_rate == frequency) return;
    if (audio_state.resampler_live) {
        audio_state.resampler.flush();
        drain_resampler_locked();
    }
    audio_state.input_rate = frequency;
    audio_state.resampler.configure(frequency, kOutputRate);
    audio_state.resampler_live = true;
    pump_output();
}

} // namespace wr64::audio

namespace wr64::input {
namespace {

PadState pad{};
bool initialized = false;

constexpr uint16_t kA = 0x8000;
constexpr uint16_t kB = 0x4000;
constexpr uint16_t kZ = 0x2000;
constexpr uint16_t kStart = 0x1000;
constexpr uint16_t kUp = 0x0800;
constexpr uint16_t kDown = 0x0400;
constexpr uint16_t kLeft = 0x0200;
constexpr uint16_t kRight = 0x0100;
constexpr uint16_t kL = 0x0020;
constexpr uint16_t kR = 0x0010;
constexpr uint16_t kCUp = 0x0008;
constexpr uint16_t kCRight = 0x0001;

} // namespace

void poll() {
    if (!initialized) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        initialized = true;
    }
    padUpdate(&pad);
}

bool get(int controller, uint16_t* buttons, float* x, float* y) {
    if ((controller != 0) || !initialized) {
        return false;
    }

    const uint64_t buttons_down = padGetButtons(&pad);
    const HidAnalogStickState stick = padGetStickPos(&pad, 0);
    uint16_t result = 0;
    if (buttons_down & HidNpadButton_A) result |= kA;
    if (buttons_down & HidNpadButton_X) result |= kB;
    if (buttons_down & HidNpadButton_ZL) result |= kZ;
    if (buttons_down & HidNpadButton_Plus) result |= kStart;
    if (buttons_down & HidNpadButton_Up) result |= kUp;
    if (buttons_down & HidNpadButton_Down) result |= kDown;
    if (buttons_down & HidNpadButton_Left) result |= kLeft;
    if (buttons_down & HidNpadButton_Right) result |= kRight;
    if (buttons_down & HidNpadButton_L) result |= kL;
    if (buttons_down & HidNpadButton_R) result |= kR;
    if (buttons_down & HidNpadButton_Y) result |= kCUp;
    if (buttons_down & HidNpadButton_B) result |= kCRight;

    *buttons = result;
    *x = std::clamp(static_cast<float>(stick.x) / 32767.0F, -1.0F, 1.0F);
    *y = std::clamp(static_cast<float>(stick.y) / 32767.0F, -1.0F, 1.0F);
    return true;
}

void set_rumble(int, bool) {}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller) {
    if (controller == 0) {
        return {ultramodern::input::Device::Controller, ultramodern::input::Pak::None};
    }
    return {ultramodern::input::Device::None, ultramodern::input::Pak::None};
}

} // namespace wr64::input

#endif
