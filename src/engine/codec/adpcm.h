#pragma once
// IMA ADPCM in the two flavours used by DID:
//  * the engine's own blocks in FIL cinemas (Dcp_Adpcm, adpcm.ASM): word predictor, byte step index,
//    byte pad, then dwords whose nibbles are consumed from the most significant one;
//  * standard Microsoft/DVI IMA ADPCM WAV files (RIFF, format tag 0x11) in animations and sounds.
#include <cstdint>
#include <vector>

#include "util/bytes.h"

namespace blub {

// Dcp_Adpcm with one block of block_size bytes; returns 16-bit mono samples.
std::vector<int16_t> decode_did_adpcm(const uint8_t *src, size_t size, uint32_t block_size);

struct Pcm {
    int rate = 22050;
    int channels = 1;
    std::vector<int16_t> samples;     // interleaved
};

// RIFF WAVE with PCM (8/16 bit) or IMA ADPCM data
bool decode_wav(const uint8_t *data, size_t size, Pcm &out);

} // namespace blub
