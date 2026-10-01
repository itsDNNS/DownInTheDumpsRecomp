#include "host/machine.h"

#include <SDL.h>

#include <algorithm>
#include <cstdarg>
#include <stdexcept>

#include "gfx/display.h"
#include "data/exe_symbols.h"
#include "util/i18n.h"
#include "util/sha1.h"
#include "recomp/hle_addrs.h"

namespace blub {

Machine *g_machine = nullptr;             // global (not static) so that debuggers find it

namespace {
constexpr uint16_t SEL_CODE = 0x0F, SEL_DATA = 0x17;
}  // namespace

Machine &machine() { return *g_machine; }

Machine::Machine(const HostConfig &config) : cfg(config) { g_machine = this; }

Machine::~Machine() {
    stop_recordings();
    close_wav();
    for (auto &f : files)
        if (f.second.fp) std::fclose(f.second.fp);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    for (auto &p : pads) SDL_GameControllerClose(p.second);
    display.reset();
    SDL_Quit();
    g_machine = nullptr;
}

void Machine::trace(const char *fmt, ...) {
    if (!cfg.trace) return;
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "%8.3f ", perf_freq_ > 1 ? now() : 0.0);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

void Machine::note(const char *fmt, ...) {
    char text[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    char line[340];
    std::snprintf(line, sizeof line, "%8.1f s  %s", perf_freq_ > 1 ? now() : 0.0, text);
    recent.emplace_back(line);
    if (recent.size() > 30) recent.pop_front();
}

bool Machine::init(std::string *error) {
    std::string problems;
    if (!game.open(cfg.game, &problems)) {
        if (error) *error = tr("keine Spieldaten in ", "no game data in ") + cfg.game + (problems.empty() ? "" : " (" + problems + ")");
        return false;
    }
    for (auto &d : game.discs()) std::printf("disc: %s\n", d->description().c_str());
    Bytes exe_file = game.read_all("\\DID.EXE");
    if (exe_file.empty()) {
        if (error) *error = tr("DID.EXE fehlt in ", "DID.EXE is missing in ") + cfg.game;
        return false;
    }
    // the recompiled code belongs to exactly this build of DID.EXE
    if (exe_file.size() != exesym::EXE_SIZE || sha1_hex(exe_file.data(), exe_file.size()) != exesym::EXE_SHA1) {
        if (error)
            *error = std::string(tr("diese Fassung von DID.EXE wird nicht unterstützt (SHA-1 ",
                                    "this build of DID.EXE is not supported (SHA-1 ")) +
                     sha1_hex(exe_file.data(), exe_file.size()) + ")";
        return false;
    }
    if (!exe.load_bytes(exe_file, error)) return false;
    m.write(exe.code_base, exe.code.data(), exe.code.size());
    m.write(exe.data_base, exe.data.data(), exe.data.size());

    if (cfg.headless) {                      // tests: no window on the screen, no sound device
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS |
                 (cfg.gamepad ? SDL_INIT_GAMECONTROLLER : 0)) != 0) {
        if (error) *error = SDL_GetError();
        return false;
    }
    perf_freq_ = SDL_GetPerformanceFrequency();
    t0_ = double(SDL_GetPerformanceCounter()) / double(perf_freq_);

    DisplayOptions o;
    o.scale = cfg.scale;
    o.fullscreen = cfg.fullscreen;
    o.smooth = cfg.smooth;
    o.upscaler = cfg.xbrz ? Upscaler::XBRZ : Upscaler::None;
    o.vsync = cfg.vsync;
    master_volume = cfg.volume;
    display = std::make_unique<Display>();
    if (!display->init(o)) {
        if (error) *error = std::string("cannot open the window: ") + SDL_GetError();
        return false;
    }
    SDL_ShowCursor(SDL_DISABLE);      // the game draws its own mouse pointer
    vram.assign(0x200000, 0);
    // a replay runs much faster than real time: its sound is mixed (the game depends on it), not played
    if (!cfg.nosound && cfg.replay.empty() && !cfg.explore && !audio_init()) std::fprintf(stderr, "sound disabled: %s\n", SDL_GetError());

    // flat selectors of DOS/4GW
    selectors[SEL_CODE] = 0;
    selectors[SEL_DATA] = 0;
    c.load_seg(0, SEL_DATA);
    c.load_seg(1, SEL_CODE);
    c.load_seg(2, SEL_DATA);
    c.load_seg(3, SEL_DATA);
    c.load_seg(4, SEL_DATA);
    c.load_seg(5, SEL_DATA);

    errno_addr = host_alloc(4);
    slots_addr = host_alloc(SLOTS * 0xF0);
    return start_recordings(error);
}

uint32_t Machine::host_alloc(uint32_t size) {
    uint32_t a = host_brk_;
    host_brk_ = (host_brk_ + size + 15) & ~15u;
    if (host_brk_ > layout::HOST_AREA_END) throw std::runtime_error("host area exhausted");
    return a;
}

std::string Machine::gstr(uint32_t a, size_t max) const {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = char(m.r8(a + uint32_t(i)));
        if (!ch) break;
        s += ch;
    }
    return s;
}

