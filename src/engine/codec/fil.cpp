#include "codec/fil.h"

#include <algorithm>

#include "codec/adpcm.h"
#include "data/exe_symbols.h"

namespace blub {

bool FilMovie::parse(Bytes payload, uint16_t attr) {
    data = std::move(payload);
    subtitles.clear();
    palettes.clear();
    frames.clear();
    try {
        Reader r(data);
        if (attr & 0x4000) {
            const uint16_t n = r.u16();
            for (uint16_t k = 0; k < n; k++) {
                FilSubtitle s;
                s.page = r.u16(); s.text = r.u16(); s.x = r.s16(); s.y = r.s16();
                s.duration = r.u16(); s.start = r.u16(); s.flags = r.u16();
                r.skip(4);
                subtitles.push_back(s);
            }
        }
        r.skip(4);
        width = r.u16();
        height = r.u16();
        const uint16_t nframes = r.u16(), npal = r.u16();
        r.skip(2);
        rate = r.u16();
        adpcm_block = r.u16();
        r.skip(6);
        for (uint16_t k = 0; k < npal; k++) {
            Palette p;
            for (int i = 0; i < 768; i++) p[i] = uint8_t(std::min(255, r.u8() * 4));   // 6-bit VGA
            palettes.push_back(p);
        }
        r.skip(4u * nframes);                  // frame size table
        for (uint16_t k = 0; k < nframes; k++) {
            FilFrame f;
            f.header = r.ptr();
            uint32_t vs = r.u32();
            f.flags = uint8_t(vs >> 28);
            f.video_size = vs & 0x0FFFFFFF;
            f.palette = r.u16();
            f.audio_size = r.u32();
            f.codec = r.u16();
            f.width = r.u16();
            f.height = r.u16();
            r.skip(2);                         // the frame header is 18 bytes
            if (!rate) f.audio_size = 0;       // READ_FRAME ignores the field without sound
            f.video = r.ptr();
            r.skip(f.video_size);
            f.audio = r.ptr();
            r.skip(f.audio_size);
            frames.push_back(f);
        }
        return r.eof();
    } catch (const FormatError &) {
        return false;
    }
}

FilDecoder::FilDecoder(const ExeImage &exe) : exe_(exe) {
    arena_.write(exe.data_base, exe.data.data(), exe.data.size());
}

void FilDecoder::start(const FilMovie &m) {
    arena_.reset_heap();
    scratch_ = arena_.alloc(0x20000);
    tmp_ = arena_.alloc(0x40000);
    fb_ = arena_.alloc(640 * 480);
    vbuf_ = arena_.alloc(0x200000);
    std::fill_n(arena_.ptr(fb_), 640 * 480, 0);
    namespace S = exesym;
    arena_.w32(S::esp_col, scratch_);
    arena_.w32(S::Buff_Dcp_FID, tmp_);
    arena_.w32(S::pixi, fb_);
    arena_.w32(S::qspr + 4, fb_);            // FIL_ScreenDest (640 wide movies)
    arena_.w32(S::pspr, uint32_t(m.width));
    arena_.w32(S::qspr, uint32_t(m.height));
    arena_.w32(S::feoption + 4, vbuf_);      // FIL_VideoBuf
    pal_ = m.palettes.empty() ? Palette{} : m.palettes[0];
}

bool FilDecoder::decode(const FilMovie &m, size_t k) {
    namespace S = exesym;
    const FilFrame &f = m.frames[k];
    if (f.video_size > 0x200000) return false;
    arena_.write(vbuf_, f.video, f.video_size);
    arena_.write(S::FILSize1 + 4, f.header, 18);
    arena_.w32(S::esp_col + 8, uint32_t(f.flags) << 28);    // FIL_FrameFlags
    arena_.w32(S::FR_Disp, uint32_t(k));
    arena_.w8(S::feoption + 8, 0);                          // FIL_SkipDecode
    Cpu c;
    lifted_fil_frame(c, arena_);
    if (f.palette < m.palettes.size()) pal_ = m.palettes[f.palette];
    return c.fault == 0;
}

const uint8_t *FilDecoder::pixels() { return arena_.ptr(fb_); }

std::vector<int16_t> FilDecoder::audio(const FilMovie &m, size_t k) const {
    const FilFrame &f = m.frames[k];
    if (!f.audio_size) return {};
    if (m.adpcm_block) return decode_did_adpcm(f.audio, f.audio_size, uint32_t(m.adpcm_block));
    std::vector<int16_t> pcm(f.audio_size / 2);
    for (size_t i = 0; i < pcm.size(); i++) pcm[i] = int16_t(le16(f.audio + 2 * i));
    return pcm;
}

} // namespace blub
