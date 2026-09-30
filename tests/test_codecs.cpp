// Checks the port's codecs against reference data produced by the ORIGINAL code in an emulator
// (scripts/make_port_fixtures.py).  Needs the original game files:
//   blub_tests <fixtures dir> [game data]     (ISO folder or CD contents; defaults to $BLUB_GAME)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "codec/fil.h"
#include "data/gamefs.h"
#include "data/gap.h"

using namespace blub;

static uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static int failures = 0;
#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            std::printf("FAIL: " __VA_ARGS__);   \
            std::printf("\n");                   \
            failures++;                          \
        }                                        \
    } while (0)

static void test_fil(const std::string &fixtures, GameData &game, const ExeImage &exe) {
    std::ifstream in(fixtures + "/fil.txt");
    CHECK(in.good(), "missing fil.txt");
    std::string line;
    FilMovie movie;
    FilDecoder dec(exe);
    std::string name;
    int bad_video = 0, bad_audio = 0, frames = 0;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        if (line.rfind("movie ", 0) == 0) {
            std::string tag, gap;
            int id, off, attr, n;
            ls >> tag >> gap >> id >> off >> attr >> n;
            GapArchive arc;
            CHECK(arc.open(game.open_file(dos_components(gap)), gap), "open %s", gap.c_str());
            Bytes p = arc.payload(id);
            p.erase(p.begin(), p.begin() + off);
            const size_t plen = p.size();
            const bool ok = movie.parse(std::move(p), uint16_t(attr));
            CHECK(ok, "parse %s %d (payload %zu bytes, %zu frames parsed)", gap.c_str(), id, plen, movie.frames.size());
            std::fflush(stdout);
            CHECK(int(movie.frames.size()) == n, "%s %d: %zu frames, expected %d", gap.c_str(), id,
                  movie.frames.size(), n);
            dec.start(movie);
            name = gap + " " + std::to_string(id);
            continue;
        }
        size_t k;
        std::string vhex, ahex;
        ls >> k >> vhex >> ahex;
        const uint32_t vexp = std::stoul(vhex, nullptr, 16), aexp = std::stoul(ahex, nullptr, 16);
        frames++;
        if (k >= movie.frames.size()) continue;
        if (!dec.decode(movie, k)) {
            CHECK(false, "%s frame %zu: codec fault", name.c_str(), k);
            continue;
        }
        const uint32_t v = crc32(dec.pixels(), size_t(movie.width) * movie.height);
        if (v != vexp && bad_video++ < 5) std::printf("  %s frame %zu (codec %u): video crc %08x != %08x\n",
                                                    name.c_str(), k, movie.frames[k].codec, v, vexp);
        auto pcm = dec.audio(movie, k);
        const uint32_t a = pcm.empty() ? 0 : crc32(reinterpret_cast<const uint8_t *>(pcm.data()), pcm.size() * 2);
        if (a != aexp && bad_audio++ < 5) std::printf("  %s frame %zu: audio crc %08x != %08x\n", name.c_str(), k, a, aexp);
    }
    std::printf("FIL: %d frames, %d video mismatches, %d audio mismatches\n", frames, bad_video, bad_audio);
    failures += bad_video + bad_audio;
}

void test_recomp(GameData &game, const ExeImage &exe);   // test_recomp.cpp
extern int recomp_failures;

int main(int argc, char **argv) {
    std::string fixtures = argc > 1 ? argv[1] : "tests/fixtures";
    std::string game = argc > 2 ? argv[2] : (std::getenv("BLUB_GAME") ? std::getenv("BLUB_GAME") : "");
    if (game.empty()) {
        std::printf("set BLUB_GAME to the game directory\n");
        return 2;
    }
    ExeImage exe;
    std::string err;
    GameData data;
    if (!data.open(game, &err)) {
        std::printf("no game data in %s %s\n", game.c_str(), err.c_str());
        return 2;
    }
    if (!exe.load_bytes(data.read_all("\\DID.EXE"), &err)) {
        std::printf("cannot load DID.EXE: %s\n", err.c_str());
        return 2;
    }
    test_fil(fixtures, data, exe);
    test_recomp(data, exe);
    failures += recomp_failures;
    std::printf(failures ? "FAILED (%d)\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
