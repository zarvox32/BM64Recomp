#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "bm64_menu_cursor_search.h"
#include "bm64_audio_cues.h"
#include "bm64_screen_reader.h"
#include "recomp_input.h"
#include "recomp_ui.h"

namespace {
    using Clock = std::chrono::steady_clock;

    // The game's RAM: 8 MB starting at 0x80000000.
    constexpr uint32_t ram_base = 0x80000000;
    constexpr uint32_t ram_size = 8 * 1024 * 1024;
    // Time for the game to act on a press and finish any menu animation before
    // memory is compared. Also the pause before a beep that follows a low tone.
    // The main menu moves its cursor 0.7 to 1.5 seconds after a press, so this is
    // generous; BM64_MENU_SEARCH_SETTLE_MS overrides it.
    std::chrono::milliseconds settle_time{ 2000 };
    // Menu cursors are small indexes.
    constexpr int32_t max_cursor_value = 63;
    // How many candidates each log entry lists.
    constexpr size_t candidates_to_log = 5;
    // Starts and stops mapping a menu.
    constexpr SDL_Scancode toggle_key = SDL_SCANCODE_F9;

    // N64 buttons (libultra masks).
    constexpr uint16_t button_a = 0x8000;
    constexpr uint16_t button_b = 0x4000;
    constexpr uint16_t button_start = 0x1000;
    constexpr uint16_t dpad_up = 0x0800;
    constexpr uint16_t dpad_down = 0x0400;
    constexpr uint16_t dpad_left = 0x0200;
    constexpr uint16_t dpad_right = 0x0100;
    constexpr uint16_t c_up = 0x0008;
    constexpr uint16_t c_down = 0x0004;
    constexpr uint16_t c_left = 0x0002;
    constexpr uint16_t c_right = 0x0001;
    // get_n64_input scales full stick tilt to 0.65.
    constexpr float stick_threshold = 0.4f;

    std::atomic<uint8_t*> rdram{ nullptr };
    std::atomic<bool> quit{ false };
    std::thread worker;

    enum class Change { Moved, Stuck, Wrapped };

    struct Candidate {
        uint32_t offset = 0;   // From ram_base.
        int width = 4;         // 1, 2 or 4 bytes.
        int moves = 0;         // Presses that moved it one step in the pressed direction.
        int stuck = 0;         // Presses that didn't change it.
        int wraps = 0;         // Presses that sent it around to the other end.
        int32_t value = 0;     // After the latest press.
        int32_t min_value = 0;
        int32_t max_value = 0;
        Change last_change = Change::Moved;
        std::string history;   // Its value before the first press and after each one.
    };

    struct Search {
        bool active = false;   // Mapping a menu (started with F9).
        int menu = 0;
        int press = 0;
        // False until the first measured press of a menu collects candidates.
        bool started = false;
        // Once the end of the menu has been found: the cursor value of its last
        // item, or -1 if not known yet.
        int32_t last_item = -1;
        std::vector<Candidate> candidates;
        // Memory before and after the press being measured.
        std::vector<uint8_t> before;
        std::vector<uint8_t> after;
        // Memory once the previous press settled (or when F9 was pressed), which
        // becomes the next press's "before". It can't be taken when a press is
        // noticed: the game may already have moved the cursor by then.
        std::vector<uint8_t> baseline;
    };

    namespace tones {
        // The only meaning of the normal beep: press a direction now.
        void ready() { bm64::audio_cues::play({ { 880.0f, 70 } }); }
        void last_item() { bm64::audio_cues::play({ { 330.0f, 250 } }); }
        void wrap() { bm64::audio_cues::play({ { 880.0f, 70, 70 }, { 880.0f, 70 } }); }
        void too_soon() { bm64::audio_cues::play({ { 196.0f, 150 } }, 0.2f); }
        void lost() { bm64::audio_cues::play({ { 440.0f, 120, 30 }, { 294.0f, 160 } }); }
    }

    void say(const std::string& text) {
        bm64::screen_reader::speak(text, true);
    }

    void log_line(const std::string& line) {
        fprintf(stderr, "[Search] %s\n", line.c_str());
        fflush(stderr);
    }

