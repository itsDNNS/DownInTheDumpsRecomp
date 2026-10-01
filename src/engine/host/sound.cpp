// Replacement of the HMI SOS 4 library: timer events and a software mixer on SDL audio.
// The sample slots live in guest memory (layout of _SOS_SAMPLE, 0xF0 bytes) because the game's
// callbacks (_FIL_CallBack, _WAC_CallBack, ...) receive and modify them. The mixing rules follow the
// original _hmiDigitalMixer: at the end of a sample loops are counted down, then pfnSampleProcessed
// may supply a new buffer; a finished or stopped sample calls pfnSampleDone on the next mix pass.
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "host/machine.h"
#include "recomp/hle_addrs.h"

namespace blub {

namespace {

// _SOS_SAMPLE
enum S : uint32_t { pSample = 0x00, pSampleCurrent = 0x04, wLength = 0x0C, wID = 0x1C, wFlags = 0x20,
                    hSample = 0x28, wVolume = 0x2C, wLoopCount = 0x30, wRate = 0x34, wBitsPerSample = 0x38,
                    wChannels = 0x3C, wPanPosition = 0x44, wTotalBytesProcessed = 0x58,
                    pfnSampleProcessed = 0x5C, pfnSampleDone = 0x60 };
constexpr uint32_t ACTIVE = 0x8000, FINISHED = 0x4000, DONE = 0x2000, SLOT_SIZE = 0xF0;
constexpr uint32_t LIB_START = 0x2FC2E;   // code of the (replaced) SOS library itself

// _SOS_DIGI_DRIVER: capabilities at 0x6C, hardware at 0x5C
void fill_caps(Arena &m, uint32_t drv) {
    const uint32_t caps = drv + 0x6C;
    std::memset(m.ptr(caps), 0, 0x6C);
    m.write(caps, "SDL2 digital audio", 19);
    m.w32(caps + 0x20, 0x0400);            // version
    m.w32(caps + 0x24, 16);                // bits per sample
    m.w32(caps + 0x28, 2);                 // channels
    m.w32(caps + 0x2C, 4000);
    m.w32(caps + 0x30, 44100);
    m.w32(caps + 0x64, 0xE000);            // device ID
    m.w32(drv + 0x5C, 0x220);              // port, IRQ, DMA (only printed)
    m.w32(drv + 0x60, 5);
    m.w32(drv + 0x64, 1);
}

// debugging: BLUB_AUDIO_DUMP=file writes every buffer when it starts playing ([u32 id][u32 len][data])
void dump_buffer(Arena &m, uint32_t s) {
    static std::FILE *f = [] { const char *p = std::getenv("BLUB_AUDIO_DUMP"); return p ? std::fopen(p, "wb") : nullptr; }();
    if (!f) return;
    uint32_t hdr[2] = {m.r32(s + wID), m.r32(s + wLength)};
    std::fwrite(hdr, 4, 2, f);
    std::fwrite(m.ptr(m.r32(s + pSample)), 1, hdr[1], f);
    std::fflush(f);
}

}  // namespace

bool Machine::audio_init() {
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!audio_dev) return false;
    out_rate = have.freq;
    SDL_PauseAudioDevice(audio_dev, 0);
    return true;
}

void Machine::run_timers() {
    const double t = now();
    for (size_t i = 0; i < timers.size(); i++) {
        TimerEvent &ev = timers[i];
        if (ev.rate <= 0 || !ev.fn || ev.fn >= LIB_START) continue;
        int n = 0;
        while (ev.next <= t && n++ < 8) {
            uint32_t fn = ev.fn;
            ev.next += 1.0 / ev.rate;
            call_guest(fn);
            if (i >= timers.size()) return;
        }
        if (timers[i].next <= t) timers[i].next = t + 1.0 / timers[i].rate;   // don't try to catch up
    }
}

// a finished or stopped sample: pfnSampleDone, then the slot is free again
bool Machine::retire(int h) {
    const uint32_t s = slots_addr + uint32_t(h) * SLOT_SIZE;
    const uint32_t flags = m.r32(s + wFlags);
    if (!(flags & ACTIVE) || !(flags & FINISHED)) return false;
    m.w32(s + wFlags, 0);
    call_guest(m.r32(s + pfnSampleDone), true, s);
    m.w32(s + wFlags, (m.r32(s + wFlags) & ~ACTIVE) | DONE);
    return true;
}

