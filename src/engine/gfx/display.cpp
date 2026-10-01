#include "gfx/display.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

#include "xbrz.h"

namespace blub {

// A few threads that run the parts of a job together with the calling thread (xBRZ slices).
class Workers {
public:
    explicit Workers(int n) {
        for (int i = 0; i < n; i++) threads_.emplace_back([this] { loop(); });
    }
    ~Workers() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : threads_) t.join();
    }
    int size() const { return int(threads_.size()) + 1; }
    // fn(0) .. fn(parts - 1); returns when all of them are done
    void run(int parts, const std::function<void(int)> &fn) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            job_ = &fn;
            parts_ = parts;
            next_ = 0;
            done_ = 0;
        }
        cv_.notify_all();
        work();
        std::unique_lock<std::mutex> lk(mu_);
        done_cv_.wait(lk, [this] { return done_ == parts_; });
        job_ = nullptr;
    }

private:
    void work() {
        for (;;) {
            int part;
            const std::function<void(int)> *job;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!job_ || next_ >= parts_) return;
                part = next_++;
                job = job_;
            }
            (*job)(part);
            std::lock_guard<std::mutex> lk(mu_);
            if (++done_ == parts_) done_cv_.notify_all();
        }
    }
    void loop() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || (job_ && next_ < parts_); });
                if (stop_) return;
            }
            work();
        }
    }
    std::vector<std::thread> threads_;
    std::mutex mu_;
    std::condition_variable cv_, done_cv_;
    const std::function<void(int)> *job_ = nullptr;
    int parts_ = 0, next_ = 0, done_ = 0;
    bool stop_ = false;
};

Display::Display() = default;

Display::~Display() {
    workers_.reset();
    if (big_) SDL_DestroyTexture(big_);
    if (tex_) SDL_DestroyTexture(tex_);
    if (ren_) SDL_DestroyRenderer(ren_);
    if (win_) SDL_DestroyWindow(win_);
}

bool Display::init(const DisplayOptions &o) {
    // xBRZ output is filtered linearly to the window size; the plain picture as configured
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, o.smooth || o.upscaler == Upscaler::XBRZ ? "linear" : "nearest");
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (o.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    fullscreen_ = o.fullscreen;
    upscaler_ = o.upscaler;
    win_ = SDL_CreateWindow(o.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            SCREEN_W * o.scale, SCREEN_H * o.scale, flags);
    if (!win_) return false;
    ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | (o.vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, 0);
    if (!ren_) return false;
    SDL_RenderSetLogicalSize(ren_, SCREEN_W, SCREEN_H);     // keeps 4:3 with letterboxing
    tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
    if (tex_ && !o.smooth) SDL_SetTextureScaleMode(tex_, SDL_ScaleModeNearest);
    if (upscaler_ == Upscaler::XBRZ) {
        const int n = int(std::thread::hardware_concurrency());
        workers_ = std::make_unique<Workers>(std::clamp(n - 1, 0, 7));
    }
    return tex_ != nullptr;
}

// the xBRZ scale that covers the picture area of the window (2..4; the rest is filtered linearly)
int Display::xbrz_factor() const {
    int w = SCREEN_W, h = SCREEN_H;
    SDL_GetRendererOutputSize(ren_, &w, &h);
    const int area_h = std::min(h, w * 3 / 4);
    return std::clamp(int(std::ceil(area_h / double(SCREEN_H) - 0.05)), 2, 4);
}

namespace {
// xBRZ works on tiles: the output of a pixel depends on the source pixels up to 2 away, so a tile
// is scaled again when anything changed within 2 pixels of it, and it is scaled from a cut-out
// with 4 pixels of context on every side - the result is the same as scaling the whole picture
constexpr int TILE_W = 64, TILE_H = 32;
constexpr int TILES_X = SCREEN_W / TILE_W, TILES_Y = SCREEN_H / TILE_H;
constexpr int REACH = 2, CONTEXT = 4;
}  // namespace

