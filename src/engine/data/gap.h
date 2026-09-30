#pragma once
// GAP asset database (see engine/hardware.c: IdSeek / IdSeekHead / IdRead in the decompilation).
//
// Layout: dword offset of the index, 9 dwords "Used" (cache sizes), entries..., index = dword offsets.
// Entry i spans [index[i], index[i+1]) and starts with an 18-byte HEAD.  Scripts address resources
// by id; the engine's PtrDataBase points 4 bytes into the index, so id n is index entry n + 1.
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "data/gamefs.h"
#include "util/bytes.h"

namespace blub {

enum ResType : uint16_t {
    RES_SPRITESET = 0x0000, RES_ZONE = 0x0014, RES_CAMERA = 0x8000, RES_SOUND_ANIM = 0x8001,
    RES_SOUND = 0x8002, RES_CINEMA = 0x8003, RES_IPOV = 0x8004, RES_ANIM = 0x8005, RES_IMAGE = 0x8006,
    RES_HIDING = 0x8007, RES_SPRITESET8 = 0x8008, RES_GRIDBLOCK = 0x800A, RES_TALK = 0x8011,
    RES_TALK_SPRITES = 0x8012, RES_ICONS = 0x8030, RES_CURSOR = 0x8031, RES_VALUES = 0x8033,
};

// struct HEAD of the debug info
struct ResHead {
    uint16_t type = 0;
    uint16_t attr = 0;
    int16_t x = 0, y = 0, w = 0, h = 0, depth = 0;
    uint32_t size = 0;
};

class GapArchive {
public:
    bool open(const std::string &path);
    bool open(std::unique_ptr<ReadFile> file, const std::string &name);   // e.g. inside an ISO image
    bool is_open() const { return f_ != nullptr; }
    const std::string &path() const { return path_; }

    int count() const { return int(offs_.size()) - 2; }          // number of script ids
    bool valid(int id) const;
    ResHead head(int id);
    Bytes payload(int id);                                        // data after the HEAD
    uint32_t payload_offset(int id) const { return offs_[id + 1] + 18; }  // absolute file offset
    uint32_t entry_end(int id) const { return offs_[id + 2]; }
    Bytes read_at(uint32_t file_offset, uint32_t size);           // IPOV tracks use absolute offsets

private:
    std::string path_;
    std::unique_ptr<ReadFile> f_;
    std::vector<uint32_t> offs_;
    uint32_t index_start_ = 0;
};

} // namespace blub
