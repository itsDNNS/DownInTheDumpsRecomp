#include "gfx/display.h"

#include <SDL.h>
#include <vector>

namespace blub {

Display::~Display() {
    if (tex_) SDL_DestroyTexture(tex_);
    if (ren_) SDL_DestroyRenderer(ren_);
    if (win_) SDL_DestroyWindow(win_);
}

bool Display::init(const DisplayOptions &o) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, o.smooth ? "linear" : "nearest");
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (o.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    fullscreen_ = o.fullscreen;
    win_ = SDL_CreateWindow(o.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            SCREEN_W * o.scale, SCREEN_H * o.scale, flags);
    if (!win_) return false;
    ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | (o.vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, 0);
    if (!ren_) return false;
    SDL_RenderSetLogicalSize(ren_, SCREEN_W, SCREEN_H);     // keeps 4:3 with letterboxing
    tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
    return tex_ != nullptr;
}

void Display::present(const uint8_t *px, const Palette &pal) {
    uint32_t lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = 0xFF000000u | (uint32_t(pal[3 * i]) << 16) | (uint32_t(pal[3 * i + 1]) << 8) | pal[3 * i + 2];
    void *dst;
    int pitch;
    if (SDL_LockTexture(tex_, nullptr, &dst, &pitch) == 0) {
        for (int y = 0; y < SCREEN_H; y++) {
            uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(dst) + size_t(y) * pitch);
            const uint8_t *src = px + size_t(y) * SCREEN_W;
            for (int x = 0; x < SCREEN_W; x++) row[x] = lut[src[x]];
        }
        SDL_UnlockTexture(tex_);
    }
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
    SDL_RenderClear(ren_);
    SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
    SDL_RenderPresent(ren_);
}

void Display::toggle_fullscreen() {
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(win_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void Display::to_screen(int wx, int wy, int &sx, int &sy) const {
    // with a logical size SDL already delivers mouse events in logical coordinates
    sx = wx;
    sy = wy;
}

void Display::warp(int sx, int sy) {
    if (!(SDL_GetWindowFlags(win_) & SDL_WINDOW_INPUT_FOCUS)) return;
    int wx, wy;
    SDL_RenderLogicalToWindow(ren_, float(sx), float(sy), &wx, &wy);
    SDL_WarpMouseInWindow(win_, wx, wy);
}

} // namespace blub