void Machine::put_str(uint32_t a, const std::string &s) {
    m.write(a, s.c_str(), s.size() + 1);
}

double Machine::real_now() const {
    return double(SDL_GetPerformanceCounter()) / double(perf_freq_) - t0_;
}

double Machine::now() const { return clock == Clock::Real ? real_now() : vclock; }

// a wait of the game: really, or on the virtual clock (when recording, it never runs ahead of the real
// one: the player plays in real time)
void Machine::sleep_for(double seconds) {
    if (clock == Clock::Real) {
        if (seconds > 0.0005) SDL_Delay(uint32_t(seconds * 1000.0));
        return;
    }
    vclock += std::max(seconds, 1e-5);
    if (clock == Clock::Record) {
        const double ahead = vclock - real_now();
        if (ahead > 0.0005) SDL_Delay(uint32_t(ahead * 1000.0));
    }
}

// time the game spends without waiting: running code, polling a clock
void Machine::tick(double seconds) {
    if (clock != Clock::Real) vclock += seconds;
}

int Machine::run() {
    // argv of DID.EXE: the program name followed by the configured arguments
    std::vector<std::string> args{"C:\\DID\\DID.EXE"};
    if (cfg.dualpage) args.push_back("/DUALPAGE");
    if (cfg.nosound) args.push_back("/NOSOUND");
    for (auto &a : cfg.args) args.push_back(a);
    uint32_t argv = host_alloc(uint32_t(4 * (args.size() + 1)));
    for (size_t i = 0; i < args.size(); i++) {
        uint32_t s = host_alloc(uint32_t(args[i].size() + 1));
        put_str(s, args[i]);
        m.w32(argv + uint32_t(4 * i), s);
    }
    m.w32(argv + uint32_t(4 * args.size()), 0);

    c.esp = Arena::STACK_TOP;
    c.budget = POLL_INTERVAL;
    c.eax = uint32_t(args.size());
    c.edx = argv;
    c.esp -= 4;
    m.w32(c.esp, layout::RET_MAGIC);
    try {
        dispatch_address(c, m, hle::k_main);
    } catch (const GuestExit &e) {
        return e.code;
    }
    return int(c.eax);
}

void Machine::call_guest(uint32_t addr, bool with_arg, uint32_t stack_arg) {
    if (!addr) return;
    Cpu saved = c;
    c.esp = (c.esp - 0x200) & ~3u;       // like an interrupt: leave the interrupted frame alone
    if (with_arg) {
        c.esp -= 4;
        m.w32(c.esp, stack_arg);
    }
    c.eax = stack_arg;
    c.esp -= 4;
    m.w32(c.esp, layout::RET_MAGIC);
    bool nested = in_callback;
    in_callback = true;
    dispatch_address(c, m, addr);
    in_callback = nested;
    int32_t budget = c.budget;
    c = saved;
    c.budget = budget;
}

// WaitFrameTime calls WaitTimer until SystemTimer reaches the time of the next frame
void Machine::idle_wait() {
    constexpr uint32_t NEXT_FRAME = 0x4097A, SYSTEM_TIMER = 0x5AC80;
    mouse_polls = 0;
    poll();
    present_if_due();                        // between two frames the pointer is redrawn here
    if (in_callback || m.r32(NEXT_FRAME) <= m.r32(SYSTEM_TIMER)) return;
    double due = now() + 0.002;
    for (auto &ev : timers)
        if (ev.fn && ev.rate > 0) due = std::min(due, ev.next);
    if (display && clock == Clock::Real) {   // wake up when the next picture may be shown
        const double next_picture = last_present + display->frame_interval();
        if (next_picture > now()) due = std::min(due, next_picture);
    }
    sleep_for(due - now());
}

void Machine::check_quit() {
    if (quit_requested) throw GuestExit{0};
}

void Machine::poll() {
    c.budget = POLL_INTERVAL;
    tick(2e-5);                              // the guest ran a while since the last poll
    if (in_callback) return;
    run_timers();
    mix_audio();
    double t = now();
    if ((checks || clock == Clock::Replay) && t >= next_check) checkpoint();
    if (last_events < 0 || t - last_events > 0.005) {
        last_events = t;
        pump_events();
        run_script();
        if (cfg.explore) explore_step();
        record_input();                      // recordings: what changed in this round
        if (cfg.quit_after > 0 && t > cfg.quit_after) quit_requested = true;
        check_quit();
    }
    // screens that are drawn without any wait still show up; the hotspot overlay follows the buttons
    // even while the picture stays the same; test screenshots are taken in present()
    if ((t - last_present > 0.05 && screen_changed()) || (hotspots && t - last_present > 0.1) ||
        (!cfg.shot_dir.empty() && t - last_shot >= cfg.shot_interval))
        present(false);
}

