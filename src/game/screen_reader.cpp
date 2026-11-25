#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "bm64_screen_reader.h"
#include "librecomp/helpers.hpp"
#include "../../patches/screen_reader.h"

#ifdef BM64_HAS_PRISM
#include <prism.h>
#endif

namespace {
    using bm64::screen_reader::Mode;

    struct Request {
        std::string text;
        bool interrupt;
    };

    // Everything below the mutex is guarded by it.
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<Request> queue;
    bool stop_pending = false;
    bool quit = false;
    bool started = false;
    std::thread worker;

    std::atomic<Mode> mode{ Mode::Auto };
    // Set from BM64_SPEECH_LOG: print everything spoken to stderr, so speech
    // can be checked without hearing it and quoted in bug reports.
    std::atomic<bool> log_speech{ false };
    std::atomic<bool> backend_ready{ false };
    std::atomic<bool> backend_is_screen_reader{ false };

    std::mutex name_mutex;
    std::string selected_backend_name;

    Mode mode_from_env() {
        const char* value = std::getenv("BM64_SPEECH");
        if (value == nullptr) {
            return Mode::Auto;
        }
        std::string lowered;
        for (const char* c = value; *c != '\0'; c++) {
            lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        }
        if (lowered == "off" || lowered == "0") {
            return Mode::Off;
        }
        if (lowered == "on" || lowered == "1") {
            return Mode::On;
        }
        return Mode::Auto;
    }

    bool output_allowed() {
        switch (mode.load()) {
            case Mode::Off:
                return false;
            case Mode::On:
                return true;
            case Mode::Auto:
            default:
                return backend_is_screen_reader.load();
        }
    }

    bool is_blank(const std::string& text) {
        for (char c : text) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        return true;
    }

#ifdef BM64_HAS_PRISM
    void worker_main() {
        PrismConfig config = prism_config_init();
        PrismContext* context = prism_init(&config);
        PrismBackend* backend = nullptr;
        uint64_t features = 0;

        if (context == nullptr) {
            fprintf(stderr, "[Speech] prism_init failed; speech is unavailable\n");
        }
        else {
            // Picks the running screen reader if there is one, otherwise a speech engine.
            backend = prism_registry_create_best(context);
            if (backend == nullptr) {
                fprintf(stderr, "[Speech] No speech backend is available\n");
            }
            else {
                PrismError err = prism_backend_initialize(backend);
                if (err != PRISM_OK && err != PRISM_ERROR_ALREADY_INITIALIZED) {
                    fprintf(stderr, "[Speech] Backend '%s' failed to initialize: %s\n",
                        prism_backend_name(backend), prism_error_string(err));
                    prism_backend_free(backend);
                    backend = nullptr;
                }
                else {
                    features = prism_backend_get_features(backend);
                    const char* name = prism_backend_name(backend);
                    {
                        std::lock_guard name_lock{ name_mutex };
                        selected_backend_name = name != nullptr ? name : "";
                    }
                    backend_is_screen_reader = (features & PRISM_BACKEND_IS_SCREEN_READER) != 0;
                    backend_ready = true;
                    fprintf(stderr, "[Speech] Using backend '%s' (screen reader: %s)\n",
                        name != nullptr ? name : "?", backend_is_screen_reader ? "yes" : "no");
                }
            }
        }

        // Output sends to speech and braille when the backend supports both.
        const bool can_output = (features & PRISM_BACKEND_SUPPORTS_OUTPUT) != 0;
        const bool can_speak = (features & PRISM_BACKEND_SUPPORTS_SPEAK) != 0;
        const bool can_stop = (features & PRISM_BACKEND_SUPPORTS_STOP) != 0;

        std::unique_lock lock{ queue_mutex };
        while (true) {
            queue_cv.wait(lock, [] { return quit || stop_pending || !queue.empty(); });
            if (quit) {
                break;
            }
            const bool do_stop = std::exchange(stop_pending, false);
            std::deque<Request> batch;
            batch.swap(queue);
            lock.unlock();

            if (backend != nullptr) {
                if (do_stop && can_stop) {
                    PrismError err = prism_backend_stop(backend);
                    if (err != PRISM_OK && err != PRISM_ERROR_NOT_SPEAKING) {
                        fprintf(stderr, "[Speech] Stop failed: %s\n", prism_error_string(err));
                    }
                }
                if (output_allowed()) {
                    for (const Request& request : batch) {
                        PrismError err = PRISM_ERROR_NOT_IMPLEMENTED;
                        if (can_output) {
                            err = prism_backend_output(backend, request.text.c_str(), request.interrupt);
                        }
                        else if (can_speak) {
                            err = prism_backend_speak(backend, request.text.c_str(), request.interrupt);
                        }
                        if (err != PRISM_OK) {
                            fprintf(stderr, "[Speech] Output failed: %s\n", prism_error_string(err));
                        }
                    }
                }
            }

            lock.lock();
        }
        lock.unlock();

        backend_ready = false;
        if (backend != nullptr) {
            prism_backend_free(backend);
        }
        if (context != nullptr) {
            prism_shutdown(context);
        }
    }
#endif
}