    std::string hex_address(uint32_t offset) {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%08X", ram_base + offset);
        return buf;
    }

    std::string shot_name(int menu, const char* suffix, int press) {
        char buf[32];
        snprintf(buf, sizeof(buf), "menu%02d_%s%02d", menu, suffix, press);
        return buf;
    }

    // RDRAM holds native-endian 32-bit words, so 8- and 16-bit values are found
    // by flipping the low address bits, as recomp's MEM_B and MEM_H do.
    int32_t read_value(const std::vector<uint8_t>& ram, uint32_t offset, int width) {
        switch (width) {
            case 1:
                return static_cast<int8_t>(ram[offset ^ 3]);
            case 2: {
                int16_t value;
                std::memcpy(&value, &ram[offset ^ 2], sizeof(value));
                return value;
            }
            default: {
                int32_t value;
                std::memcpy(&value, &ram[offset], sizeof(value));
                return value;
            }
        }
    }

    void snapshot(std::vector<uint8_t>& out) {
        // The game keeps running while this copies; a value caught mid-write
        // only costs that one candidate.
        out.resize(ram_size);
        std::memcpy(out.data(), rdram.load(), ram_size);
    }

    bool is_cursor_value(int32_t value) {
        return value >= 0 && value <= max_cursor_value;
    }

    // The first measured press of a menu: every small value that moved one
    // step in the pressed direction is a candidate.
    void collect_candidates(Search& search, int step) {
        search.candidates.clear();
        for (int width : { 1, 2, 4 }) {
            for (uint32_t offset = 0; offset + width <= ram_size; offset += width) {
                int32_t before = read_value(search.before, offset, width);
                if (!is_cursor_value(before)) {
                    continue;
                }
                int32_t after = read_value(search.after, offset, width);
                if (after - before != step || !is_cursor_value(after)) {
                    continue;
                }
                Candidate candidate;
                candidate.offset = offset;
                candidate.width = width;
                candidate.moves = 1;
                candidate.value = after;
                candidate.min_value = std::min(before, after);
                candidate.max_value = std::max(before, after);
                candidate.history = std::to_string(before) + "," + std::to_string(after);
                search.candidates.push_back(std::move(candidate));
            }
        }
        search.started = true;
    }

    // Later presses: keep candidates that moved one step, stayed put (the end of
    // a menu that doesn't wrap), or wrapped around. Values that change by
    // themselves (animations, timers) fail this within a few presses.
    void filter_candidates(Search& search, int step) {
        std::vector<Candidate> kept;
        for (Candidate& candidate : search.candidates) {
            int32_t before = read_value(search.before, candidate.offset, candidate.width);
            int32_t after = read_value(search.after, candidate.offset, candidate.width);
            // Say why a strong candidate was dropped, so a wrong rule shows up.
            auto drop = [&](const char* reason) {
                if (candidate.moves >= 2) {
                    log_line("  dropped " + hex_address(candidate.offset) + " (" + std::to_string(candidate.width) +
                        " bytes, values " + candidate.history + "): " + reason + "; before " +
                        std::to_string(before) + ", after " + std::to_string(after));
                }
            };
            if (!is_cursor_value(after)) {
                drop("out of range");
                continue;
            }
            int32_t delta = after - before;
            if (delta == step) {
                candidate.moves++;
                candidate.last_change = Change::Moved;
            }
            else if (delta == 0) {
                candidate.stuck++;
                candidate.last_change = Change::Stuck;
            }
            else if ((step > 0 && after < before) || (step < 0 && after > before)) {
                candidate.wraps++;
                candidate.last_change = Change::Wrapped;
            }
            else {
                drop("moved by more than one step");
                continue;
            }
            candidate.value = after;
            candidate.min_value = std::min(candidate.min_value, after);
            candidate.max_value = std::max(candidate.max_value, after);
            candidate.history += "," + std::to_string(after);
            kept.push_back(std::move(candidate));
        }
        search.candidates = std::move(kept);
    }

