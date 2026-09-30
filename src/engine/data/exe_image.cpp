#include "data/exe_image.h"

#include <vector>

namespace blub {

namespace {
struct Obj {
    uint32_t vsize, base, flags, pidx, pcnt;
};
} // namespace

bool ExeImage::load(const std::string &path, std::string *error) {
    Bytes d;
    try {
        d = read_file(path);
    } catch (const std::exception &e) {
        if (error) *error = e.what();
        return false;
    }
    return load_bytes(d, error);
}

bool ExeImage::load_bytes(const Bytes &d, std::string *error) {
    auto fail = [&](const char *m) {
        if (error) *error = m;
        return false;
    };
    if (d.size() < 0x40) return fail("not an EXE");
    const uint32_t le = le32(&d[0x3C]);
    if (le + 0xC4 > d.size() || d[le] != 'L' || d[le + 1] != 'E') return fail("not an LE executable");
    auto u32 = [&](uint32_t o) { return le32(&d[le + o]); };
    const uint32_t npages = u32(0x14), page_size = u32(0x28), last_page = u32(0x2C);
    const uint32_t obj_tab = u32(0x40), nobj = u32(0x44), page_map = u32(0x48);
    const uint32_t fix_page = u32(0x68), fix_rec = u32(0x6C), data_pages = u32(0x80);
    if (nobj != 2) return fail("unexpected object count (not DID.EXE?)");
    std::vector<Obj> objs(nobj);
    std::vector<Bytes> img(nobj);
    for (uint32_t i = 0; i < nobj; i++) {
        const uint8_t *o = &d[le + obj_tab + 24 * i];
        objs[i] = {le32(o), le32(o + 4), le32(o + 8), le32(o + 12), le32(o + 16)};
        img[i].assign(objs[i].vsize, 0);
        for (uint32_t k = 0; k < objs[i].pcnt; k++) {
            const uint8_t *e = &d[le + page_map + 4 * (objs[i].pidx + k - 1)];
            const uint32_t num = (uint32_t(e[0]) << 16) | (e[1] << 8) | e[2];
            const uint32_t off = data_pages + (num - 1) * page_size;
            const uint32_t sz = num == npages ? last_page : page_size;
            const uint32_t dst = k * page_size;
            for (uint32_t b = 0; b < sz && off + b < d.size() && dst + b < img[i].size(); b++)
                img[i][dst + b] = d[off + b];
        }
    }
    // relocations: only internal references of type 7 (32-bit offset) matter for the tables
    for (uint32_t oi = 0; oi < nobj; oi++) {
        for (uint32_t k = 0; k < objs[oi].pcnt; k++) {
            const uint32_t p = objs[oi].pidx + k;
            uint32_t pos = le + fix_rec + u32(fix_page + 4 * (p - 1));
            const uint32_t end = le + fix_rec + u32(fix_page + 4 * p);
            while (pos < end && pos + 2 < d.size()) {
                const uint8_t src = d[pos], flg = d[pos + 1];
                pos += 2;
                std::vector<int16_t> srcoffs;
                uint8_t cnt = 0;
                if (src & 0x20) cnt = d[pos++];
                else { srcoffs.push_back(int16_t(le16(&d[pos]))); pos += 2; }
                if (flg & 3) return fail("unsupported fixup");
                uint32_t tobj;
                if (flg & 0x40) { tobj = le16(&d[pos]); pos += 2; }
                else tobj = d[pos++];
                const uint8_t type = src & 0x0F;
                uint32_t toff = 0;
                if (type != 2) {
                    if (flg & 0x10) { toff = le32(&d[pos]); pos += 4; }
                    else { toff = le16(&d[pos]); pos += 2; }
                }
                for (uint8_t c = 0; c < cnt; c++) { srcoffs.push_back(int16_t(le16(&d[pos]))); pos += 2; }
                if (tobj < 1 || tobj > nobj) return fail("bad fixup target");
                const uint32_t target = objs[tobj - 1].base + toff;
                for (int16_t so : srcoffs) {
                    const int64_t at = int64_t(k) * page_size + so;
                    Bytes &im = img[oi];
                    if (at < 0 || at + 4 > int64_t(im.size())) continue;
                    uint32_t v = target;
                    if (type == 8) v = target - (objs[oi].base + uint32_t(at) + 4);
                    else if (type != 7) continue;
                    im[at] = uint8_t(v); im[at + 1] = uint8_t(v >> 8);
                    im[at + 2] = uint8_t(v >> 16); im[at + 3] = uint8_t(v >> 24);
                }
            }
        }
    }
    code_base = objs[0].base;
    data_base = objs[1].base;
    code = std::move(img[0]);
    data = std::move(img[1]);
    return true;
}

} // namespace blub