void bm64::screen_reader::init() {
#ifdef BM64_HAS_PRISM
    std::lock_guard lock{ queue_mutex };
    if (started) {
        return;
    }
    mode = mode_from_env();
    const char* log_env = std::getenv("BM64_SPEECH_LOG");
    log_speech = log_env != nullptr && log_env[0] != '\0' && log_env[0] != '0';
    quit = false;
    stop_pending = false;
    queue.clear();
    started = true;
    worker = std::thread{ worker_main };
#endif
}

void bm64::screen_reader::shutdown() {
#ifdef BM64_HAS_PRISM
    {
        std::lock_guard lock{ queue_mutex };
        if (!started) {
            return;
        }
        started = false;
        quit = true;
        queue.clear();
    }
    queue_cv.notify_one();
    if (worker.joinable()) {
        worker.join();
    }
#endif
}

void bm64::screen_reader::speak(std::string text, bool interrupt) {
    if (mode.load() == Mode::Off || is_blank(text)) {
        return;
    }
    if (log_speech.load()) {
        fprintf(stderr, "[Speech] %s%s\n", interrupt ? "" : "(queued) ", text.c_str());
    }
    {
        std::lock_guard lock{ queue_mutex };
        if (!started) {
            return;
        }
        // Queued text that hasn't been spoken yet is stale once something interrupts it.
        if (interrupt) {
            queue.clear();
        }
        queue.push_back(Request{ std::move(text), interrupt });
    }
    queue_cv.notify_one();
}

void bm64::screen_reader::stop() {
    {
        std::lock_guard lock{ queue_mutex };
        if (!started) {
            return;
        }
        queue.clear();
        stop_pending = true;
    }
    queue_cv.notify_one();
}

void bm64::screen_reader::set_mode(Mode new_mode) {
    mode = new_mode;
    if (new_mode == Mode::Off) {
        stop();
    }
}

bm64::screen_reader::Mode bm64::screen_reader::get_mode() {
    return mode.load();
}

bool bm64::screen_reader::is_active() {
    return backend_ready.load() && output_allowed();
}

std::string bm64::screen_reader::backend_name() {
    std::lock_guard lock{ name_mutex };
    return selected_backend_name;
}

// Bridges callable from game patches and mods. See patches/screen_reader.h.

extern "C" void recomp_speak(uint8_t* rdram, recomp_context* ctx) {
    std::string text = _arg_string<0>(rdram, ctx);
    s32 interrupt = _arg<1, s32>(rdram, ctx);
    bm64::screen_reader::speak(std::move(text), interrupt != 0);
}

extern "C" void recomp_speech_stop(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    bm64::screen_reader::stop();
}

extern "C" void recomp_speech_is_active(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<s32>(ctx, bm64::screen_reader::is_active() ? 1 : 0);
}
