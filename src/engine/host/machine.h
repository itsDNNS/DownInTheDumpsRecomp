#pragma once
// The host machine the recompiled DID.EXE runs on: replaces DOS, DOS/4GW (DPMI), the VESA/VGA card,
// mouse, keyboard, MSCDEX, the PC timer and the HMI SOS sound library with SDL2-based implementations.
//
// Guest address space (Arena, 64 MB):
//   0x00010000  code object of DID.EXE        0x00040000  data object (up to 0x5D130)
//   0x00060000  "conventional" DOS memory (DPMI 0100h), up to 0xF0000
//   0x00100000  ... guest stack, grows down from Arena::STACK_TOP (0xF00000)
//   0x00F10000  VESA bank window (64 KB, reached through the selector of segment A000h)
//   0x00F20000  host area: argv, errno, SOS sample slots, scratch
//   0x01000000  DPMI memory blocks (0501h)
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "codec/image.h"
#include "data/exe_image.h"
#include "data/gamefs.h"
#include "recomp/runtime.h"

struct SDL_Window;
union SDL_Event;
typedef struct _SDL_GameController SDL_GameController;

namespace blub {

class Display;
struct OverlayBox;

struct HostConfig {
    std::string game;                     // game data: folder with the disc contents and/or ISO images
    std::string save_dir;                 // writable directory (DID.CFG, save games)
    std::vector<std::string> args;        // command line of DID.EXE, e.g. {"ITOON\\ITOON.EXP"}
    int scale = 2;
    bool fullscreen = false;
    bool smooth = false;
    bool xbrz = false;                    // xBRZ upscaling
    bool nosound = false;
    bool dualpage = true;                 // /DUALPAGE: page flipping instead of drawing on screen
    bool vsync = true;
    int volume = 100;                     // master volume in percent
    bool esc_skips = true;                // Esc acts like the space bar (skips videos)
    bool gamepad = true;                  // game controllers move the mouse pointer
    bool trace = false;                   // log DOS/DPMI/SOS calls
    // testing
    std::string shot_dir;                 // save the screen as BMP every shot_interval seconds
    double shot_interval = 1.0;
    bool shot_presented = false;          // ... as shown in the window (upscaled, with the overlay)
    double quit_after = 0;                // seconds; 0 = run until the game ends
    std::string wav;                      // record the mixed sound to this WAV file
    std::string script;                   // scripted input: lines "<seconds> click|rclick|move|key <args>"
    // recordings (host/replay.cpp): the game runs on a virtual clock and is deterministic
    std::string record;                   // write the input of this session to this file
    std::string replay;                   // play back a recording as fast as possible
    std::string checkpoints;              // replay: write a hash of the screen every second of game time
    bool headless = false;                // no window, no sound device (replays)
    uint32_t explore = 0;                 // play by itself with this seed (host/explore.cpp)
    std::string version;                  // of blub, for the header of recordings
};

struct GuestExit {
    int code;
};

namespace layout {
// DOS memory like a real PC (about 570 KB free): MemAlloc first asks for 640 KB and only records
// the block it gets on the retry path
constexpr uint32_t DOS_BASE = 0x00060000, DOS_END = 0x000F0000;
constexpr uint32_t VGA_WINDOW = 0x00F10000, VGA_WINDOW_SIZE = 0x10000;
constexpr uint32_t HOST_AREA = 0x00F20000, HOST_AREA_END = 0x00FF0000;
constexpr uint32_t HEAP_BASE = 0x01000000;
constexpr uint32_t RET_MAGIC = 0xFEEDF00Du;   // return address of host -> guest calls
}  // namespace layout

// open DOS file
struct DosFile {
    std::FILE *fp = nullptr;              // file in the save directory
    std::unique_ptr<ReadFile> ro;         // or: file on a game disc (read-only)
    std::string host_path;
    bool writable = false;
};

class Machine {
public:
    explicit Machine(const HostConfig &cfg);
    ~Machine();
    bool init(std::string *error);          // opens cfg.game and loads DID.EXE from it
    int run();                               // runs main() of the game, returns the exit code

    Arena m;
    Cpu c;
    HostConfig cfg;
    ExeImage exe;

