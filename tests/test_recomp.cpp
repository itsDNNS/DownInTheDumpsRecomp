// Differential tests of single recompiled game functions (src/engine/recomp) against the port's
// reference implementations, which are themselves verified against the original code.
#include <cstdio>
#include <string>

#include "codec/adpcm.h"
#include "codec/fil.h"
#include "data/gamefs.h"
#include "data/gap.h"
#include "recomp/functions.h"

using namespace blub;

int recomp_failures = 0;

namespace {

void load(Arena &m, const ExeImage &exe) {
    m.write(exe.code_base, exe.code.data(), exe.code.size());
    m.write(exe.data_base, exe.data.data(), exe.data.size());
}

// call a recompiled function like the original code: return address on the stack
void call(Cpu &c, Arena &m, uint32_t addr) {
    c.esp -= 4;
    m.w32(c.esp, 0xFEEDF00Du);
    dispatch_address(c, m, addr);
}

// Dcp_Adpcm (adpcm.ASM): ecx blocks of SzBlockAdpcm bytes from esi, 16-bit samples to edi
void test_dcp_adpcm(GameData &game, const ExeImage &exe) {
    GapArchive gap;
    if (!gap.open(game.open_file({"DID", "DID.GAP"}), "DID.GAP")) {
        std::printf("FAIL: DID.GAP\n");
        recomp_failures++;
        return;
    }
    int checked = 0, bad = 0;
    for (int id : {2, 4}) {
        FilMovie mv;
        if (!mv.parse(gap.payload(id), gap.head(id).attr)) continue;
        for (size_t k = 0; k < mv.frames.size() && k < 40; k++) {
            const FilFrame &f = mv.frames[k];
            if (!f.audio_size) continue;
            auto ref = decode_did_adpcm(f.audio, f.audio_size, uint32_t(mv.adpcm_block));
            Arena m;
            load(m, exe);
            Cpu c;
            const uint32_t src = Arena::HEAP_BASE, dst = Arena::HEAP_BASE + 0x100000;
            m.write(src, f.audio, f.audio_size);
            m.w32(0x5119E, uint32_t(mv.adpcm_block));         // SzBlockAdpcm
            c.ecx = f.audio_size / uint32_t(mv.adpcm_block);
            c.esi = src;
            c.edi = dst;
            call(c, m, 0x1F8FB);
            size_t diff = ref.size();
            for (size_t i = 0; i < ref.size(); i++)
                if (int16_t(m.r16(dst + uint32_t(2 * i))) != ref[i]) {
                    diff = i;
                    break;
                }
            checked++;
            if (diff != ref.size()) {
                if (bad++ < 3)
                    std::printf("FAIL: Dcp_Adpcm GAP %d frame %zu: sample %zu of %zu differs (%d != %d)\n", id, k, diff,
                                ref.size(), int16_t(m.r16(dst + uint32_t(2 * diff))), ref[diff]);
            }
        }
    }
    std::printf("Dcp_Adpcm: %d blocks, %d mismatches\n", checked, bad);
    recomp_failures += bad;
}

}  // namespace

void test_recomp(GameData &game, const ExeImage &exe) {
    test_dcp_adpcm(game, exe);
}
