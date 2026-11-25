#ifndef __BM64_SPEECH_H__
#define __BM64_SPEECH_H__

#include <string>

// Accessibility speech output (screen reader or text-to-speech) via Prism.
//
// All functions are safe to call from any thread. Requests are queued and
// handed to Prism on a dedicated worker thread, so callers never block on
// the speech backend. If speech is unavailable or disabled, calls are no-ops.
namespace bm64::speech {
    enum class Mode {
        Off,  // Never speak.
        Auto, // Speak only when a screen reader (NVDA, JAWS, VoiceOver, Orca...) is running.
        On,   // Always speak, falling back to the system voice without a screen reader.
    };

    // Starts the speech worker and selects the best available backend.
    // The initial mode comes from the BM64_SPEECH environment variable
    // ("off", "auto" or "on"), defaulting to Auto.
    void init();
    // Stops the worker and releases the backend. Pending speech is dropped.
    void shutdown();

    // Queues text to be spoken. With interrupt set, current speech and any
    // queued-but-unspoken text are discarded first.
    void speak(std::string text, bool interrupt = true);
    // Stops current speech and discards anything queued.
    void stop();

    void set_mode(Mode mode);
    Mode get_mode();

    // True once a backend has been initialized and the current mode allows output.
    bool is_active();
    // Name of the selected backend, e.g. "NVDA" or "SAPI". Empty if none.
    std::string backend_name();
}

#endif
