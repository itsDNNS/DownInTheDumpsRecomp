#pragma once
// FIL cinemas (GAP 0x8003, camera travels 0x8004): container parsing and frame decoding.
// The video codecs are the recompiled original routines (lifted_fil_frame), the audio is the
// engine's IMA ADPCM variant.
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "codec/image.h"
#include "codec/lifted.h"
#include "data/exe_image.h"

namespace blub {

struct FilSubtitle {
    uint16_t page, text;
    int16_t x, y;
    uint16_t duration, start, flags;
};

struct FilFrame {
    uint16_t codec = 0, palette = 0, width = 0, height = 0;
    uint8_t flags = 0;
    const uint8_t *header = nullptr;      // the 18-byte frame header
    const uint8_t *video = nullptr;
    uint32_t video_size = 0;
    const uint8_t *audio = nullptr;
    uint32_t audio_size = 0;
};

struct FilMovie {
    Bytes data;                           // owns the payload
    std::vector<FilSubtitle> subtitles;
    int width = 0, height = 0, rate = 0, adpcm_block = 0;
    std::vector<Palette> palettes;        // already scaled to 8 bit
    std::vector<FilFrame> frames;
    // attr: GAP attribute; bit 0x4000 = subtitle table in front of the header
    bool parse(Bytes payload, uint16_t attr);
};

class FilDecoder {
public:
    explicit FilDecoder(const ExeImage &exe);
    void start(const FilMovie &m);
    // decode frame k (frames must be decoded in order); returns false on a codec fault
    bool decode(const FilMovie &m, size_t k);
    const uint8_t *pixels();                 // width*height indices of the current frame
    const Palette &palette() const { return pal_; }
    std::vector<int16_t> audio(const FilMovie &m, size_t k) const;

private:
    Arena arena_;
    const ExeImage &exe_;
    uint32_t scratch_ = 0, tmp_ = 0, fb_ = 0, vbuf_ = 0;
    Palette pal_{};
    std::vector<uint8_t> out_;
};

} // namespace blub
