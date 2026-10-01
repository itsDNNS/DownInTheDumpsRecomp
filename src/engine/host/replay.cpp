// Recordings: the input of a played session, played back exactly - for tests over long stretches of
// the game (blub --record FILE, blub --replay FILE --checkpoints FILE).
//
// With a recording the game runs on a virtual clock (Machine::sleep_for, Machine::tick) that only
// advances where the game waits or polls, and the input reaches the game only at the moments the
// events are pumped (every 5 ms of game time). The same input at the same virtual times then gives
// exactly the same game. While recording, the virtual clock is held to real time (the player plays
// normally); a replay runs as fast as the computer can. Checkpoints are hashes of the visible picture
// and palette at every second of game time: two runs that behave the same write the same file.
//
// A recording is a test script (see load_script in main.cpp): "<time> pos <x> <y>" (mouse driver
// coordinates), "<time> buttons <mask>", "<time> key <BIOS key, hex>", "<time> quit".
#include <SDL.h>

#include <algorithm>
#include <cinttypes>

#include "gfx/display.h"
#include "host/machine.h"

namespace blub {

bool Machine::start_recordings(std::string *error) {
    if (!cfg.record.empty()) {
        rec = std::fopen(cfg.record.c_str(), "wb");      // LF on every system
        if (!rec) {
            if (error) *error = "cannot write " + cfg.record;
            return false;
        }
        std::fprintf(rec, "# blub recording (blub %s) - replay with: blub --replay <this file>\n", cfg.version.c_str());
        std::string args;
        for (const std::string &a : cfg.args) args += (args.empty() ? "" : " ") + a;
        std::fprintf(rec, "# blub-recording dualpage=%d sound=%d args=%s\n", cfg.dualpage ? 1 : 0, cfg.nosound ? 0 : 1,
                     args.c_str());
        clock = Clock::Record;
    }
    // replays and the automatic exploration run as fast as possible (and take no input of the player)
    if (!cfg.replay.empty() || cfg.explore) clock = Clock::Replay;
    if (cfg.explore) explore_rng = cfg.explore;
    if (!cfg.checkpoints.empty()) {
        checks = std::fopen(cfg.checkpoints.c_str(), "wb");
        if (!checks) {
            if (error) *error = "cannot write " + cfg.checkpoints;
            return false;
        }
    }
    vclock = 0;
    return true;
}

void Machine::stop_recordings() {
    if (rec) {
        std::fprintf(rec, "%.17g quit\n", now());
        std::fclose(rec);
        rec = nullptr;
    }
    if (checks) {
        std::fclose(checks);
        checks = nullptr;
    }
}

void Machine::push_key(uint16_t key) {
    if (keys.size() >= 16) return;
    keys.push_back(key);
    if (rec) std::fprintf(rec, "%.17g key %04X\n", now(), key);
}

// the state the game will see; within one event round the order of the changes does not matter
void Machine::record_input() {
    if (!rec) return;
    const double t = now();
    if (mouse_x != rec_x || mouse_y != rec_y) {
        std::fprintf(rec, "%.17g pos %d %d\n", t, mouse_x, mouse_y);
        rec_x = mouse_x;
        rec_y = mouse_y;
    }
    if (mouse_buttons != rec_buttons) {
        std::fprintf(rec, "%.17g buttons %d\n", t, mouse_buttons);
        rec_buttons = mouse_buttons;
    }
}

// every second of game time: the checkpoint hash, and the progress of a replay
void Machine::checkpoint() {
    if (checks) {
        if (flush_window()) dirty = true;
        uint64_t h = 1469598103934665603ull;     // FNV-1a over the visible picture and the palette
        auto add = [&](const uint8_t *p, size_t n) {
            for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
        };
        if (graphics) {
            const uint32_t start = std::min<uint32_t>(display_start, uint32_t(vram.size() - SCREEN_W * SCREEN_H));
            add(&vram[start], size_t(SCREEN_W) * SCREEN_H);
            add(pal.data(), pal.size());
        }
        std::fprintf(checks, "%.0f %016" PRIx64 "\n", next_check, h);
    }
    if (clock == Clock::Replay && int(next_check) % 60 == 0) {
        std::printf("replay: %d min of game time\n", int(next_check) / 60);
        std::fflush(stdout);
    }
    next_check += 1.0;
}

}  // namespace blub