    // Best first: the most moves, then the fewest presses that didn't move it,
    // then the widest value.
    void rank_candidates(Search& search) {
        std::stable_sort(search.candidates.begin(), search.candidates.end(),
            [](const Candidate& a, const Candidate& b) {
                if (a.moves != b.moves) return a.moves > b.moves;
                if (a.stuck + a.wraps != b.stuck + b.wraps) return a.stuck + a.wraps < b.stuck + b.wraps;
                return a.width > b.width;
            });
    }

    // Logs the top candidates. Parts of an already listed value with the same
    // history (e.g. the low byte of a 32-bit cursor) are skipped.
    void log_candidates(const Search& search) {
        std::vector<const Candidate*> listed;
        for (const Candidate& candidate : search.candidates) {
            if (listed.size() >= candidates_to_log) {
                break;
            }
            bool duplicate = std::any_of(listed.begin(), listed.end(), [&](const Candidate* other) {
                return candidate.offset >= other->offset &&
                    candidate.offset + candidate.width <= other->offset + other->width &&
                    candidate.history == other->history;
            });
            if (duplicate) {
                continue;
            }
            listed.push_back(&candidate);
            log_line("  " + std::to_string(listed.size()) + ". " + hex_address(candidate.offset) +
                " (" + std::to_string(candidate.width) + " bytes): values " + candidate.history +
                " (moved " + std::to_string(candidate.moves) + ", stuck " + std::to_string(candidate.stuck) +
                ", wrapped " + std::to_string(candidate.wraps) + ")");
        }
    }

    // Addresses to follow on every press, from BM64_MENU_SEARCH_TRACE: a comma
    // separated list of "address:width", e.g. "801A0978:2,801A097A:1". Shows
    // directly what a suspected cursor does, including why it was dropped.
    struct Trace {
        uint32_t offset;
        int width;
    };
    std::vector<Trace> traces;

    void parse_traces() {
        const char* spec = std::getenv("BM64_MENU_SEARCH_TRACE");
        if (spec == nullptr) {
            return;
        }
        std::string list = spec;
        size_t start = 0;
        while (start < list.size()) {
            size_t end = list.find(',', start);
            std::string item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
            start = end == std::string::npos ? list.size() : end + 1;
            size_t colon = item.find(':');
            uint32_t address = static_cast<uint32_t>(std::strtoul(item.substr(0, colon).c_str(), nullptr, 16));
            int width = colon == std::string::npos ? 2 : std::atoi(item.c_str() + colon + 1);
            if (address >= ram_base && address - ram_base + 4 <= ram_size && (width == 1 || width == 2 || width == 4)) {
                traces.push_back({ address - ram_base, width });
            }
        }
    }

    void log_traces(const Search& search) {
        for (const Trace& trace : traces) {
            std::string line = "  trace " + hex_address(trace.offset) + " (" + std::to_string(trace.width) + " bytes): ";
            line += "before " + std::to_string(read_value(search.before, trace.offset, trace.width)) +
                ", after " + std::to_string(read_value(search.after, trace.offset, trace.width));
            log_line(line);
        }
    }

    std::string items_text(int count) {
        return std::to_string(count) + (count == 1 ? " item" : " items");
    }

    // Measures one press and gives feedback. Returns true if a normal beep
    // should follow after a pause (after a low tone).
    bool measure_press(Search& search, int step, const std::string& input) {
        search.press++;
        if (!search.started) {
            collect_candidates(search, step);
        }
        else {
            filter_candidates(search, step);
        }
        rank_candidates(search);

        log_line("Menu " + std::to_string(search.menu) + ", press " + std::to_string(search.press) +
            " (" + (step > 0 ? "forward" : "back") + ", " + input + "): " +
            std::to_string(search.candidates.size()) + " candidates");
        log_traces(search);
        log_candidates(search);
        log_line("shot " + shot_name(search.menu, "press", search.press));

        if (search.candidates.empty()) {
            log_line("  No candidates left; stopping.");
            search.active = false;
            tones::lost();
            say("Lost track of the cursor. Press F9 to start this menu again.");
            return false;
        }

        const Candidate& best = search.candidates.front();
        switch (best.last_change) {
            case Change::Moved:
                // Once the end is known, arriving on the last item gets the low
                // tone first, then the normal beep.
                if (search.last_item >= 0 && step > 0 && best.value == search.last_item) {
                    tones::last_item();
                    return true;
                }
                tones::ready();
                return false;
            case Change::Stuck: {
                // Pressing past the end didn't move the cursor: the menu doesn't wrap.
                int count = best.max_value - best.min_value + 1;
                search.last_item = best.max_value;
                log_line(std::string("  The cursor didn't move: the ") + (step > 0 ? "last" : "first") +
                    " item, and the menu doesn't wrap. " + items_text(count) + " seen.");
                tones::last_item();
                say(std::string(step > 0 ? "Last item" : "First item") + ", the menu doesn't wrap. You went through " +
                    items_text(count) + ".");
                return true;
            }
            case Change::Wrapped: {
                int count = best.max_value - best.min_value + 1;
                search.last_item = best.max_value;
                log_line("  The cursor wrapped around: the menu wraps, with " + items_text(count) + ".");
                tones::wrap();
                say(std::string("Wrapped to the ") + (step > 0 ? "first" : "last") + " item. The menu wraps, with " +
                    items_text(count) + ".");
                return true;
            }
        }
        return false;
    }

