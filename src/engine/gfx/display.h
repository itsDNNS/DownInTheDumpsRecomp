#pragma once
// Presents the engine's 8-bit 640x480 frame buffer (VGA mode of the original) in a modern window:
// integer or smooth scaling, window/fullscreen, aspect ratio kept.
#include <cstdint>
#include <string>

#include "codec/image.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace blub {

constexpr int SCREEN_W = 640;
constexpr int SCREEN_H = 480;

struct DisplayOptions {
    int scale = 2;              // initial window scale
    bool fullscreen = false;
    bool smooth = false;        // linear filtering instead of nearest neighbour
    bool vsync = true;
    std::string title = "Down in the Dumps";
};

class Display {
public:
    ~Display();
    bool init(const DisplayOptions &o);
    // 640x480 palette indices + 8-bit RGB palette
    void present(const uint8_t *pixels, const Palette &pal);
    void toggle_fullscreen();
    // convert window coordinates into frame buffer coordinates
    void to_screen(int wx, int wy, int &sx, int &sy) const;
    // move the host mouse pointer to frame buffer coordinates
    void warp(int sx, int sy);

private:
    SDL_Window *win_ = nullptr;
    SDL_Renderer *ren_ = nullptr;
    SDL_Texture *tex_ = nullptr;
    bool fullscreen_ = false;
};

} // namespace blub