// The mixer runs on the system clock, not on the sound device: the game's sample callbacks (FIL,
// speech and music streaming) and its waiting loops depend on the samples being consumed in real time,
// even when the device is slow or stalls (seen with Wine/PulseAudio). The device gets what it can take.
void Machine::mix_audio() {
    double &last = mixed_until;
    const double t = now();
    if (last < 0) last = t - 0.05;                        // start with 50 ms of sound
    if (t - last > 0.25) last = t - 0.25;                 // after a long pause (debugger, loading): no burst
    int frames = int((t - last) * out_rate);
    if (frames < 256) return;
    frames = std::min(frames, 8192);
    last += double(frames) / out_rate;
    std::vector<int32_t> acc(size_t(frames) * 2, 0);

    for (int h = 0; h < SLOTS; h++) {
        const uint32_t s = slots_addr + uint32_t(h) * SLOT_SIZE;
        if (!(m.r32(s + wFlags) & ACTIVE)) continue;
        if (retire(h)) continue;
        int f = 0;
        while (f < frames) {
            const uint32_t bits = m.r32(s + wBitsPerSample) == 8 ? 8 : 16;
            const uint32_t chans = m.r32(s + wChannels) == 2 ? 2 : 1;
            const uint32_t step = (bits / 8) * chans;
            uint32_t cur = m.r32(s + pSampleCurrent);
            const uint32_t end = m.r32(s + pSample) + m.r32(s + wLength);
            if (cur >= end) {                            // end of the buffer
                const int32_t loops = int32_t(m.r32(s + wLoopCount));
                if (loops != -1) {
                    if (loops == 0) {
                        m.w32(s + wFlags, m.r32(s + wFlags) | FINISHED);
                        const uint32_t cb = m.r32(s + pfnSampleProcessed);
                        if (!cb) break;
                        m.w32(s + pSample, 0);
                        call_guest(cb, true, s);
                        if (m.r32(s + pSample) == 0) break;
                        m.w32(s + wFlags, m.r32(s + wFlags) & ~FINISHED);
                        dump_buffer(m, s);
                    } else {
                        m.w32(s + wLoopCount, uint32_t(loops - 1));
                    }
                }
                m.w32(s + pSampleCurrent, m.r32(s + pSample));
                if (m.r32(s + wLength) == 0) {
                    m.w32(s + wFlags, m.r32(s + wFlags) | FINISHED);
                    break;
                }
                continue;
            }
            // volume: low word left, high word right (0..7FFFh); optional pan position
            const uint32_t vol = m.r32(s + wVolume);
            int vl = int(vol & 0x7FFF), vr = int((vol >> 16) & 0x7FFF);
            const uint32_t pan = m.r32(s + wPanPosition);
            if (pan != 0x8000) {
                int l = 0x7FFF, r = 0x7FFF;
                if (pan & 0x8000) l = int(0xFFFF - (pan & 0xFFFF)); else r = int(pan & 0xFFFF);
                vl = int((int32_t(int16_t(vol)) * l) >> 15);
                vr = int((int32_t(int16_t(vol)) * r) >> 15);
            }
            const double inc = double(m.r32(s + wRate)) / out_rate;
            double &pos = slot_pos[size_t(h)];
            uint32_t consumed = 0;
            for (; f < frames && cur < end; f++) {
                int l, r;
                if (bits == 16) {
                    l = int16_t(m.r16(cur));
                    r = chans == 2 ? int16_t(m.r16(cur + 2)) : l;
                } else {
                    l = (int(m.r8(cur)) - 128) << 8;
                    r = chans == 2 ? (int(m.r8(cur + 1)) - 128) << 8 : l;
                }
                acc[size_t(f) * 2] += (l * vl) >> 15;
                acc[size_t(f) * 2 + 1] += (r * vr) >> 15;
                pos += inc;
                while (pos >= 1.0 && cur < end) {
                    pos -= 1.0;
                    cur += step;
                    consumed++;
                }
            }
            m.w32(s + pSampleCurrent, cur);
            m.w32(s + wTotalBytesProcessed, m.r32(s + wTotalBytesProcessed) + consumed);
        }
    }

    std::vector<int16_t> out(acc.size());
    for (size_t i = 0; i < acc.size(); i++) out[i] = int16_t(std::clamp(acc[i] * master_volume / 100, -32768, 32767));
    if (audio_dev) {
        const uint32_t queued = SDL_GetQueuedAudioSize(audio_dev) / 4;
        if (queued < uint32_t(out_rate) / 5) {             // device keeps up: queue (at most 200 ms ahead)
            if (queued < uint32_t(out_rate) / 100) {       // (almost) run dry: 20 ms of silence as cushion
                std::vector<int16_t> pad(size_t(out_rate / 50) * 2, 0);
                SDL_QueueAudio(audio_dev, pad.data(), uint32_t(pad.size() * 2));
            }
            SDL_QueueAudio(audio_dev, out.data(), uint32_t(out.size() * 2));
        }                                                  // else: device stalls, this piece is dropped
    }
    if (!cfg.wav.empty()) {
        if (!wav_fp) {
            wav_fp = std::fopen(cfg.wav.c_str(), "wb");
            uint8_t header[44] = {};
            if (wav_fp) std::fwrite(header, 1, 44, wav_fp);
        }
        if (wav_fp) wav_bytes += uint32_t(std::fwrite(out.data(), 1, out.size() * 2, wav_fp));
    }
}