    // --- host <-> guest
    // call guest code like an interrupt (registers are preserved); stack_arg: one cdecl argument
    void call_guest(uint32_t addr, bool with_arg = false, uint32_t stack_arg = 0);
    std::string gstr(uint32_t a, size_t max = 4096) const;
    void put_str(uint32_t a, const std::string &s);
    uint32_t host_alloc(uint32_t size);                 // permanent allocation in the host area
    void trace(const char *fmt, ...);

    // --- recordings (host/replay.cpp)
    std::FILE *rec = nullptr;                // Record: the input log
    int rec_x = -1, rec_y = -1, rec_buttons = 0;
    void record_input();                     // after pump_events: what changed of mouse and buttons
    void push_key(uint16_t key);             // into the BIOS key buffer (and the input log)
    std::FILE *checks = nullptr;             // Replay: screen hashes
    double next_check = 1, last_real_present = -1;
    void checkpoint();
    bool start_recordings(std::string *error);
    void stop_recordings();

    // --- automatic exploration (host/explore.cpp)
    struct ExploreAction { double t; int kind, x, y; };
    std::deque<ExploreAction> explore_queue;
    uint32_t explore_rng = 1;
    double title_shown = -1;                 // the game showed a text (hint: SubTitle)
    double probe_from = 0;
    int probe_tries = 0;
    double explore_next = 0, explore_quiet = -1;
    double explore_save_at = 300, explore_load_at = -1;   // the first save after 5 minutes
    double explore_slot_click = 0;
    int explore_slot_tries = 0;
    std::map<int, std::set<int64_t>> explored;   // per scene: pointer and hotspot of the clicks
    void explore_step();
    bool explore_choose(bool bar, std::pair<int, int> &out);
    void explore_at(double t, int kind, int x = 0, int y = 0);
    void explore_click(int x, int y, double t);
    void explore_save(double t);
    void explore_load(double t);
    uint32_t explore_random(uint32_t n);

    // --- error reports: the error that stopped the game, and what it did last (files it opened)
    std::string fatal;
    std::deque<std::string> recent;
    void note(const char *fmt, ...);

    // --- poll: timers, sound, input, screen (called from BLUB_POLL and from host functions)
    void poll();
    void idle_wait();                        // WaitTimer hook: sleep instead of spinning
    void check_quit();

    // --- DOS (host/dos.cpp)
    GameData game;
    std::string save_path(const std::vector<std::string> &comps, bool *exists = nullptr);
    bool dos_exists(const std::string &path, bool *is_dir = nullptr);
    int dos_open(const std::string &path, int mode, bool create, bool truncate);
    int dos_close(int h);
    int64_t dos_read(int h, uint32_t buf, uint32_t n);
    int64_t dos_write(int h, uint32_t buf, uint32_t n);
    int64_t dos_seek(int h, int64_t off, int whence);
    int64_t dos_length(int h);
    int dos_unlink(const std::string &path);
    int dos_rename(const std::string &from, const std::string &to);
    int dos_mkdir(const std::string &path);
    int dos_chdir(const std::string &path);
    int dos_findfirst(const std::string &pattern, uint32_t find_t);
    int dos_findnext(uint32_t find_t);
    void int21(Cpu &r);
    std::string cwd = "\\";
    std::map<int, DosFile> files;
    struct FindState { std::vector<std::pair<std::string, uint64_t>> hits; size_t next = 0; };
    std::map<uint32_t, FindState> finds;
    uint32_t errno_addr = 0;

    // --- DPMI / selectors (host/dpmi.cpp)
    uint32_t selector_base(uint16_t sel) const;
    void int31(Cpu &r);
    void real_mode_int(int vector, uint32_t regs_addr);
    std::map<uint16_t, uint32_t> selectors;
    uint16_t next_selector = 0x100;
    uint32_t dos_brk = layout::DOS_BASE;
    struct Block { uint32_t addr, size; };
    std::map<uint32_t, Block> blocks;        // DPMI 0501h handles
    uint32_t heap_brk = layout::HEAP_BASE;
    uint32_t next_handle = 1;

