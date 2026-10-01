// Keyboard (BIOS int 16h) and mouse (int 33h) from SDL events.
#include <SDL.h>

#include <algorithm>

#include "gfx/display.h"
#include "host/machine.h"

namespace blub {

namespace {

// PC scan code set 1 for SDL scan codes
uint8_t pc_scan(SDL_Scancode s) {
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03; case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05; case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09; case SDL_SCANCODE_9: return 0x0A;
    case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13; case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19; case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B; case SDL_SCANCODE_RETURN: return 0x1C;
    case SDL_SCANCODE_KP_ENTER: return 0x1C;
    case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_S: return 0x1F; case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_F: return 0x21; case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29; case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_Z: return 0x2C; case SDL_SCANCODE_X: return 0x2D; case SDL_SCANCODE_C: return 0x2E;
    case SDL_SCANCODE_V: return 0x2F; case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_F1: return 0x3B; case SDL_SCANCODE_F2: return 0x3C; case SDL_SCANCODE_F3: return 0x3D;
    case SDL_SCANCODE_F4: return 0x3E; case SDL_SCANCODE_F5: return 0x3F; case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41; case SDL_SCANCODE_F8: return 0x42; case SDL_SCANCODE_F9: return 0x43;
    case SDL_SCANCODE_F10: return 0x44;
    case SDL_SCANCODE_HOME: return 0x47; case SDL_SCANCODE_UP: return 0x48; case SDL_SCANCODE_PAGEUP: return 0x49;
    case SDL_SCANCODE_LEFT: return 0x4B; case SDL_SCANCODE_RIGHT: return 0x4D; case SDL_SCANCODE_END: return 0x4F;
    case SDL_SCANCODE_DOWN: return 0x50; case SDL_SCANCODE_PAGEDOWN: return 0x51;
    case SDL_SCANCODE_INSERT: return 0x52; case SDL_SCANCODE_DELETE: return 0x53;
    case SDL_SCANCODE_NONUSBACKSLASH: return 0x56;
    default: return 0;
    }
}

// UTF-8 text input -> code page 850 (German umlauts etc.)
int cp850(const char *utf8) {
    uint32_t cp = uint8_t(utf8[0]);
    if (cp >= 0xC0 && utf8[1]) cp = ((cp & 0x1F) << 6) | (uint8_t(utf8[1]) & 0x3F);
    if (cp < 0x80) return int(cp);
    switch (cp) {
    case 0xE4: return 0x84; case 0xF6: return 0x94; case 0xFC: return 0x81;
    case 0xC4: return 0x8E; case 0xD6: return 0x99; case 0xDC: return 0x9A;
    case 0xDF: return 0xE1; case 0xE9: return 0x82; case 0xE8: return 0x8A;
    case 0xE0: return 0x85; case 0xE7: return 0x87; case 0xB0: return 0xF8;
    default: return -1;
    }
}

uint8_t g_last_scan = 0;
SDL_Joystick *g_test_pad = nullptr;              // test scripts: virtual game controller

}  // namespace