// ---------------------------------------------------------------- Cpu hooks

void Cpu::load_seg(int reg, uint16_t selector) {
    sel[reg] = selector;
    sb[reg] = machine().selector_base(selector);
}

// an error the game cannot continue after: logged and kept for the error report
static void fatal_error(const char *fmt, ...) {
    char text[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "fatal: %s\n", text);
    if (g_machine) g_machine->fatal = text;
}

void Cpu::divide_error() {
    fatal_error("division by zero");
    throw GuestExit{3};
}

// Garbage addresses, for example from a frame table read beyond its end, pointed into unmapped memory
// on DOS (a page fault); wrapped around into the 64 MB they would overwrite the game's own data.
void wild_access(uint32_t addr, size_t size) {
    fatal_error("memory access outside the game's memory (%u bytes at %08X)", unsigned(size), addr);
    throw GuestExit{5};
}

// The called code returned to another address than the one behind the call. The game does that on
// purpose: "push label / jmp routine", whose ret then goes to the label (TokenSay and TokenSaySprit
// with subtitles, DebugAct). Like the CPU, continue at the address returned to, until a return
// reaches the code behind the call.
void Cpu::bad_return(uint32_t call_site, uint32_t expected) {
    for (int n = 0; g_machine && n < 64 && last_ret != expected; n++)
        if (!dispatch_address(*this, g_machine->m, last_ret)) break;
    if (last_ret == expected) return;
    static int reported = 0;
    if (reported++ < 20)
        std::fprintf(stderr, "warning: call at %05X (%s) returned to %05X instead of %05X\n", call_site,
                     function_name(call_site), last_ret, expected);
}

void Cpu::bad_target(uint32_t addr) {
    fatal_error("jump/call to unknown address %08X", addr);
    throw GuestExit{4};
}

void Cpu::unsupported(uint32_t addr, const char *why) {
    fatal_error("%s (%05X) is not recompiled: %s", function_name(addr), addr, why);
    throw GuestExit{4};
}

void Cpu::halt() { throw GuestExit{0}; }

// ---------------------------------------------------------------- dispatch from generated code

void host_poll(Cpu &c, Arena &m) {
    if (g_machine) g_machine->poll();
    else c.budget = POLL_INTERVAL;       // unit tests run recompiled functions without a machine
}

void host_hook(Cpu &c, Arena &m, uint32_t addr) {
    Machine &mc = machine();
    switch (addr) {
    case hle::k_WaitTimer: mc.idle_wait(); break;
    case hle::k_SubTitle: mc.title_shown = mc.now(); break;
    case hle::k_hook_PlayFIL_WaitSound: {
        // loop "dec eax / cmp wSOSSamplePending, 1": wait for the sound callback in real time,
        // with a time-based instead of the original iteration-based timeout
        static double started = 0;
        if (c.eax == 0x2625A00u - 1) started = mc.now();
        if (m.r16(0x5AC8A) != 1) break;
        mc.poll();
        mc.present_if_due();                 // the video frame is complete
        if (m.r16(0x5AC8A) != 1) break;
        if (mc.now() - started > 1.0) c.eax = 1;     // give up after one second
        else mc.sleep_for(0.001);
        break;
    }
    default: break;
    }
}

void host_call(Cpu &c, Arena &m, uint32_t addr) {
    Machine &mc = machine();
    if (!mc.crt_call(addr) && !mc.sos_call(addr)) c.unsupported(addr, "host function missing");
}

void host_int(Cpu &c, Arena &m, int vector) {
    Machine &mc = machine();
    switch (vector) {
    case 0x10: mc.int10(c); break;
    case 0x16: mc.int16(c); break;
    case 0x1A: {                            // BIOS tick count (18.2 Hz)
        mc.tick(1e-5);
        uint32_t ticks = uint32_t(mc.now() * 18.2065);
        c.ecx = (c.ecx & 0xFFFF0000u) | (ticks >> 16);
        c.edx = (c.edx & 0xFFFF0000u) | (ticks & 0xFFFF);
        c.eax &= 0xFFFFFF00u;
        break;
    }
    case 0x21: mc.int21(c); break;
    case 0x2F:
        if ((c.eax & 0xFFFF) == 0x1500) {   // MSCDEX installation check: one CD-ROM drive, D:
            c.ebx = (c.ebx & 0xFFFF0000u) | 1;
            c.ecx = (c.ecx & 0xFFFF0000u) | 3;
        }
        break;
    case 0x31: mc.int31(c); break;
    case 0x33: mc.int33(c); break;
    default: mc.trace("int %02Xh ignored (ax=%04X)", vector, c.eax & 0xFFFF); break;
    }
}

uint32_t host_in(Cpu &c, uint16_t port, int size) { return machine().port_in(port, size); }

void host_out(Cpu &c, uint16_t port, uint32_t value, int size) { machine().port_out(port, value, size); }

}  // namespace blub