    // --- video (host/video.cpp)
    void int10(Cpu &r);
    bool vesa(uint32_t &eax, uint32_t &ebx, uint32_t &ecx, uint32_t &edx, uint32_t es_di_linear);
    void set_bank(uint32_t bank);
    bool flush_window();
    bool shows_bank() const;
    void present(bool force);
    // the picture also changes between the game's frames (it draws its mouse pointer while it waits for
    // the next one): show changes at the refresh rate of the display, from points where the picture is
    // complete (the frame wait, mouse requests, the vertical retrace)
    void present_if_due();
    bool screen_changed();
    void hotspot_boxes(std::vector<OverlayBox> &out);     // host/hotspots.cpp
    bool hotspots = false;                   // F2: show the clickable areas
    uint32_t port_in(uint16_t port, int size);
    void port_out(uint16_t port, uint32_t value, int size);
    std::unique_ptr<Display> display;
    std::vector<uint8_t> vram;
    uint32_t bank = 0, display_start = 0;
    bool graphics = false;
    Palette pal{};
    std::array<uint8_t, 768> dac{};
    int dac_write = 0, dac_read = 0;
    bool last_retrace = false;
    double last_present = -1;                // now() of the last picture
    bool dirty = true;                       // the visible picture changed since then (palette, a bank)

    // --- input (host/input.cpp)
    void int16(Cpu &r);
    void int33(Cpu &r);
    void pump_events();
    // game controllers (host/gamepad.cpp): they drive the mouse pointer
    void gamepad_event(const SDL_Event &e);
    void gamepad_move();
    void hotspot_targets(std::vector<std::pair<int, int>> &out);   // host/hotspots.cpp
    void print_hotspots();
    void jump_to_hotspot(int dir);
    std::map<int32_t, SDL_GameController *> pads;
    double pad_x = 0, pad_y = 0, pad_time = -1;   // pointer in screen pixels (with fractions)
    int pad_mouse_x = -1, pad_mouse_y = -1;       // mouse_x/y as the controller left them
    int touch_finger = -1;
    float touch_x = 0, touch_y = 0;
    std::deque<uint16_t> keys;               // BIOS keys: scan code << 8 | ascii
    int mouse_x = 0, mouse_y = 0;            // in mouse driver coordinates
    int mouse_buttons = 0;
    int mouse_polls = 0;                     // position requests since the last WaitTimer
    int mx_min = 0, mx_max = 639, my_min = 0, my_max = 479;
    int mouse_scale = 4;                     // the game works with pixel * 4
    bool quit_requested = false;
    struct ScriptEvent { double t; std::string what; int a = 0, b = 0; };
    std::deque<ScriptEvent> script;
    void run_script();
    void save_shot(const std::string &path);
    double last_shot = -1;
    int shot_no = 0;

    // --- time / SOS timer events (host/sound.cpp)
    struct TimerEvent { uint32_t handle, fn; double rate, next; };
    std::vector<TimerEvent> timers;
    uint32_t next_timer = 1;
    double now() const;                      // seconds since start (the virtual clock for recordings)
    double real_now() const;
    // Real: the wall clock. Record/Replay: a virtual clock that only advances where the game waits or
    // polls (sleep_for, tick), so that the same input gives the same game - recorded in real time,
    // replayed as fast as possible
    enum class Clock { Real, Record, Replay };
    Clock clock = Clock::Real;
    double vclock = 0;
    void sleep_for(double seconds);
    void tick(double seconds);
    double last_events = -1;                 // the last time pump_events ran
    void run_timers();
    bool in_callback = false;

    // --- SOS digital audio (host/sound.cpp)
    bool sos_call(uint32_t addr);          // false: not an SOS function
    bool audio_init();
    void mix_audio();
    bool retire(int slot);
    uint32_t slots_addr = 0;                 // 32 sample slots (0xF0 bytes each) in the host area
    static constexpr int SLOTS = 32;
    std::array<double, SLOTS> slot_pos{};    // fractional read position (in samples)
    double mixed_until = -1;                 // the mixer's clock
    uint32_t audio_dev = 0;
    int out_rate = 44100;
    int master_volume = 100;
    std::FILE *wav_fp = nullptr;
    uint32_t wav_bytes = 0;
    void close_wav();

    // --- C runtime (host/crt.cpp)
    bool crt_call(uint32_t addr);          // false: not a C runtime function
    std::string format(const std::string &fmt, uint32_t args);

private:
    uint32_t host_brk_ = layout::HOST_AREA;
    double t0_ = 0;
    uint64_t perf_freq_ = 1;
};

Machine &machine();

}  // namespace blub