    // With BM64_MENU_SEARCH_DUMP set to a folder, saves the game's RAM as
    // <name>.bin there (raw RDRAM: native-endian 32-bit words), so any pattern
    // can be searched for offline when the built-in rules miss a cursor.
    void dump_snapshot(const std::vector<uint8_t>& ram, const std::string& name) {
        const char* dir = std::getenv("BM64_MENU_SEARCH_DUMP");
        if (dir == nullptr) {
            return;
        }
        std::string path = std::string(dir) + "/" + name + ".bin";
        FILE* file = fopen(path.c_str(), "wb");
        if (file == nullptr) {
            log_line("Couldn't write " + path);
            return;
        }
        fwrite(ram.data(), 1, ram.size(), file);
        fclose(file);
    }

    void start_menu(Search& search) {
        search.active = true;
        search.menu++;
        search.press = 0;
        search.started = false;
        search.last_item = -1;
        search.candidates.clear();
        // The menu is idle now, so this is a clean "before" for the first press.
        snapshot(search.baseline);
        dump_snapshot(search.baseline, shot_name(search.menu, "start", 0));
        log_line("Mapping menu " + std::to_string(search.menu) + ".");
        log_line("shot " + shot_name(search.menu, "start", 0));
        say("Mapping menu " + std::to_string(search.menu) + ". Press Down after each beep.");
        tones::ready();
    }

    void stop_menu(Search& search, const std::string& message) {
        if (search.started) {
            log_line("Menu " + std::to_string(search.menu) + " stopped after " + std::to_string(search.press) +
                " presses, " + std::to_string(search.candidates.size()) + " candidates. Best:");
            log_candidates(search);
        }
        else {
            log_line("Menu " + std::to_string(search.menu) + " stopped before any press was measured.");
        }
        search.active = false;
        say(message);
    }

    struct Inputs {
        bool forward = false;
        bool back = false;
        bool action = false;
        bool toggle = false;
        std::string forward_input;
        std::string back_input;
    };

    Inputs read_inputs() {
        Inputs inputs;
        // While one of the port's own menus has the input, the game isn't reading it.
        if (recompui::is_context_capturing_input()) {
            return inputs;
        }
        uint16_t buttons = 0;
        float x = 0.0f;
        float y = 0.0f;
        recomp::get_n64_input(0, &buttons, &x, &y);

        if (buttons & dpad_down) inputs.forward_input = "D-pad down";
        else if (buttons & dpad_right) inputs.forward_input = "D-pad right";
        else if (buttons & c_down) inputs.forward_input = "C down";
        else if (buttons & c_right) inputs.forward_input = "C right";
        else if (y < -stick_threshold) inputs.forward_input = "stick down";
        else if (x > stick_threshold) inputs.forward_input = "stick right";

        if (buttons & dpad_up) inputs.back_input = "D-pad up";
        else if (buttons & dpad_left) inputs.back_input = "D-pad left";
        else if (buttons & c_up) inputs.back_input = "C up";
        else if (buttons & c_left) inputs.back_input = "C left";
        else if (y > stick_threshold) inputs.back_input = "stick up";
        else if (x < -stick_threshold) inputs.back_input = "stick left";

        inputs.forward = !inputs.forward_input.empty();
        inputs.back = !inputs.back_input.empty();
        inputs.action = (buttons & (button_a | button_b | button_start)) != 0;
        // The keyboard state only updates while the game window has focus.
        inputs.toggle = SDL_GetKeyboardState(nullptr)[toggle_key] != 0;
        return inputs;
    }