void Machine::close_wav() {
    if (!wav_fp) return;
    auto put32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, wav_fp); };
    auto put16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, wav_fp); };
    std::fseek(wav_fp, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, wav_fp); put32(36 + wav_bytes); std::fwrite("WAVEfmt ", 1, 8, wav_fp);
    put32(16); put16(1); put16(2); put32(uint32_t(out_rate)); put32(uint32_t(out_rate) * 4); put16(4); put16(16);
    std::fwrite("data", 1, 4, wav_fp); put32(wav_bytes);
    std::fclose(wav_fp);
    wav_fp = nullptr;
}

// SOS API (Watcom register calling convention: eax, edx, ebx, ecx)
bool Machine::sos_call(uint32_t addr) {
    Cpu &r = c;
    auto slot = [&](uint32_t h) -> uint32_t { return h < SLOTS ? slots_addr + h * SLOT_SIZE : 0; };
    auto active = [&](uint32_t s) { return s && (m.r32(s + wFlags) & ACTIVE); };
    uint32_t ret = 0;
    switch (addr) {
    case hle::k_CPU_Detect:
        m.w16(0x42E68, 5);                      // Pentium
        m.w16(0x42E6A, 3);
        m.w16(0x42EB2, 1000);                   // CpuPower
        m.w16(0x42EB4, 200);                    // CpuSpeed[0]: MHz
        ret = r.eax;
        break;
    case hle::k_Speed_Calc:
        m.w16(0x42EB2, 1000);
        m.w16(0x42EB4, 200);
        ret = r.eax;
        break;

    case hle::k_sosDIGIDetectInit: case hle::k_sosDIGIDetectUnInit: case hle::k_sosDIGIInitSystem:
    case hle::k_sosTIMERInitSystem: case hle::k_sosTIMERUnInitSystem:
        break;
    case hle::k_sosDIGIUnInitSystem: case hle::k_sosDIGIUnInitDriver:
        for (int h = 0; h < SLOTS; h++) m.w32(slot(uint32_t(h)) + wFlags, 0);
        break;
    case hle::k_sosDIGIDetectFindFirst: case hle::k_sosDIGIDetectFindNext:   // (driver *)
        fill_caps(m, r.eax);
        break;
    case hle::k_sosDIGIDetectFindHardware:                                   // (id, driver *)
        fill_caps(m, r.edx);
        break;
    case hle::k_sosDIGIDetectGetSettings:
        break;
    case hle::k_sosDIGIInitDriver:                                           // (driver *, handle *)
        m.w32(r.edx, 0);
        m.w32(r.eax + 0x34, 0);                                             // no pseudo DMA
        break;
    case hle::k_sosTIMERRegisterEvent: {                                     // (rate, fn, handle *)
        TimerEvent ev{next_timer++, r.edx, double(r.eax), now() + (r.eax ? 1.0 / r.eax : 0)};
        timers.push_back(ev);
        m.w32(r.ebx, ev.handle);
        trace("SOS timer %u: %u Hz -> %05X", ev.handle, r.eax, r.edx);
        break;
    }
    case hle::k_sosTIMERAlterEventRate:                                      // (handle, rate)
        for (auto &ev : timers)
            if (ev.handle == r.eax) {
                ev.rate = double(r.edx);
                ev.next = now() + (r.edx ? 1.0 / r.edx : 0);
            }
        break;
    case hle::k_sosTIMERRemoveEvent:
        timers.erase(std::remove_if(timers.begin(), timers.end(), [&](auto &ev) { return ev.handle == r.eax; }),
                     timers.end());
        break;
    case hle::k_sosDIGIStartSample: {                                        // (driver, sample *)
        // on the original hardware the mixer interrupt released stopped samples long before the
        // next one was started (CD access in between); without that, SOS_Stop would later hit a stale slot
        for (int h = 0; h < SLOTS; h++) retire(h);
        ret = 0xFFFFFFFFu;
        for (uint32_t h = 0; h < SLOTS; h++) {
            const uint32_t s = slot(h);
            if (m.r32(s + wFlags) & ACTIVE) continue;
            std::memmove(m.ptr(s), m.ptr(r.edx), SLOT_SIZE);
            m.w32(s + pSampleCurrent, m.r32(s + pSample));
            m.w32(s + hSample, h);
            m.w32(s + wFlags, (m.r32(s + wFlags) & ~(FINISHED | DONE)) | ACTIVE);
            slot_pos[h] = 0;
            ret = h;
            dump_buffer(m, s);
            trace("SOS start slot %u: id %u ptr %08X len %u vol %08X rate %u bits %u ch %u loop %d cb %05X/%05X", h,
                  m.r32(s + wID), m.r32(s + pSample), m.r32(s + wLength), m.r32(s + wVolume), m.r32(s + wRate),
                  m.r32(s + wBitsPerSample), m.r32(s + wChannels), int32_t(m.r32(s + wLoopCount)),
                  m.r32(s + pfnSampleProcessed), m.r32(s + pfnSampleDone));
            break;
        }
        break;
    }
    case hle::k_sosDIGIStopSample: {                                         // (driver, handle)
        const uint32_t s = slot(r.edx);
        if (active(s)) m.w32(s + wFlags, m.r32(s + wFlags) | FINISHED);
        break;
    }
    case hle::k_sosDIGIGetSampleHandle: {                                    // (driver, id)
        // first slot with that ID, active or not (Init_SOS patches the library's compare to this)
        ret = 0xFFFFFFFFu;
        for (uint32_t h = 0; h < SLOTS; h++)
            if (m.r32(slot(h) + wID) == r.edx) {
                ret = m.r32(slot(h) + hSample);
                break;
            }
        break;
    }
    case hle::k_sosDIGISetSampleVolume: case hle::k_sosDIGISetSampleRate: { // (driver, handle, value)
        const uint32_t s = slot(r.edx);
        const uint32_t field = addr == hle::k_sosDIGISetSampleVolume ? wVolume : wRate;
        if (active(s)) {
            ret = m.r32(s + field);
            m.w32(s + field, r.ebx);
        } else {
            ret = 10;
        }
        break;
    }
    case hle::k_sosDIGIGetBytesProcessed: {
        const uint32_t s = slot(r.edx);
        ret = active(s) ? m.r32(s + wTotalBytesProcessed) : 10;
        break;
    }
    default:
        return false;
    }
    if (addr != hle::k_CPU_Detect && addr != hle::k_Speed_Calc) trace("SOS %s -> %d", function_name(addr), int(ret));
    r.eax = ret;
    r.last_ret = m.r32(r.esp);
    r.esp += 4;
    return true;
}

}  // namespace blub
