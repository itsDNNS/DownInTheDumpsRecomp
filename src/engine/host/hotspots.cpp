// Hotspot display (F2): the clickable areas of the current scene, drawn over the picture.
//
// The game keeps its buttons in a tree of LISTBUTTON nodes below FirstButton (atoms.ASM) and tests
// the mouse against them every frame (ScrutAllButtons / ScrutLevelButtons / ScrutOneButton in
// atomsc.c): a node is {Frere, Fils, Pere, Adr} (next sibling, first child, parent, BUTTON), a
// switched-off button (type bit 14) hides its whole subtree, and a button is a rectangle
// {XButton, YButton, LButton, HButton} in screen coordinates, optionally narrowed by a shape mask of
// 8x8 (type bit 0) or 16x16 (type bit 1) cells stored behind the BUTTON. What a button does are the
// script methods AdrProg[IndMETH + 1..3] (enter, leave, click; IndMETH = 4 while an object is the
// cursor). The nodes live in the program image, the BUTTONs on the heap. This only reads the game's
// memory.
#include "data/exe_symbols.h"
#include "gfx/display.h"
#include "host/machine.h"

namespace blub {

namespace {
// LISTBUTTON
constexpr uint32_t LB_FRERE = 0, LB_FILS = 4, LB_ADR = 12;
// BUTTON
constexpr uint32_t B_TYPE = 4, B_PROG = 8, B_X = 24, B_Y = 26, B_L = 28, B_H = 30, B_SIZE = 34;

constexpr uint32_t CLICK = 0xFFD000FFu;          // a click does something: yellow
constexpr uint32_t CLICK_FILL = 0xFFD00030u;
constexpr uint32_t HOVER = 0x40E0FFC0u;           // only reacts to the pointer: light blue
constexpr uint32_t HOVER_FILL = 0x40E0FF20u;
}  // namespace

void Machine::hotspot_boxes(std::vector<OverlayBox> &out) {
    // the cursor is switched off (sequences, videos): no button reacts
    if (m.r16(exesym::Mouse) >= 0xFFFE) return;
    const uint32_t ind = m.r32(exesym::IndMETH) == 4 ? 4 : 0;
    auto valid = [](uint32_t a) { return a >= 0x40000 && a < Arena::SIZE - 0x100; };
    int budget = 2000;                               // the tree is small; never loop on a bad link
    auto walk = [&](auto &&self, uint32_t node) -> void {
        for (; valid(node) && budget-- > 0; node = m.r32(node + LB_FRERE)) {
            const uint32_t but = m.r32(node + LB_ADR);
            if (!valid(but)) continue;
            const uint16_t type = m.r16(but + B_TYPE);
            if (type & 0x4000) continue;             // switched off, children included
            const int x = int16_t(m.r16(but + B_X)), y = int16_t(m.r16(but + B_Y));
            const int w = int16_t(m.r16(but + B_L)), h = int16_t(m.r16(but + B_H));
            const bool click = m.r16(but + B_PROG + 2 * (ind + 3)) != 0;
            const bool hover = m.r16(but + B_PROG + 2 * (ind + 1)) || m.r16(but + B_PROG + 2 * (ind + 2));
            // the scene itself (a button as large as the screen) is no hotspot
            const bool whole = w * h >= SCREEN_W * SCREEN_H / 2;
            if ((click || hover) && !whole && w > 0 && h > 0) {
                const uint32_t line = click ? CLICK : HOVER, fill = click ? CLICK_FILL : HOVER_FILL;
                const int n = (type & 0xF0) ? ((type & 1) ? 8 : (type & 2) ? 16 : 0) : 0;
                if (n) {
                    // shaped button: the cells of its mask; the game maps pixel X to cell X * n / L
                    auto bit = [&](int r, int c) {
                        if (r < 0 || r >= n || c < 0 || c >= n) return false;
                        const uint32_t row = n == 8 ? m.r8(but + B_SIZE + r)
                                                    : uint32_t(m.r8(but + B_SIZE + 2 * r)) << 8 | m.r8(but + B_SIZE + 2 * r + 1);
                        return (row >> (n - 1 - c) & 1) != 0;
                    };
                    auto ex = [&](int c) { return x + (c * w + n - 1) / n; };   // first pixel of column c
                    auto ey = [&](int r) { return y + (r * h + n - 1) / n; };
                    // filled runs per row, then the outline: cell edges without a neighbour, merged
                    for (int r = 0; r < n; r++)
                        for (int c = 0; c < n;) {
                            if (!bit(r, c)) { c++; continue; }
                            const int c0 = c;
                            while (c < n && bit(r, c)) c++;
                            out.push_back({ex(c0), ey(r), ex(c) - ex(c0), ey(r + 1) - ey(r), fill, true});
                        }
                    for (int side = 0; side < 2; side++)     // top and bottom edges
                        for (int r = 0; r < n; r++)
                            for (int c = 0; c < n;) {
                                const int nr = side ? r + 1 : r - 1;
                                if (!bit(r, c) || bit(nr, c)) { c++; continue; }
                                const int c0 = c;
                                while (c < n && bit(r, c) && !bit(nr, c)) c++;
                                out.push_back({ex(c0), side ? ey(r + 1) - 1 : ey(r), ex(c) - ex(c0), 1, line, true});
                            }
                    for (int side = 0; side < 2; side++)     // left and right edges
                        for (int c = 0; c < n; c++)
                            for (int r = 0; r < n;) {
                                const int nc = side ? c + 1 : c - 1;
                                if (!bit(r, c) || bit(r, nc)) { r++; continue; }
                                const int r0 = r;
                                while (r < n && bit(r, c) && !bit(r, nc)) r++;
                                out.push_back({side ? ex(c + 1) - 1 : ex(c), ey(r0), 1, ey(r) - ey(r0), line, true});
                            }
                } else {
                    out.push_back({x, y, w, h, fill, true});
                    out.push_back({x, y, w, h, line, false});
                }
            }
            self(self, m.r32(node + LB_FILS));
        }
    };
    walk(walk, m.r32(exesym::FirstButton + LB_FILS));
}

}  // namespace blub
