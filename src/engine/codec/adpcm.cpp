#include "codec/adpcm.h"

#include <algorithm>
#include <cstring>

namespace blub {

namespace {
const int16_t kSteps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
const int8_t kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct ImaState {
    int pred = 0, index = 0;
    // did: the engine's decoder computes the magnitude 7 as 2*step - step/8 (Dcp_Adpcm handlers),
    // which rounds differently from the standard sum
    int16_t step(unsigned nib, bool did = false) {
        const int st = kSteps[index];
        int diff = st >> 3;
        if (did && (nib & 7) == 7) {
            diff = 2 * st - (st >> 3);
        } else {
            if (nib & 4) diff += st;
            if (nib & 2) diff += st >> 1;
            if (nib & 1) diff += st >> 2;
        }
        pred = (nib & 8) ? pred - diff : pred + diff;
        pred = std::clamp(pred, -32768, 32767);
        index = std::clamp(index + kIndex[nib], 0, 88);
        return int16_t(pred);
    }
};
} // namespace

std::vector<int16_t> decode_did_adpcm(const uint8_t *src, size_t size, uint32_t block_size) {
    std::vector<int16_t> out;
    if (size < 4 || block_size < 8) return out;
    ImaState s;
    s.pred = int16_t(le16(src));
    s.index = std::min<int>(src[2], 88);
    out.push_back(int16_t(s.pred));
    const size_t ndw = block_size / 4 - 1;
    size_t p = 4;
    for (size_t k = 0; k < ndw && p + 4 <= size; k++, p += 4) {
        uint32_t v = (uint32_t(src[p]) << 24) | (uint32_t(src[p + 1]) << 16) | (uint32_t(src[p + 2]) << 8) | src[p + 3];
        for (int n = 0; n < 8; n++, v <<= 4) out.push_back(s.step(v >> 28, true));
    }
    return out;
}

bool decode_wav(const uint8_t *d, size_t size, Pcm &out) {
    if (size < 12 || std::memcmp(d, "RIFF", 4) || std::memcmp(d + 8, "WAVE", 4)) return false;
    uint16_t fmt = 0, ch = 1, bits = 16, align = 0;
    uint32_t rate = 22050;
    const uint8_t *data = nullptr;
    size_t dlen = 0;
    for (size_t p = 12; p + 8 <= size;) {
        const uint32_t len = le32(d + p + 4);
        const uint8_t *c = d + p + 8;
        if (!std::memcmp(d + p, "fmt ", 4) && len >= 16) {
            fmt = le16(c); ch = le16(c + 2); rate = le32(c + 4); align = le16(c + 12); bits = le16(c + 14);
        } else if (!std::memcmp(d + p, "data", 4)) {
            data = c;
            dlen = std::min<size_t>(len, size - (p + 8));
        }
        p += 8 + len + (len & 1);
    }
    if (!data || !ch) return false;
    out.rate = int(rate);
    out.channels = ch;
    out.samples.clear();
    if (fmt == 1 && bits == 16) {
        for (size_t i = 0; i + 1 < dlen; i += 2) out.samples.push_back(int16_t(le16(data + i)));
    } else if (fmt == 1 && bits == 8) {
        for (size_t i = 0; i < dlen; i++) out.samples.push_back(int16_t((int(data[i]) - 128) << 8));
    } else if (fmt == 0x11 && align >= 4 * ch) {
        // IMA ADPCM: per channel header (pred, index, pad), then 4-byte groups per channel,
        // low nibble first
        for (size_t b = 0; b < dlen; b += align) {
            const uint8_t *blk = data + b;
            const size_t blen = std::min<size_t>(align, dlen - b);
            if (blen < 4u * ch) break;
            std::vector<ImaState> st(ch);
            std::vector<std::vector<int16_t>> chan(ch);
            for (int c = 0; c < ch; c++) {
                st[c].pred = int16_t(le16(blk + 4 * c));
                st[c].index = std::min<int>(blk[4 * c + 2], 88);
                chan[c].push_back(int16_t(st[c].pred));
            }
            for (size_t p = 4u * ch; p + 4u * ch <= blen; p += 4u * ch)
                for (int c = 0; c < ch; c++)
                    for (int k = 0; k < 4; k++) {
                        const uint8_t byte = blk[p + 4 * c + k];
                        chan[c].push_back(st[c].step(byte & 15));
                        chan[c].push_back(st[c].step(byte >> 4));
                    }
            for (size_t i = 0; i < chan[0].size(); i++)
                for (int c = 0; c < ch; c++) out.samples.push_back(chan[c][i]);
        }
    } else {
        return false;
    }
    return true;
}

} // namespace blub
