// Game controllers (SDL game controller API). The game is played with the mouse only, so a
// controller moves the mouse pointer and clicks:
//   left stick, d-pad   move the pointer              right stick   move it slowly (aiming)
//   touchpad            move it like a laptop touchpad (DualShock 4, DualSense)
//   A, touchpad click   click                          B             skip video / sequence (space bar)
//   X                   show hotspots (F2)             LB / RB       pointer to the previous / next hotspot
//   Start               pause (P)                      Back          like Esc
#include <SDL.h>

#include <algorithm>
#include <cmath>

#include "gfx/display.h"
#include "host/machine.h"

namespace blub {

namespace {
constexpr double SPEED = 640;                    // screen pixels per second at full deflection
constexpr double SLOW = 0.3;                     // right stick
constexpr double DPAD = 220;
constexpr double DEAD_ZONE = 0.2;
constexpr double TOUCH = 1.4;                    // touchpad width -> screen widths

// stick position with a radial dead zone and a curve that is fine near the middle
void stick(SDL_GameController *g, SDL_GameControllerAxis ax, SDL_GameControllerAxis ay, double &x, double &y) {
    x = SDL_GameControllerGetAxis(g, ax) / 32767.0;
    y = SDL_GameControllerGetAxis(g, ay) / 32767.0;
    const double r = std::hypot(x, y);
    if (r < DEAD_ZONE) {
        x = y = 0;
        return;
    }
    const double k = std::min(1.0, (r - DEAD_ZONE) / (1 - DEAD_ZONE));
    x = x / r * k * k;
    y = y / r * k * k;
}
}  // namespace

void Machine::gamepad_event(const SDL_Event &e) {
    if (!cfg.gamepad) return;
    switch (e.type) {
    case SDL_CONTROLLERDEVICEADDED:
        if (SDL_GameController *g = SDL_GameControllerOpen(e.cdevice.which)) {
            pads[SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g))] = g;
            trace("controller: %s", SDL_GameControllerName(g) ? SDL_GameControllerName(g) : "?");
        }
        break;
    case SDL_CONTROLLERDEVICEREMOVED: {
        auto it = pads.find(e.cdevice.which);
        if (it != pads.end()) {
            SDL_GameControllerClose(it->second);
            pads.erase(it);
        }
        break;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        const bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
        trace("input: controller button %s %s", SDL_GameControllerGetStringForButton(SDL_GameControllerButton(e.cbutton.button)),
              down ? "down" : "up");
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A:
        case SDL_CONTROLLER_BUTTON_TOUCHPAD:
            if (down) mouse_buttons |= 1;
            else mouse_buttons &= ~1;
            break;
        case SDL_CONTROLLER_BUTTON_B:
            if (down) push_key(0x3920);                                     // space bar
            break;
        case SDL_CONTROLLER_BUTTON_BACK:
            if (down) push_key(cfg.esc_skips ? 0x3920 : 0x011B);
            break;
        case SDL_CONTROLLER_BUTTON_START:
            if (down) push_key(0x1970);                                     // P: pause
            break;
        case SDL_CONTROLLER_BUTTON_X:
            if (down) {
                hotspots = !hotspots;
                dirty = true;
            }
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (down) jump_to_hotspot(e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ? 1 : -1);
            break;
        default: break;
        }
        break;
    }
    case SDL_CONTROLLERTOUCHPADDOWN:
        touch_finger = e.ctouchpad.finger;
        touch_x = e.ctouchpad.x;
        touch_y = e.ctouchpad.y;
        break;
    case SDL_CONTROLLERTOUCHPADMOTION:
        if (e.ctouchpad.finger == touch_finger) {
            // the touchpads of these controllers are about twice as wide as high
            const double dx = (e.ctouchpad.x - touch_x) * TOUCH * SCREEN_W, dy = (e.ctouchpad.y - touch_y) * TOUCH * SCREEN_W / 2;
            touch_x = e.ctouchpad.x;
            touch_y = e.ctouchpad.y;
            if (pad_mouse_x != mouse_x || pad_mouse_y != mouse_y) {        // the mouse moved it since
                pad_x = mouse_x / double(mouse_scale);
                pad_y = mouse_y / double(mouse_scale);
            }
            pad_x = std::clamp(pad_x + dx, 0.0, SCREEN_W - 1.0);
            pad_y = std::clamp(pad_y + dy, 0.0, SCREEN_H - 1.0);
            mouse_x = pad_mouse_x = int(pad_x) * mouse_scale;
            mouse_y = pad_mouse_y = int(pad_y) * mouse_scale;
        }
        break;
    case SDL_CONTROLLERTOUCHPADUP:
        if (e.ctouchpad.finger == touch_finger) touch_finger = -1;
        break;
    default: break;
    }
}

// the sticks and the d-pad move the pointer (called with the events, about every 5 ms)
void Machine::gamepad_move() {
    const double t = now(), dt = pad_time < 0 ? 0 : std::min(0.05, t - pad_time);
    pad_time = t;
    if (pads.empty() || !cfg.gamepad) return;
    double vx = 0, vy = 0;
    for (auto &[id, g] : pads) {
        double x, y;
        stick(g, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, x, y);
        vx += x * SPEED;
        vy += y * SPEED;
        stick(g, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, x, y);
        vx += x * SPEED * SLOW;
        vy += y * SPEED * SLOW;
        vx += DPAD * (SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) -
                      SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
        vy += DPAD * (SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_DPAD_DOWN) -
                      SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_DPAD_UP));
    }
    if (vx == 0 && vy == 0) return;
    if (pad_mouse_x != mouse_x || pad_mouse_y != mouse_y) {    // start from where the mouse left it
        pad_x = mouse_x / double(mouse_scale);
        pad_y = mouse_y / double(mouse_scale);
    }
    pad_x = std::clamp(pad_x + vx * dt, 0.0, SCREEN_W - 1.0);
    pad_y = std::clamp(pad_y + vy * dt, 0.0, SCREEN_H - 1.0);
    mouse_x = pad_mouse_x = int(pad_x) * mouse_scale;
    mouse_y = pad_mouse_y = int(pad_y) * mouse_scale;
}

// LB / RB: the pointer jumps to the previous / next hotspot in reading order (rows of 40 pixels)
void Machine::jump_to_hotspot(int dir) {
    std::vector<std::pair<int, int>> t;
    hotspot_targets(t);
    if (t.empty()) return;
    auto key = [](int x, int y) { return (y / 40) * 1000 + x; };
    std::sort(t.begin(), t.end(), [&](auto &a, auto &b) { return key(a.first, a.second) < key(b.first, b.second); });
    const int px = mouse_x / mouse_scale, py = mouse_y / mouse_scale, here = key(px, py);
    size_t pick;
    if (dir > 0) {
        pick = 0;                                    // the first one after the pointer, else wrap around
        while (pick < t.size() && key(t[pick].first, t[pick].second) <= here) pick++;
        if (pick == t.size()) pick = 0;
    } else {
        pick = t.size() - 1;                         // the last one before the pointer, else wrap around
        while (pick > 0 && key(t[pick].first, t[pick].second) >= here) pick--;
        if (key(t[pick].first, t[pick].second) >= here) pick = t.size() - 1;
    }
    pad_x = t[pick].first;
    pad_y = t[pick].second;
    mouse_x = pad_mouse_x = t[pick].first * mouse_scale;
    mouse_y = pad_mouse_y = t[pick].second * mouse_scale;
}

}  // namespace blub
