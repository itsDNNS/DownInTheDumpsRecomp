// DOS/4GW services: DPMI (int 31h) selectors and memory, simulated real-mode interrupts for VESA
// (int 10h) and MSCDEX (int 2Fh).
#include <SDL.h>

#include "host/machine.h"

namespace blub {

uint32_t Machine::selector_base(uint16_t sel) const {
    auto it = selectors.find(uint16_t(sel & ~3u));
    if (it == selectors.end()) it = selectors.find(sel);
    return it == selectors.end() ? 0 : it->second;
}

namespace {
// real-mode register structure of DPMI 0300h
enum RM : uint32_t { EDI = 0x00, ESI = 0x04, EBP = 0x08, EBX = 0x10, EDX = 0x14, ECX = 0x18, EAX = 0x1C,
                     FLAGS = 0x20, ES = 0x22, DS = 0x24 };
}  // namespace

void Machine::real_mode_int(int vector, uint32_t rm) {
    uint32_t eax = m.r32(rm + EAX), ebx = m.r32(rm + EBX), ecx = m.r32(rm + ECX), edx = m.r32(rm + EDX);
    const uint32_t es = m.r16(rm + ES);
    uint16_t flags = m.r16(rm + FLAGS) & ~1u;
    if (vector == 0x10) {
        uint32_t buf = es * 16 + (m.r32(rm + EDI) & 0xFFFF);
        if (!vesa(eax, ebx, ecx, edx, buf)) flags |= 1;
    } else if (vector == 0x2F) {
        const uint32_t buf = es * 16 + (ebx & 0xFFFF);
        switch (eax & 0xFFFF) {
        case 0x1500:                         // installation check
            ebx = (ebx & 0xFFFF0000u) | 1;
            ecx = (ecx & 0xFFFF0000u) | 3;
            break;
        case 0x150D:                         // drive letters
            m.w8(buf, 3);
            break;
        case 0x1508: {                       // absolute disk read: only the CD speed test of the setup
            // GetCDSpeed divides by the timer ticks the reads took: behave like an 8x drive
            const double seconds = double(edx & 0xFFFF) * 2048.0 / 1.2e6, until = now() + seconds;
            while (now() < until) {
                poll();
                sleep_for(0.001);
            }
            break;
        }
        case 0x1510:                         // device driver request: report "done"
            m.w16(buf + 3, 0x0100);
            break;
        default:
            trace("MSCDEX %04X ignored", eax & 0xFFFF);
            break;
        }
    } else {
        trace("real-mode int %02Xh ignored", vector);
    }
    m.w32(rm + EAX, eax);
    m.w32(rm + EBX, ebx);
    m.w32(rm + ECX, ecx);
    m.w32(rm + EDX, edx);
    m.w16(rm + FLAGS, flags);
}

void Machine::int31(Cpu &r) {
    const uint16_t ax = uint16_t(r.eax);
    auto lo = [](uint32_t &reg, uint32_t v) { reg = (reg & 0xFFFF0000u) | (v & 0xFFFF); };
    r.f.cf = false;
    switch (ax) {
    case 0x0000: {                          // allocate descriptors
        uint16_t first = next_selector;
        for (uint32_t i = 0; i < (r.ecx & 0xFFFF); i++) selectors[next_selector] = 0, next_selector += 8;
        lo(r.eax, first);
        break;
    }
    case 0x0001: selectors.erase(uint16_t(r.ebx)); break;
    case 0x0002: {                          // segment to descriptor
        uint32_t seg = r.ebx & 0xFFFF;
        uint32_t base = seg == 0xA000 ? layout::VGA_WINDOW : seg * 16;
        uint16_t found = 0;
        for (auto &s : selectors)
            if (s.first >= 0x100 && s.second == base) found = s.first;
        if (!found) {
            found = next_selector;
            next_selector += 8;
            selectors[found] = base;
        }
        lo(r.eax, found);
        break;
    }
    case 0x0003: lo(r.eax, 8); break;
    case 0x0006: {
        uint32_t base = selector_base(uint16_t(r.ebx));
        lo(r.ecx, base >> 16);
        lo(r.edx, base);
        break;
    }
    case 0x0007: selectors[uint16_t(r.ebx)] = ((r.ecx & 0xFFFF) << 16) | (r.edx & 0xFFFF); break;
    case 0x0008: case 0x0009: break;       // limit / access rights
    case 0x0100: {                          // allocate DOS memory (BX paragraphs)
        uint32_t want = (r.ebx & 0xFFFF) * 16;
        uint32_t avail = layout::DOS_END - dos_brk;
        if (want > avail) {
            lo(r.eax, 8);
            lo(r.ebx, avail / 16);
            r.f.cf = true;
            break;
        }
        uint32_t base = dos_brk;
        dos_brk += (want + 15) & ~15u;
        uint16_t sel = next_selector;
        next_selector += 8;
        selectors[sel] = base;
        lo(r.eax, base / 16);
        lo(r.edx, sel);
        trace("DPMI: %u bytes DOS memory at %05X", want, base);
        break;
    }
    case 0x0101: break;                     // free DOS memory
    case 0x0300: case 0x0301: case 0x0302:
        real_mode_int(int(r.ebx & 0xFF), r.sb[0] + r.edi);
        break;
    case 0x0400: lo(r.eax, 0x005A); lo(r.ebx, 1); lo(r.ecx, 4); lo(r.edx, 0x0870); break;
    case 0x0500: {                          // free memory information
        const uint32_t buf = r.sb[0] + r.edi;
        const uint32_t free_bytes = Arena::SIZE - heap_brk;
        for (uint32_t i = 0; i < 0x30; i += 4) m.w32(buf + i, 0xFFFFFFFFu);
        m.w32(buf + 0x00, free_bytes);
        m.w32(buf + 0x04, free_bytes / 4096);
        m.w32(buf + 0x08, free_bytes / 4096);
        m.w32(buf + 0x0C, (Arena::SIZE - layout::HEAP_BASE) / 4096);
        m.w32(buf + 0x10, free_bytes / 4096);
        m.w32(buf + 0x14, free_bytes / 4096);
        m.w32(buf + 0x18, Arena::SIZE / 4096);
        m.w32(buf + 0x1C, free_bytes / 4096);
        m.w32(buf + 0x20, 0);
        break;
    }
    case 0x0501: case 0x0503: {             // allocate / resize memory block (BX:CX bytes)
        uint32_t size = ((r.ebx & 0xFFFF) << 16) | (r.ecx & 0xFFFF);
        uint32_t handle = ((r.esi & 0xFFFF) << 16) | (r.edi & 0xFFFF);
        uint32_t addr;
        if (ax == 0x0503 && blocks.count(handle) && blocks[handle].addr + size <= Arena::SIZE &&
            (blocks[handle].addr + blocks[handle].size == heap_brk || size <= blocks[handle].size)) {
            Block &b = blocks[handle];          // grow/shrink in place (the last block)
            if (b.addr + b.size == heap_brk) heap_brk = (b.addr + size + 0xFFF) & ~0xFFFu;
            b.size = size;
            addr = b.addr;
        } else {
            if (heap_brk + size > Arena::SIZE || size == 0) {
                lo(r.eax, 0x8013);
                r.f.cf = true;
                break;
            }
            addr = heap_brk;
            heap_brk = (heap_brk + size + 0xFFF) & ~0xFFFu;
            if (ax == 0x0503 && blocks.count(handle)) {
                Block old = blocks[handle];
                const uint32_t n = std::min(old.size, size);
                std::memmove(m.span(addr, n), m.span(old.addr, n), n);
                blocks.erase(handle);
            }
            handle = next_handle++;
            blocks[handle] = Block{addr, size};
        }
        lo(r.ebx, addr >> 16);
        lo(r.ecx, addr);
        lo(r.esi, handle >> 16);
        lo(r.edi, handle);
        trace("DPMI %04X: %u bytes at %08X (handle %u)", ax, size, addr, handle);
        break;
    }
    case 0x0502: blocks.erase(((r.esi & 0xFFFF) << 16) | (r.edi & 0xFFFF)); break;
    case 0x0600: case 0x0601: case 0x0702: case 0x0703: break;   // (un)lock, page hints
    default:
        trace("DPMI %04X not implemented", ax);
        r.f.cf = true;
        break;
    }
}

}  // namespace blub
