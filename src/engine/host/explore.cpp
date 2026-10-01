// Automatic exploration (blub --explore SEED): the game plays by itself, to find errors in parts of
// the game nobody has played through yet. Whenever the game waits for the player, a pseudo-random
// action is chosen from what the scene offers: a click on one of its hotspots (preferring the ones
// not clicked yet in this scene), an object from the inventory used on a hotspot, a walk to some
// spot, sometimes the space bar.
//
// Objects are used like a player would: the object is held over a few hotspots, and where the game
// answers with a hint (a SubTitle like "put the object there" while an object is the pointer), it
// is used there.
//
// The top bar is used only to save the game every 15 minutes (first slot, name TEST) and to load
// that save 2 minutes later, so saving and loading get tested in every chapter and situation.
//
// It runs on the virtual clock as fast as a replay; the choices depend only on the seed and the
// game, so the same seed gives the same run, and with --record the run becomes a recording.
#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "data/exe_symbols.h"
#include "gfx/display.h"
#include "host/machine.h"

namespace blub {

namespace {
constexpr uint32_t CURRENT_POV = 0x5214C;        // Variable[0]: the scene
constexpr int BAR_Y = 420;                       // the inventory bar at the bottom
constexpr int TOP_Y = 60;                        // the bar with save/load/quit at the top
constexpr int PROBES = 6;                        // hotspots an object is held over
constexpr double SAVE_EVERY = 900, LOAD_AFTER = 120;
constexpr uint32_t X_SPOT = 0x52176, Y_SPOT = 0x52178;   // where the game sees the pointer
enum Kind { MOVE, PRESS, RELEASE, KEY, PICK_SCENE, PICK_BAR, PROBE, TOP_CLICK, SAVE_SLOT, SAVE_NAME };
}  // namespace

uint32_t Machine::explore_random(uint32_t n) {
    uint32_t x = explore_rng;                    // xorshift32
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    explore_rng = x;
    return n ? x % n : 0;
}

// a hotspot of the scene or of the inventory bar; in the scene preferably one not tried yet with
// the current pointer (an object or the hand)
bool Machine::explore_choose(bool bar, std::pair<int, int> &out) {
    std::vector<std::pair<int, int>> all, pick;
    hotspot_targets(all);
    for (auto &p : all)
        if (bar ? p.second >= BAR_Y : (p.second >= TOP_Y && p.second < BAR_Y)) pick.push_back(p);
    if (pick.empty()) return false;
    const int scene = m.r16(CURRENT_POV), pointer = m.r16(exesym::Mouse);
    auto key = [&](const std::pair<int, int> &p) { return int64_t(pointer) << 32 | (p.first / 8) << 8 | (p.second / 8); };
    std::vector<std::pair<int, int>> fresh;
    for (auto &p : pick)
        if (!explored[scene].count(key(p))) fresh.push_back(p);
    const auto &from = !bar && !fresh.empty() && explore_random(10) < 7 ? fresh : pick;
    out = from[explore_random(uint32_t(from.size()))];
    if (!bar) explored[scene].insert(key(out));
    return true;
}

// the queue stays in time order: an action queued later can be due before ones queued earlier
// (the click on an object is due before the probing that was planned with it)
void Machine::explore_at(double t, int kind, int x, int y) {
    auto it = explore_queue.end();
    while (it != explore_queue.begin() && std::prev(it)->t > t) --it;
    explore_queue.insert(it, {t, kind, x, y});
}

void Machine::explore_click(int x, int y, double t) {
    explore_at(t, MOVE, x, y);
    explore_at(t + 0.1, PRESS);
    explore_at(t + 0.25, RELEASE);
}

// The top bar slides in once the pointer is up there, and the save screen shows its slots only after
// an animation (timing as in tests/recordings/chapter1_saveload). The game places the pointer sprite
// at the mouse minus its hotspot, kept on the screen: with a pointer whose hotspot is far down
// (sprite 2, the open hand) the clicks never get above y 30, where LOAD is. TOP_CLICK clicks only if
// the game's click point is on the button, else the rest of the save or load is dropped.
//
// The name is typed only into the game's text input (Gets), which the click on the slot starts:
// it hides the pointer and waits for Enter, nothing else.
void Machine::explore_save(double t) {
    std::printf("explore %7.1f s: save the game\n", t);
    explore_at(t, MOVE, 320, 30);
    explore_at(t + 1, MOVE, 452, 45);            // the game sees it in its next frames (8 per second)
    explore_at(t + 1.5, TOP_CLICK, 452, 45);     // SAVE
    explore_slot_tries = 0;
    explore_at(t + 12, SAVE_SLOT);
    explore_next = t + 30;
    explore_save_at = t + SAVE_EVERY;
    explore_load_at = t + LOAD_AFTER;
}

void Machine::explore_load(double t) {
    std::printf("explore %7.1f s: load the game\n", t);
    explore_at(t, MOVE, 320, 30);
    explore_at(t + 1, MOVE, 452, 15);
    explore_at(t + 1.5, TOP_CLICK, 452, 15);     // LOAD
    explore_click(162, 168, t + 13);             // the first slot
    explore_next = t + 22;
    explore_load_at = -1;
}

void Machine::explore_step() {
    const double t = now();
    while (!explore_queue.empty() && explore_queue.front().t <= t) {
        const ExploreAction a = explore_queue.front();
        explore_queue.pop_front();
        std::pair<int, int> p;
        switch (a.kind) {
        case MOVE:
            mouse_x = a.x * mouse_scale;
            mouse_y = a.y * mouse_scale;
            break;
        case PRESS: mouse_buttons |= 1; break;
        case RELEASE: mouse_buttons &= ~1; break;
        case KEY: push_key(uint16_t(a.x)); break;
        case SAVE_SLOT:                              // the first slot of the save screen: a new save
            explore_at(t, MOVE, 162, 168);
            explore_at(t + 0.5, PRESS);              // after the title its pointer shows
            explore_at(t + 0.65, RELEASE);
            explore_slot_click = t + 0.5;
            explore_at(t + 2, SAVE_NAME);
            break;
        case SAVE_NAME:
            if (text_input_at >= explore_slot_click) {
                const uint16_t name[] = {0x1454, 0x1245, 0x1F53, 0x1454, 0x1C0D};   // T E S T, Enter
                for (int i = 0; i < 5; i++) explore_at(t + 0.2 * i, KEY, name[i]);
                explore_next = t + 10;
            } else if (++explore_slot_tries < 6) {   // the slot is not there yet
                explore_at(t + 1, SAVE_SLOT);
                explore_next = t + 10;
            } else {                                 // no slot: back to the game (outside of the slots)
                std::printf("explore %7.1f s: the save screen does not take the click\n", t);
                explore_click(320, 380, t);
                explore_next = t + 6;
            }
            break;
        case TOP_CLICK: {
            // the buttons of the top bar are 64 x 30
            const int xs = int16_t(m.r16(X_SPOT)), ys = int16_t(m.r16(Y_SPOT));
            if (std::abs(xs - a.x) < 32 && ys / 30 == a.y / 30) {
                explore_at(t, PRESS);
                explore_at(t + 0.15, RELEASE);
            } else {
                std::printf("explore %7.1f s: the pointer does not reach %d,%d (it clicks at %d,%d)\n", t, a.x, a.y, xs, ys);
                explore_queue.clear();
                if (a.y < 30) {                      // try again later
                    explore_load_at = t + 60;
                } else {
                    explore_save_at = t + 60;
                    explore_load_at = -1;
                }
                explore_next = t + 1;
            }
            break;
        }
        case PICK_SCENE:
            if (explore_choose(false, p)) {
                std::printf("explore %7.1f s: hotspot %d,%d (scene %d)\n", t, p.first, p.second, m.r16(CURRENT_POV));
                explore_click(p.first, p.second, t);
            }
            break;
        case PICK_BAR:
            if (explore_choose(true, p)) {
                std::printf("explore %7.1f s: object %d,%d\n", t, p.first, p.second);
                explore_click(p.first, p.second, t);
            } else {
                probe_tries = -1;                    // nothing in the inventory: no probing
            }
            break;
        case PROBE: {
            // the pointer is the object while one is held (0xF800 + its number)
            const int held = m.r16(exesym::Mouse);
            if (probe_tries < 0 || held < 0xF800 || held >= 0xFC00) break;
            if (probe_tries < PROBES && title_shown >= probe_from) {
                // the game answered the object over this hotspot with a hint: use it here
                std::printf("explore %7.1f s: use object %d on %d,%d (hint)\n", t, held - 0xF800, mouse_x / mouse_scale,
                            mouse_y / mouse_scale);
                explore_at(t, PRESS);
                explore_at(t + 0.15, RELEASE);
                probe_tries = -1;
            } else if (probe_tries > 0 && explore_choose(false, p)) {
                explore_at(t, MOVE, p.first, p.second);
                probe_from = t + 0.01;                 // texts from now on
                probe_tries--;
                explore_at(t + 0.45, PROBE);
            } else if (probe_tries == 0) {
                // no hint: try it on the last one anyway
                std::printf("explore %7.1f s: use object %d on %d,%d\n", t, held - 0xF800, mouse_x / mouse_scale,
                            mouse_y / mouse_scale);
                explore_at(t, PRESS);
                explore_at(t + 0.15, RELEASE);
                probe_tries = -1;
            }
            break;
        }
        default: break;
        }
    }
    if (!explore_queue.empty()) return;
    // the game's text input (Gets, no frames meanwhile) waits for Enter: started by a click of the
    // exploration on a saved game, it would wait forever
    if (text_input_at > frame_wait_at && t - text_input_at > 20) {
        std::printf("explore %7.1f s: Enter for the text input\n", t);
        explore_at(t, KEY, 0x1C0D);
        text_input_at = t;                       // again in 20 s if that was not enough
        return;
    }
    // the game waits for the player only while its pointer is shown
    if (m.r16(exesym::Mouse) >= 0xFFFE) {
        if (explore_quiet < 0) explore_quiet = t;
        if (t - explore_quiet > 25) {            // a long sequence: skip it now and then
            std::printf("explore %7.1f s: space bar\n", t);
            explore_at(t, KEY, 0x3920);
            explore_quiet = t;
        }
        return;
    }
    explore_quiet = -1;
    if (t < explore_next) return;
    if (m.r16(exesym::Mouse) < 0xF800) {         // the top bar does nothing while an object is held
        if (t >= explore_save_at) {
            explore_save(t);
            return;
        }
        if (explore_load_at >= 0 && t >= explore_load_at) {
            explore_load(t);
            return;
        }
    }
    explore_next = t + 1.0 + explore_random(2000) / 1000.0;
    const uint32_t r = explore_random(100);
    if (r < 55) {                                // a hotspot of the scene
        explore_at(t, PICK_SCENE);
    } else if (r < 85) {                         // an object of the inventory, held over hotspots
        explore_at(t, MOVE, 320, 450);
        explore_at(t + 0.8, PICK_BAR);
        probe_tries = PROBES;
        explore_at(t + 1.4, PROBE);
        explore_next += 4.0;
    } else if (r < 97) {                         // walk somewhere
        const int x = 20 + int(explore_random(600)), y = 150 + int(explore_random(260));
        std::printf("explore %7.1f s: walk %d,%d\n", t, x, y);
        explore_click(x, y, t);
    } else {
        std::printf("explore %7.1f s: space bar\n", t);
        explore_at(t, KEY, 0x3920);
    }
}

}  // namespace blub
