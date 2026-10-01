// VESA mode 101h (640x480x256) with a 64 KB bank window, VGA DAC and retrace, presented with SDL.
#include <SDL.h>

#include "gfx/display.h"
#include "host/machine.h"

namespace blub {

namespace {
constexpr uint32_t VRAM_KB = 2048;
constexpr uint16_t MODE_640x480 = 0x101;
}  // namespace

void Machine::flush_window() {
    const uint32_t off = bank * 0x10000u;
    if (off + layout::VGA_WINDOW_SIZE <= vram.size()) std::memcpy(&vram[off], m.ptr(layout::VGA_WINDOW), layout::VGA_WINDOW_SIZE);
}

void Machine::set_bank(uint32_t b) {
    if (b == bank) return;
    flush_window();
    bank = b;
    const uint32_t off = bank * 0x10000u;
    if (off + layout::VGA_WINDOW_SIZE <= vram.size())
        std::memcpy(m.ptr(layout::VGA_WINDOW), &vram[off], layout::VGA_WINDOW_SIZE);
    else
        std::memset(m.ptr(layout::VGA_WINDOW), 0, layout::VGA_WINDOW_SIZE);
    dirty = true;
}

void Machine::present(bool force) {
    last_present = uint64_t(now() * 1000.0);
    if (!graphics) return;
    flush_window();
    const uint32_t start = std::min<uint32_t>(display_start, uint32_t(vram.size() - SCREEN_W * SCREEN_H));
    std::vector<OverlayBox> boxes;
    if (hotspots) hotspot_boxes(boxes);
    const bool shot = !cfg.shot_dir.empty() && now() - last_shot >= cfg.shot_interval;
    char name[64];
    if (shot) {
        last_shot = now();
        std::snprintf(name, sizeof name, "/shot_%04d.bmp", shot_no++);
        if (cfg.shot_presented) display->capture_next(cfg.shot_dir + name);
    }
    display->present(&vram[start], pal, boxes);
    if (shot && !cfg.shot_presented) save_shot(cfg.shot_dir + name);
}

void Machine::save_shot(const std::string &path) {
    const uint32_t start = std::min<uint32_t>(display_start, uint32_t(vram.size() - SCREEN_W * SCREEN_H));
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(&vram[start], SCREEN_W, SCREEN_H, 8, SCREEN_W,
                                                        SDL_PIXELFORMAT_INDEX8);
    if (!s) return;
    SDL_Color colors[256];
    for (int i = 0; i < 256; i++) colors[i] = SDL_Color{pal[3 * i], pal[3 * i + 1], pal[3 * i + 2], 255};
    SDL_SetPaletteColors(s->format->palette, colors, 0, 256);
    SDL_SaveBMP(s, path.c_str());
    SDL_FreeSurface(s);
}

// VESA BIOS; es_di: linear address of the ES:DI buffer (real-mode calls)
bool Machine::vesa(uint32_t &eax, uint32_t &ebx, uint32_t &ecx, uint32_t &edx, uint32_t es_di) {
    const uint16_t fn = uint16_t(eax);
    auto lo = [](uint32_t &reg, uint32_t v) { reg = (reg & 0xFFFF0000u) | (v & 0xFFFF); };
    switch (fn) {
    case 0x4F00: {                           // controller information
        std::memset(m.ptr(es_di), 0, 256);
        m.write(es_di, "VESA", 4);
        m.w16(es_di + 4, 0x0102);
        m.w32(es_di + 10, 0);                  // capabilities
        // mode list right behind the block (inside the caller's buffer)
        m.w16(es_di + 0x100 - 4, MODE_640x480);
        m.w16(es_di + 0x100 - 2, 0xFFFF);
        m.w32(es_di + 14, ((es_di >> 4) << 16) | (0x100 - 4));
        m.w16(es_di + 18, VRAM_KB / 64);
        lo(eax, 0x004F);
        return true;
    }
    case 0x4F01: {                           // mode information
        std::memset(m.ptr(es_di), 0, 256);
        m.w16(es_di + 0x00, 0x009B);           // supported, color, graphics
        m.w8(es_di + 0x02, 0x07);              // window A: exists, readable, writable
        m.w8(es_di + 0x03, 0x00);
        m.w16(es_di + 0x04, 64);               // granularity KB
        m.w16(es_di + 0x06, 64);               // window size KB
        m.w16(es_di + 0x08, 0xA000);
        m.w16(es_di + 0x10, SCREEN_W);         // bytes per scan line
        m.w16(es_di + 0x12, SCREEN_W);
        m.w16(es_di + 0x14, SCREEN_H);
        m.w8(es_di + 0x16, 8);
        m.w8(es_di + 0x17, 16);
        m.w8(es_di + 0x18, 1);
        m.w8(es_di + 0x19, 8);
        m.w8(es_di + 0x1A, 1);
        m.w8(es_di + 0x1B, 4);                 // packed pixel
        m.w8(es_di + 0x1D, uint8_t(VRAM_KB * 1024 / (SCREEN_W * SCREEN_H) - 1));
        lo(eax, 0x004F);
        return true;
    }
    case 0x4F02:                              // set mode
        graphics = (ebx & 0x1FF) == MODE_640x480;
        std::fill(vram.begin(), vram.end(), 0);
        std::memset(m.ptr(layout::VGA_WINDOW), 0, layout::VGA_WINDOW_SIZE);
        bank = 0;
        display_start = 0;
        lo(eax, 0x004F);
        return true;
    case 0x4F03: lo(ebx, graphics ? MODE_640x480 : 3); lo(eax, 0x004F); return true;
    case 0x4F05:                              // bank window
        if ((ebx & 0xFF00) == 0x0100) {
            lo(edx, bank);
        } else if ((ebx & 0xFF) == 0) {
            set_bank(edx & 0xFFFF);
        }
        lo(eax, 0x004F);
        return true;
    case 0x4F07:                              // display start
        if ((ebx & 0xFF) == 0 || (ebx & 0xFF) == 0x80) {
            display_start = (edx & 0xFFFF) * SCREEN_W + (ecx & 0xFFFF);
            present(true);
        } else if ((ebx & 0xFF) == 1) {
            lo(ecx, display_start % SCREEN_W);
            lo(edx, display_start / SCREEN_W);
        }
        lo(eax, 0x004F);
        return true;
    default:
        trace("VESA %04X not implemented", fn);
        lo(eax, 0x014F);
        return false;
    }
}

void Machine::int10(Cpu &r) {
    const uint8_t ah = uint8_t(r.eax >> 8);
    if (ah == 0x4F) {
        vesa(r.eax, r.ebx, r.ecx, r.edx, r.sb[0] + r.edi);
        return;
    }
    switch (ah) {
    case 0x00:                                // set mode: only mode 3 (text) leaves the VESA mode
        trace("int 10h: set mode %02X", r.eax & 0x7F);
        if ((r.eax & 0x7F) == 3) graphics = false;
        break;
    case 0x0F:                                // get mode: 80x25 text
        r.eax = (r.eax & 0xFFFF0000u) | 0x5003;
        r.ebx &= 0xFFFF00FFu;
        break;
    default: trace("int 10h ah=%02X ignored", ah); break;
    }
}

uint32_t Machine::port_in(uint16_t port, int size) {
    switch (port) {
    case 0x3DA: {                             // input status: vertical retrace at 70 Hz
        double t = now() * 70.0;
        bool retrace = (t - double(uint64_t(t))) < 0.08;
        if (retrace && !last_retrace) {
            present(false);
            poll();
        }
        last_retrace = retrace;
        return retrace ? 0x09 : 0x00;
    }
    case 0x3C9: {
        uint8_t v = dac[dac_read % 768];
        dac_read = (dac_read + 1) % 768;
        return v;
    }
    case 0x61: return 0x20;
    default: return 0xFF;
    }
}

void Machine::port_out(uint16_t port, uint32_t value, int size) {
    if (size == 2) {                          // out dx, ax: two byte ports
        port_out(port, value & 0xFF, 1);
        port_out(uint16_t(port + 1), (value >> 8) & 0xFF, 1);
        return;
    }
    switch (port) {
    case 0x3C7: dac_read = int(value & 0xFF) * 3; break;
    case 0x3C8: {
        dac_write = int(value & 0xFF) * 3;
        static double last_log = -10;
        static int count = 0;
        count++;
        if (cfg.trace && now() - last_log > 1.0) {
            trace("DAC: %d palette uploads, pal[3*%u] = %u", count, value & 0xFF, dac[dac_write % 768]);
            last_log = now();
        }
        break;
    }
    case 0x3C9: {
        uint8_t v = uint8_t(value & 0x3F);
        dac[dac_write % 768] = v;
        pal[dac_write % 768] = uint8_t((v << 2) | (v >> 4));
        dac_write = (dac_write + 1) % 768;
        dirty = true;
        break;
    }
    default: break;
    }
}

}  // namespace blub
