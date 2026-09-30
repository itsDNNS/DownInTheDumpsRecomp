// blub_view: development tool, shows the images of a GAP archive of the game.
// Usage: blub_view --gap <file.GAP> [--id <resource id>] [--screenshot <file.bmp>]
// Arrow keys: previous/next image, F11: fullscreen.
#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "codec/image.h"
#include "data/gap.h"
#include "gfx/display.h"

using namespace blub;

int main(int argc, char **argv) {
    std::string gap;
    int id = 0;
    std::string shot;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--gap") && i + 1 < argc) gap = argv[++i];
        else if (!std::strcmp(argv[i], "--id") && i + 1 < argc) id = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
    }
    if (gap.empty()) {
        std::fprintf(stderr, "usage: blub_view --gap <file.GAP> [--id <resource id>] [--screenshot <file.bmp>]
");
        return 2;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    GapArchive arc;
    if (!arc.open(gap)) {
        std::fprintf(stderr, "cannot open %s\n", gap.c_str());
        return 1;
    }
    Display disp;
    if (!disp.init(DisplayOptions{})) {
        std::fprintf(stderr, "display: %s\n", SDL_GetError());
        return 1;
    }
    std::vector<uint8_t> screen(SCREEN_W * SCREEN_H, 0);
    Palette pal{};
    auto show = [&](int rid) {
        std::fill(screen.begin(), screen.end(), 0);
        Indexed img;
        ResHead hd = arc.head(rid);
        if (hd.type == RES_IMAGE && decode_pcx(arc.payload(rid), img, &pal)) {
            for (int y = 0; y < img.h && y + 60 < SCREEN_H; y++)
                std::memcpy(&screen[(y + 60) * SCREEN_W], &img.pix[y * img.w], std::min(img.w, SCREEN_W));
        }
        std::printf("id %d type %04x attr %04x %dx%d\n", rid, hd.type, hd.attr, hd.w, hd.h);
    };
    show(id);
    if (!shot.empty()) {        // render once into a BMP and quit (used by automated checks)
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(screen.data(), SCREEN_W, SCREEN_H, 8, SCREEN_W,
                                                            SDL_PIXELFORMAT_INDEX8);
        SDL_Color c[256];
        for (int i = 0; i < 256; i++) c[i] = SDL_Color{pal[3 * i], pal[3 * i + 1], pal[3 * i + 2], 255};
        SDL_SetPaletteColors(s->format->palette, c, 0, 256);
        SDL_SaveBMP(s, shot.c_str());
        SDL_FreeSurface(s);
        SDL_Quit();
        return 0;
    }
    bool run = true;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) run = false;
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) run = false;
                if (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))
                    disp.toggle_fullscreen();
                if (e.key.keysym.sym == SDLK_RIGHT || e.key.keysym.sym == SDLK_LEFT) {
                    int step = e.key.keysym.sym == SDLK_RIGHT ? 1 : -1;
                    do id += step; while (id >= 0 && id < arc.count() && arc.head(id).type != RES_IMAGE);
                    if (id >= 0 && id < arc.count()) show(id);
                }
            }
        }
        disp.present(screen.data(), pal);
    }
    SDL_Quit();
    return 0;
}
