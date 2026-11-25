#ifndef __BM64_AUDIO_CUES_H__
#define __BM64_AUDIO_CUES_H__

#include <initializer_list>

// Short synthesized sounds (beeps) for accessibility feedback. They play on
// their own audio device, mixed by the system with the game's audio.
// All functions are safe to call from any thread.
namespace bm64::audio_cues {
    struct Tone {
        float frequency_hz;
        int duration_ms;
        // Silence after this tone, before the next one in a sequence.
        int gap_after_ms = 0;
    };

    // Opens the audio device for cues. Call after SDL audio is initialized.
    void init();
    void shutdown();

    // Plays a sequence of tones, cutting off any cue still playing.
    // volume is 0.0 to 1.0.
    void play(std::initializer_list<Tone> tones, float volume = 0.25f);
}

#endif