void Display::update_xbrz(const uint8_t *px, const Palette &pal) {
    const int f = xbrz_factor();
    if (f != factor_ || !big_) {
        if (big_) SDL_DestroyTexture(big_);
        big_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, SCREEN_W * f, SCREEN_H * f);
        if (!big_) {
            upscaler_ = Upscaler::None;
            return;
        }
        SDL_SetTextureScaleMode(big_, SDL_ScaleModeLinear);
        factor_ = f;
        scaled_.assign(size_t(SCREEN_W * f) * SCREEN_H * f, 0);
        have_last_ = false;
    }
    const int out_w = SCREEN_W * f;
    // the tiles that changed since the last frame (a new palette changes everything)
    bool dirty[TILES_Y][TILES_X] = {};
    int ndirty = 0, ty_first = 0, ty_last = TILES_Y - 1;
    bool full = !have_last_ || std::memcmp(last_pal_.data(), pal.data(), pal.size()) != 0;
    if (!full) {
        ty_first = TILES_Y;
        ty_last = -1;
        for (int y = 0; y < SCREEN_H; y++) {
            const uint8_t *was = &last_px_[size_t(y) * SCREEN_W], *now = px + size_t(y) * SCREEN_W;
            if (std::memcmp(was, now, SCREEN_W) == 0) continue;
            const int t0 = std::max(0, y - REACH) / TILE_H, t1 = std::min(SCREEN_H - 1, y + REACH) / TILE_H;
            for (int tx = 0; tx < TILES_X; tx++) {
                const int x0 = std::max(0, tx * TILE_W - REACH), x1 = std::min(SCREEN_W, (tx + 1) * TILE_W + REACH);
                if (std::memcmp(was + x0, now + x0, size_t(x1 - x0)) == 0) continue;
                for (int ty = t0; ty <= t1; ty++)
                    if (!dirty[ty][tx]) {
                        dirty[ty][tx] = true;
                        ndirty++;
                    }
                ty_first = std::min(ty_first, t0);
                ty_last = std::max(ty_last, t1);
            }
        }
        if (ndirty == 0) return;                     // nothing changed: the texture is up to date
        full = ndirty > TILES_X * TILES_Y / 2;       // most of the picture: whole rows are cheaper
    }
    last_px_.assign(px, px + size_t(SCREEN_W) * SCREEN_H);
    last_pal_ = pal;
    have_last_ = true;
    uint32_t lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = 0xFF000000u | (uint32_t(pal[3 * i]) << 16) | (uint32_t(pal[3 * i + 1]) << 8) | pal[3 * i + 2];

    if (full) {
        // whole rows, in slices
        rgb_.resize(size_t(SCREEN_W) * SCREEN_H);
        for (size_t i = 0; i < rgb_.size(); i++) rgb_[i] = lut[px[i]];
        const int a = ty_first * TILE_H, b = (ty_last + 1) * TILE_H;
        const int parts = std::clamp((b - a) / 16, 1, workers_ ? workers_->size() * 2 : 1);
        auto slice = [&](int k) {
            const int s0 = a + (b - a) * k / parts, s1 = a + (b - a) * (k + 1) / parts;
            xbrz::scale(size_t(f), rgb_.data(), scaled_.data(), SCREEN_W, SCREEN_H, xbrz::ColorFormat::rgb,
                        xbrz::ScalerCfg(), s0, s1);
            for (size_t i = size_t(s0) * f * out_w; i < size_t(s1) * f * out_w; i++) scaled_[i] |= 0xFF000000u;
        };
        if (workers_) workers_->run(parts, slice);
        else for (int k = 0; k < parts; k++) slice(k);
        const SDL_Rect r{0, a * f, out_w, (b - a) * f};
        SDL_UpdateTexture(big_, &r, &scaled_[size_t(r.y) * out_w], out_w * 4);
        return;
    }

    // the changed tiles, each from its own cut-out
    int list[TILES_X * TILES_Y], n = 0;
    for (int ty = 0; ty < TILES_Y; ty++)
        for (int tx = 0; tx < TILES_X; tx++)
            if (dirty[ty][tx]) list[n++] = ty * TILES_X + tx;
    auto tile = [&](int k) {
        const int tx = list[k] % TILES_X, ty = list[k] / TILES_X;
        const int sx0 = std::max(0, tx * TILE_W - CONTEXT), sx1 = std::min(SCREEN_W, (tx + 1) * TILE_W + CONTEXT);
        const int sy0 = std::max(0, ty * TILE_H - CONTEXT), sy1 = std::min(SCREEN_H, (ty + 1) * TILE_H + CONTEXT);
        const int sw = sx1 - sx0, sh = sy1 - sy0;
        thread_local std::vector<uint32_t> src, dst;
        src.resize(size_t(sw) * sh);
        dst.resize(size_t(sw) * f * sh * f);
        for (int y = 0; y < sh; y++) {
            const uint8_t *s = px + size_t(sy0 + y) * SCREEN_W + sx0;
            uint32_t *d = &src[size_t(y) * sw];
            for (int x = 0; x < sw; x++) d[x] = lut[s[x]];
        }
        const int rx = tx * TILE_W - sx0, ry = ty * TILE_H - sy0;   // the tile inside the cut-out
        xbrz::scale(size_t(f), src.data(), dst.data(), sw, sh, xbrz::ColorFormat::rgb, xbrz::ScalerCfg(), ry, ry + TILE_H);
        for (int y = 0; y < TILE_H * f; y++) {
            const uint32_t *s = &dst[size_t(ry * f + y) * sw * f + size_t(rx) * f];
            uint32_t *d = &scaled_[size_t(ty * TILE_H * f + y) * out_w + size_t(tx) * TILE_W * f];
            for (int x = 0; x < TILE_W * f; x++) d[x] = s[x] | 0xFF000000u;
        }
    };
    if (workers_ && n > 1) workers_->run(n, tile);
    else for (int k = 0; k < n; k++) tile(k);
    // upload per row of tiles: the span from its first to its last changed tile
    for (int ty = 0; ty < TILES_Y; ty++) {
        int t0 = 0, t1 = TILES_X;
        while (t0 < TILES_X && !dirty[ty][t0]) t0++;
        if (t0 == TILES_X) continue;
        while (!dirty[ty][t1 - 1]) t1--;
        const SDL_Rect r{t0 * TILE_W * f, ty * TILE_H * f, (t1 - t0) * TILE_W * f, TILE_H * f};
        SDL_UpdateTexture(big_, &r, &scaled_[size_t(r.y) * out_w + r.x], out_w * 4);
    }
}