void Machine::pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (clock == Clock::Replay && e.type != SDL_QUIT && e.type != SDL_WINDOWEVENT &&
            !(e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F2))
            continue;                         // a replay only gets the input of its recording
        switch (e.type) {
        case SDL_QUIT: quit_requested = true; break;
        case SDL_KEYDOWN: {
            const SDL_Keymod mod = SDL_GetModState();
            const SDL_Scancode s = e.key.keysym.scancode;
            if (s == SDL_SCANCODE_F11 || (s == SDL_SCANCODE_RETURN && (mod & KMOD_ALT))) {
                if (!e.key.repeat) display->toggle_fullscreen();
                break;
            }
            if (s == SDL_SCANCODE_F2) {                // hotspots on/off (the game never sees the key)
                if (!e.key.repeat) {
                    hotspots = !hotspots;
                    dirty = true;
                }
                break;
            }
            uint8_t scan = pc_scan(s);
            if (!scan) break;
            g_last_scan = scan;
            const SDL_Keycode k = e.key.keysym.sym;
            uint16_t key;
            if (mod & KMOD_ALT) {
                if (scan >= 0x3B && scan <= 0x44) key = uint16_t((scan + 0x2D) << 8);   // Alt+F1..F10
                else key = uint16_t(scan << 8);
            } else if (mod & KMOD_CTRL) {
                if (k >= 'a' && k <= 'z') key = uint16_t((scan << 8) | (k - 'a' + 1));
                else if (scan >= 0x3B && scan <= 0x44) key = uint16_t((scan + 0x23) << 8);  // Ctrl+F1..F10
                else break;
            } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                key = 0x1C0D;
            } else if (k == SDLK_ESCAPE) {
                key = cfg.esc_skips ? 0x3920 : 0x011B;   // the game only knows Space for skipping
            } else if (k == SDLK_BACKSPACE) {
                key = 0x0E08;
            } else if (k == SDLK_TAB) {
                key = (mod & KMOD_SHIFT) ? 0x0F00 : 0x0F09;
            } else if ((scan >= 0x3B && scan <= 0x44) || scan >= 0x47) {
                key = (mod & KMOD_SHIFT) && scan <= 0x44 ? uint16_t((scan + 0x19) << 8) : uint16_t(scan << 8);
            } else {
                break;                        // printable: arrives as SDL_TEXTINPUT
            }
            push_key(key);
            break;
        }
        case SDL_TEXTINPUT: {
            int ch = cp850(e.text.text);
            if (ch > 0) push_key(uint16_t((g_last_scan << 8) | ch));
            break;
        }
        case SDL_MOUSEMOTION:
            mouse_x = e.motion.x * mouse_scale;
            mouse_y = e.motion.y * mouse_scale;
            trace("input: mouse %d,%d", e.motion.x, e.motion.y);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int bit = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 :
                      e.button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
            if (e.type == SDL_MOUSEBUTTONDOWN) mouse_buttons |= bit; else mouse_buttons &= ~bit;
            trace("input: mouse button %d %s at %d,%d", e.button.button, e.type == SDL_MOUSEBUTTONDOWN ? "down" : "up", e.button.x, e.button.y);
            mouse_x = e.button.x * mouse_scale;
            mouse_y = e.button.y * mouse_scale;
            break;
        }
        case SDL_WINDOWEVENT:
            dirty = true;                     // shown, exposed, resized: draw the picture again
            break;
        case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERTOUCHPADDOWN: case SDL_CONTROLLERTOUCHPADMOTION: case SDL_CONTROLLERTOUCHPADUP:
            gamepad_event(e);
            break;
        default: break;
        }
    }
    if (clock != Clock::Replay) gamepad_move();
}

// scripted input for automated tests
void Machine::run_script() {
    const double t = now();
    while (!script.empty() && script.front().t <= t) {
        ScriptEvent e = script.front();
        script.pop_front();
        if (e.what == "move" || e.what == "click" || e.what == "rclick" || e.what == "press" || e.what == "release") {
            mouse_x = e.a * mouse_scale;
            mouse_y = e.b * mouse_scale;
            const int bit = e.what == "rclick" ? 2 : 1;
            if (e.what == "click" || e.what == "rclick" || e.what == "press") mouse_buttons |= bit;
            if (e.what == "click" || e.what == "rclick")
                script.push_front(ScriptEvent{t + 0.15, "release", e.a, e.b});
            if (e.what == "release") mouse_buttons &= ~1 & ~2;
        } else if (e.what == "buttons") {
            mouse_buttons = e.a;
        } else if (e.what == "pos") {                // mouse driver coordinates (recordings)
            mouse_x = e.a;
            mouse_y = e.b;
        } else if (e.what == "key") {
            push_key(uint16_t(e.a));
        } else if (e.what == "shot") {
            char name[64];
            std::snprintf(name, sizeof name, "/script_%04d.bmp", shot_no++);
            if (flush_window()) dirty = true;
            save_shot((cfg.shot_dir.empty() ? std::string(".") : cfg.shot_dir) + name);
        } else if (e.what == "hotspots") {
            hotspots = !hotspots;
            dirty = true;
        } else if (e.what == "hotspot_list") {
            print_hotspots();
        } else if (e.what == "padattach") {          // a virtual game controller, then its axes and buttons
            static SDL_Joystick *pad = nullptr;
            if (!pad) {
                const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                                            SDL_CONTROLLER_BUTTON_MAX, 0);
                if (index >= 0) pad = SDL_JoystickOpen(index);
            }
            g_test_pad = pad;
        } else if (e.what == "padaxis") {
            if (g_test_pad) SDL_JoystickSetVirtualAxis(g_test_pad, e.a, Sint16(e.b));
        } else if (e.what == "padbutton") {
            if (g_test_pad) SDL_JoystickSetVirtualButton(g_test_pad, e.a, Uint8(e.b));
        } else if (e.what == "quit") {
            quit_requested = true;
        }
    }
}

