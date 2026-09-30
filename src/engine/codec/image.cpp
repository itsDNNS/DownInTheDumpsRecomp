#include "codec/image.h"

#include <algorithm>

namespace blub {

bool decode_pcx(const Bytes &d, Indexed &out, Palette *pal) {
    if (d.size() < 128 || d[0] != 0x0A) return false;
    const int xmin = le16(&d[4]), ymin = le16(&d[6]), xmax = le16(&d[8]), ymax = le16(&d[10]);
    const int bpl = le16(&d[66]);
    const int w = xmax - xmin + 1, h = ymax - ymin + 1;
    if (w <= 0 || h <= 0 || bpl < w) return false;
    const bool has_pal = d.size() > 769 && d[d.size() - 769] == 0x0C;
    if (pal && has_pal) std::copy(d.end() - 768, d.end(), pal->begin());
    std::vector<uint8_t> raw;
    raw.reserve(size_t(bpl) * h);
    const size_t end = d.size() - (has_pal ? 769 : 0);
    size_t p = 128;
    while (raw.size() < size_t(bpl) * h && p < end) {
        uint8_t b = d[p++];
        if (b >= 0xC0) {
            const int n = b & 0x3F;
            const uint8_t v = p < end ? d[p++] : 0;
            raw.insert(raw.end(), n, v);
        } else {
            raw.push_back(b);
        }
    }
    raw.resize(size_t(bpl) * h, 0);
    out = Indexed();
    out.w = w;
    out.h = h;
    out.pix.resize(size_t(w) * h);
    out.mask.assign(size_t(w) * h, 255);
    for (int y = 0; y < h; y++) std::copy_n(&raw[size_t(y) * bpl], w, &out.pix[size_t(y) * w]);
    return true;
}

size_t decode_rle_rows(const uint8_t *d, size_t size, size_t p, int w, int h, Indexed &out) {
    out.w = w;
    out.h = h;
    out.pix.assign(size_t(std::max(w, 0)) * std::max(h, 0), 0);
    out.mask.assign(out.pix.size(), 0);
    for (int y = 0; y < h; y++) {
        int x = 0;
        while (p < size) {
            x += d[p++];
            if (x >= w || p >= size) break;
            const int n = d[p++];
            const int k = std::max(0, std::min(n, w - x));
            const size_t avail = p < size ? std::min<size_t>(size - p, size_t(k)) : 0;
            std::copy_n(d + p, avail, &out.pix[size_t(y) * w + x]);
            std::fill_n(&out.mask[size_t(y) * w + x], k, uint8_t(255));
            p += n;
            x += n;
            if (x >= w) break;
        }
    }
    return p;
}

std::vector<Indexed> decode_sprite(const Bytes &data) { return decode_sprite(data.data(), data.size()); }

std::vector<Indexed> decode_sprite(const uint8_t *d, size_t size) {
    std::vector<Indexed> frames;
    if (size < 12) return frames;
    const size_t base = 4;
    const int w = le16(d + base);
    // frame offset table until the first frame's data begins
    std::vector<uint32_t> offs;
    uint32_t first = 0;
    for (size_t k = base + 4; k + 4 <= size; k += 4) {
        if (first && k >= base + first) break;
        const uint32_t o = le32(d + k);
        if (o) first = first ? std::min(first, o) : o;
        offs.push_back(o);
    }
    for (uint32_t o : offs) {
        Indexed f;
        if (o && base + o + 4 <= size) {
            const int skip_y = le16(d + base + o), lines = le16(d + base + o + 2);
            Indexed rows;
            decode_rle_rows(d, size, base + o + 4, w, lines, rows);
            f.w = w;
            f.h = skip_y + lines;
            f.pix.assign(size_t(w) * skip_y, 0);
            f.mask.assign(size_t(w) * skip_y, 0);
            f.pix.insert(f.pix.end(), rows.pix.begin(), rows.pix.end());
            f.mask.insert(f.mask.end(), rows.mask.begin(), rows.mask.end());
        }
        frames.push_back(std::move(f));
    }
    return frames;
}

std::vector<Indexed> decode_dbd(const Bytes &d) {
    std::vector<Indexed> frames;
    if (d.size() < 8) return frames;
    const uint32_t n = le32(d.data()) / 4;
    for (uint32_t k = 0; k < n && 4 * k + 4 <= d.size(); k++) {
        const uint32_t o = le32(&d[4 * k]);
        Indexed f;
        if (o && o + 8 <= d.size()) {
            f.x = les16(&d[o]);
            f.y = les16(&d[o + 2]);
            const int w = le16(&d[o + 4]), h = le16(&d[o + 6]);
            decode_rle_rows(d.data(), d.size(), o + 8, w, h, f);
            f.x = les16(&d[o]);
            f.y = les16(&d[o + 2]);
        }
        frames.push_back(std::move(f));
    }
    while (!frames.empty() && frames.back().empty()) frames.pop_back();   // last offset = end of data
    return frames;
}

std::vector<Indexed> decode_icons(const Bytes &d) {
    std::vector<Indexed> icons;
    if (d.size() < 4) return icons;
    const uint32_t n = le32(d.data()) / 4;
    for (uint32_t k = 0; k < n && 4 * k + 4 <= d.size(); k++) {
        const uint32_t o = le32(&d[4 * k]);
        Indexed f;
        if (o && o + 8 <= d.size()) decode_rle_rows(d.data(), d.size(), o + 8, le16(&d[o + 4]), le16(&d[o + 6]), f);
        icons.push_back(std::move(f));
    }
    return icons;
}

} // namespace blub