void Display::present(const uint8_t *px, const Palette &pal, const std::vector<OverlayBox> &overlay) {
    SDL_Texture *show = tex_;
    if (upscaler_ == Upscaler::XBRZ) {
        update_xbrz(px, pal);
        if (upscaler_ == Upscaler::XBRZ) show = big_;
    }
    if (show == tex_) {
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
    }
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
    SDL_RenderClear(ren_);
    SDL_RenderCopy(ren_, show, nullptr, nullptr);
    if (!overlay.empty()) {
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        for (const OverlayBox &b : overlay) {
            SDL_SetRenderDrawColor(ren_, Uint8(b.rgba >> 24), Uint8(b.rgba >> 16), Uint8(b.rgba >> 8), Uint8(b.rgba));
            const SDL_Rect r{b.x, b.y, b.w, b.h};
            if (b.filled) SDL_RenderFillRect(ren_, &r);
            else SDL_RenderDrawRect(ren_, &r);
        }
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
    }
    if (!capture_.empty()) save_capture();
    SDL_RenderPresent(ren_);
}

void Display::save_capture() {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren_, &w, &h);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s) {
        // read the whole output, not just the logical viewport
        SDL_RenderSetLogicalSize(ren_, 0, 0);
        if (SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
            SDL_SaveBMP(s, capture_.c_str());
        SDL_RenderSetLogicalSize(ren_, SCREEN_W, SCREEN_H);
        SDL_FreeSurface(s);
    }
    capture_.clear();
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