void Machine::int16(Cpu &r) {
    const uint8_t ah = uint8_t(r.eax >> 8);
    switch (ah) {
    case 0x00: case 0x10:                    // wait for a key
        while (keys.empty()) {
            poll();
            sleep_for(0.005);
        }
        r.eax = (r.eax & 0xFFFF0000u) | keys.front();
        keys.pop_front();
        break;
    case 0x01: case 0x11:                    // key available? ZF=1: no
        poll();
        r.f.zf = keys.empty();
        if (!keys.empty()) r.eax = (r.eax & 0xFFFF0000u) | keys.front();
        break;
    case 0x02: {                              // shift state
        SDL_Keymod mod = SDL_GetModState();
        uint8_t v = uint8_t(((mod & KMOD_RSHIFT) ? 1 : 0) | ((mod & KMOD_LSHIFT) ? 2 : 0) |
                            ((mod & KMOD_CTRL) ? 4 : 0) | ((mod & KMOD_ALT) ? 8 : 0));
        r.eax = (r.eax & 0xFFFFFF00u) | v;
        break;
    }
    default: break;
    }
}

void Machine::int33(Cpu &r) {
    const uint16_t ax = uint16_t(r.eax);
    auto lo = [](uint32_t &reg, uint32_t v) { reg = (reg & 0xFFFF0000u) | (v & 0xFFFF); };
    switch (ax) {
    case 0x0000:                              // reset
        lo(r.eax, 0xFFFF);
        lo(r.ebx, 2);
        break;
    case 0x0001: case 0x0002: break;         // show/hide: the game draws the pointer itself
    case 0x0003:
        // the loops that wait for a click (pause screen, message boxes) ask for the mouse without ever
        // waiting for the timer: sleep a little there instead of spinning
        if (++mouse_polls > 8) sleep_for(0.001);
        poll();
        present_if_due();                     // the pointer was drawn since the last request
        lo(r.ebx, uint32_t(mouse_buttons));
        lo(r.ecx, uint32_t(std::clamp(mouse_x, mx_min, mx_max)));
        lo(r.edx, uint32_t(std::clamp(mouse_y, my_min, my_max)));
        break;
    case 0x0004: {
        mouse_x = int16_t(r.ecx);
        mouse_y = int16_t(r.edx);
        display->warp(mouse_x / mouse_scale, mouse_y / mouse_scale);
        break;
    }
    case 0x0007: mx_min = int16_t(r.ecx); mx_max = int16_t(r.edx); if (mx_min > mx_max) std::swap(mx_min, mx_max); break;
    case 0x0008: my_min = int16_t(r.ecx); my_max = int16_t(r.edx); if (my_min > my_max) std::swap(my_min, my_max); break;
    case 0x000B: lo(r.ecx, 0); lo(r.edx, 0); break;
    case 0x000F: case 0x001A: case 0x001D: break;
    default: trace("int 33h ax=%04X ignored", ax); break;
    }
}

}  // namespace blub
