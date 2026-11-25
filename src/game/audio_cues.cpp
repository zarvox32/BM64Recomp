#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "bm64_audio_cues.h"

namespace {
    constexpr int sample_rate = 48000;
    constexpr int channels = 2;
    // Ramp each tone in and out so it doesn't click.
    constexpr int fade_ms = 5;

    SDL_AudioDeviceID device = 0;

    void append_silence(std::vector<float>& samples, int duration_ms) {
        size_t frames = static_cast<size_t>(sample_rate) * duration_ms / 1000;
        samples.insert(samples.end(), frames * channels, 0.0f);
    }

    void append_tone(std::vector<float>& samples, const bm64::audio_cues::Tone& tone, float volume) {
        constexpr float two_pi = 6.28318530718f;
        int frames = sample_rate * tone.duration_ms / 1000;
        int fade_frames = std::min(sample_rate * fade_ms / 1000, frames / 2);
        for (int i = 0; i < frames; i++) {
            float envelope = 1.0f;
            if (i < fade_frames) {
                envelope = static_cast<float>(i) / fade_frames;
            }
            else if (i >= frames - fade_frames) {
                envelope = static_cast<float>(frames - i) / fade_frames;
            }
            float value = volume * envelope * std::sin(two_pi * tone.frequency_hz * i / sample_rate);
            for (int c = 0; c < channels; c++) {
                samples.push_back(value);
            }
        }
        append_silence(samples, tone.gap_after_ms);
    }
}

void bm64::audio_cues::init() {
    if (device != 0) {
        return;
    }
    SDL_AudioSpec desired{};
    desired.freq = sample_rate;
    desired.format = AUDIO_F32SYS;
    desired.channels = channels;
    desired.samples = 512;
    desired.callback = nullptr; // Samples are queued with SDL_QueueAudio.
    // No allowed changes: SDL converts to the device's format if needed.
    device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
    if (device == 0) {
        fprintf(stderr, "[AudioCues] Couldn't open an audio device: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(device, 0);
}

void bm64::audio_cues::shutdown() {
    if (device != 0) {
        SDL_CloseAudioDevice(device);
        device = 0;
    }
}

void bm64::audio_cues::play(std::initializer_list<Tone> tones, float volume) {
    if (device == 0) {
        return;
    }
    std::vector<float> samples;
    for (const Tone& tone : tones) {
        append_tone(samples, tone, volume);
    }
    SDL_ClearQueuedAudio(device);
    SDL_QueueAudio(device, samples.data(), static_cast<Uint32>(samples.size() * sizeof(float)));
}