    void run() {
        while (!quit && rdram.load() == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (quit) {
            return;
        }

        Search search;
        log_line("Menu search is on. Press F9 in a menu to map it.");
        bm64::screen_reader::speak("Menu search is on. Press F9 in one of the game's menus to map it.", false);

        Inputs previous;
        bool waiting = false;
        bool spoiled = false;          // Another press arrived while waiting.
        bool spoiled_by_action = false;
        int pending_step = 0;
        std::string pending_input;
        Clock::time_point press_time{};
        bool beep_pending = false;     // A normal beep follows a low tone.
        Clock::time_point beep_time{};

        while (!quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            Inputs current = read_inputs();
            bool forward_pressed = current.forward && !previous.forward;
            bool back_pressed = current.back && !previous.back;
            bool action_pressed = current.action && !previous.action;
            bool toggle_pressed = current.toggle && !previous.toggle;
            previous = current;
            Clock::time_point now = Clock::now();

            if (beep_pending && now >= beep_time) {
                beep_pending = false;
                if (search.active && !waiting) {
                    tones::ready();
                }
            }

            if (waiting) {
                if (forward_pressed || back_pressed || action_pressed || toggle_pressed) {
                    spoiled = true;
                    spoiled_by_action = spoiled_by_action || action_pressed;
                    tones::too_soon();
                }
                if (now - press_time < settle_time) {
                    continue;
                }
                waiting = false;
                snapshot(search.after);
                if (spoiled_by_action) {
                    stop_menu(search, "Menu search stopped. Press F9 in the next menu.");
                }
                else if (spoiled) {
                    // Two moves in one measurement would eliminate the real
                    // cursor, so skip it and measure the next press from here.
                    log_line("Pressed again before the beep; that press wasn't measured.");
                    say("Too fast. Wait for the beep.");
                    search.baseline.swap(search.after);
                    tones::ready();
                }
                else {
                    // Measure from the memory after the previous press.
                    search.before.swap(search.baseline);
                    dump_snapshot(search.after, shot_name(search.menu, "press", search.press + 1));
                    bool follow_up = measure_press(search, pending_step, pending_input);
                    search.baseline.swap(search.after);
                    if (follow_up && search.active) {
                        beep_pending = true;
                        beep_time = now + settle_time;
                    }
                }
                spoiled = false;
                spoiled_by_action = false;
                continue;
            }

            if (toggle_pressed) {
                beep_pending = false;
                if (search.active) {
                    stop_menu(search, "Menu search stopped.");
                }
                else {
                    start_menu(search);
                }
                continue;
            }
            if (!search.active) {
                continue;
            }
            if (action_pressed) {
                // Choosing something or going back leaves this menu.
                beep_pending = false;
                stop_menu(search, "Menu search stopped. Press F9 in the next menu.");
                continue;
            }
            if (forward_pressed || back_pressed) {
                beep_pending = false;
                pending_step = forward_pressed ? 1 : -1;
                pending_input = forward_pressed ? current.forward_input : current.back_input;
                press_time = now;
                waiting = true;
            }
        }
    }
}

void bm64::menu_cursor_search::set_rdram(uint8_t* ram) {
    rdram = ram;
}

void bm64::menu_cursor_search::start() {
    if (std::getenv("BM64_MENU_SEARCH") == nullptr || worker.joinable()) {
        return;
    }
    parse_traces();
    if (const char* settle = std::getenv("BM64_MENU_SEARCH_SETTLE_MS")) {
        int ms = std::atoi(settle);
        if (ms > 0) {
            settle_time = std::chrono::milliseconds{ ms };
        }
    }
    quit = false;
    worker = std::thread{ run };
}

void bm64::menu_cursor_search::stop() {
    quit = true;
    if (worker.joinable()) {
        worker.join();
    }
}
