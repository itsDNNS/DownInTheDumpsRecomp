#pragma once
// Presents the engine's 8-bit 640x480 frame buffer (VGA mode of the original) in a modern window:
// integer or smooth scaling or the xBRZ upscaler, window/fullscreen, aspect ratio kept, plus an
// overlay of boxes drawn over the picture (hotspot display).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "codec/image.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace blub {

constexpr int SCREEN_W = 640;
constexpr int SCREEN_H = 480;

enum class Upscaler { None, XBRZ };

struct DisplayOptions {
    int scale = 2;              // initial window scale
    bool fullscreen = false;
    bool smooth = false;        // linear filtering instead of nearest neighbour
    Upscaler upscaler = Upscaler::None;
    bool vsync = true;
    std::string title = "Down in the Dumps";
};

// a rectangle drawn over the picture, in frame buffer coordinates
struct OverlayBox {
    int x, y, w, h;
    uint32_t rgba;              // 0xRRGGBBAA
    bool filled;
};

class Workers;

class Display {
public:
    Display();
    ~Display();
    bool init(const DisplayOptions &o);
    // 640x480 palette indices + 8-bit RGB palette, and boxes to draw over the picture
    void present(const uint8_t *pixels, const Palette &pal, const std::vector<OverlayBox> &overlay = {});
    void toggle_fullscreen();
    // convert window coordinates into frame buffer coordinates
    void to_screen(int wx, int wy, int &sx, int &sy) const;
    // move the host mouse pointer to frame buffer coordinates
    void warp(int sx, int sy);
    // save the next presented picture as it appears in the window (upscaled, with the overlay)
    void capture_next(const std::string &bmp_path) { capture_ = bmp_path; }

private:
    int xbrz_factor() const;
    void update_xbrz(const uint8_t *px, const Palette &pal);
    void save_capture();

    SDL_Window *win_ = nullptr;
    SDL_Renderer *ren_ = nullptr;
    SDL_Texture *tex_ = nullptr;
    bool fullscreen_ = false;
    Upscaler upscaler_ = Upscaler::None;
    // xBRZ: the 640x480 picture as RGB, the upscaled one, the texture it goes to, and the last
    // frame (only the rows that changed since are scaled again)
    SDL_Texture *big_ = nullptr;
    int factor_ = 0;
    std::vector<uint32_t> rgb_, scaled_;
    std::vector<uint8_t> last_px_;
    Palette last_pal_{};
    bool have_last_ = false;
    std::unique_ptr<Workers> workers_;
    std::string capture_;
};

} // namespace blub
