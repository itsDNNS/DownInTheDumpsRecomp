#pragma once
// Still image formats of DID (bat256.ASM / perso.ASM in the decompilation):
//   PCX backgrounds (ReadPcx), the row RLE used by all sprites, DBO sprites (SendDBOsprite),
//   DBD sprite sets (AffDBD) and the inventory icon table (SpritObjAdr).
#include <array>
#include <cstdint>
#include <vector>

#include "util/bytes.h"

namespace blub {

using Palette = std::array<uint8_t, 768>;   // 8-bit RGB

struct Indexed {
    int x = 0, y = 0;       // placement (hot spot relative for DBD frames)
    int w = 0, h = 0;
    std::vector<uint8_t> pix;    // palette indices
    std::vector<uint8_t> mask;   // 255 where the sprite has pixels
    bool empty() const { return w <= 0 || h <= 0; }
};

// PCX, 8 bit, with the 768-byte palette after the 0x0C marker at the end.
bool decode_pcx(const Bytes &data, Indexed &out, Palette *pal = nullptr);

// Row RLE: per row (byte skip, byte count, count pixels)* until the width is reached.
// The last row of a set may end without its closing skip byte. Returns the end position.
size_t decode_rle_rows(const uint8_t *data, size_t size, size_t pos, int w, int h, Indexed &out);

// DBO sprite (0x8006 attr 0x10, 0x8031): dword ?, word w, word h, dword frame offsets relative to +4;
// frame = word skip_y, word lines, rows.  Missing frames are returned empty.
std::vector<Indexed> decode_sprite(const Bytes &data);
std::vector<Indexed> decode_sprite(const uint8_t *data, size_t size);

// DBD sprite set (0x0000, 0x8008, 0x8012, character .DBD files): dword offsets (offsets[0] = table
// size, last = end of data); frame = short x, short y, word w, word h, rows.
std::vector<Indexed> decode_dbd(const Bytes &data);

// Inventory icons (0x8030): dword offsets; entry = dword ?, word w, word h, rows.
std::vector<Indexed> decode_icons(const Bytes &data);

} // namespace blub
