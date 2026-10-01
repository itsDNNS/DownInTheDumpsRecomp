// The Watcom C runtime functions DID.EXE calls (data/recomp_hle.txt), implemented on the host.
// Register calling convention: arguments in eax, edx, ebx, ecx, result in eax, every other register
// is preserved. Functions with a variable argument list (open, printf) take all arguments on the
// stack and the caller removes them.
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "host/machine.h"
#include "recomp/hle_addrs.h"

namespace blub {

namespace {
int lower(int ch) { return std::tolower(ch & 0xFF); }
}  // namespace

// printf-style formatting with the arguments on the guest stack
std::string Machine::format(const std::string &fmt, uint32_t args) {
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        std::string spec = "%";
        size_t j = i + 1;
        while (j < fmt.size() && std::strchr("-+ #0123456789.", fmt[j])) spec += fmt[j++];
        while (j < fmt.size() && std::strchr("hlLNF", fmt[j])) j++;          // size prefixes: all 32 bit
        if (j >= fmt.size()) break;
        const char conv = fmt[j];
        char buf[512];
        switch (conv) {
        case 'd': case 'i': std::snprintf(buf, sizeof buf, (spec + "d").c_str(), int32_t(m.r32(args))); args += 4; break;
        case 'u': case 'x': case 'X': case 'o': case 'c':
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), m.r32(args)); args += 4; break;
        case 's': std::snprintf(buf, sizeof buf, (spec + "s").c_str(), gstr(m.r32(args), 400).c_str()); args += 4; break;
        case 'p': std::snprintf(buf, sizeof buf, "%08X", m.r32(args)); args += 4; break;
        case 'f': case 'e': case 'g': case 'E': case 'G': {
            double v;
            m.read(args, &v, 8);
            args += 8;
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), v);
            break;
        }
        case '%': std::snprintf(buf, sizeof buf, "%%"); break;
        default: buf[0] = 0; break;
        }
        out += buf;
        i = j;
    }
    return out;
}

bool Machine::crt_call(uint32_t addr) {
    Cpu &r = c;
    const uint32_t a0 = r.eax, a1 = r.edx, a2 = r.ebx;
    const uint32_t sp = r.esp + 4;           // first stack argument (behind the return address)
    uint32_t ret = r.eax;
    uint32_t pop = 0;                         // extra bytes removed by the callee
    switch (addr) {
    // ---- files
    case hle::k_open: {
        const int access = int(m.r32(sp + 4));
        ret = uint32_t(dos_open(gstr(m.r32(sp)), access & 3, (access & 0x20) != 0, (access & 0x40) != 0));
        break;
    }
    case hle::k_close: ret = uint32_t(dos_close(int(a0))); break;
    case hle::k_read: ret = uint32_t(dos_read(int(a0), a1, a2)); break;
    case hle::k_write: ret = uint32_t(dos_write(int(a0), a1, a2)); break;
    case hle::k_lseek: ret = uint32_t(dos_seek(int(a0), int32_t(a1), int(a2))); break;
    case hle::k_filelength: ret = uint32_t(dos_length(int(a0))); break;
    case hle::k_unlink: ret = uint32_t(dos_unlink(gstr(a0))); break;
    case hle::k_rename: ret = uint32_t(dos_rename(gstr(a0), gstr(a1))); break;
    case hle::k_mkdir: ret = uint32_t(dos_mkdir(gstr(a0))); break;
    case hle::k_chdir: ret = uint32_t(dos_chdir(gstr(a0))); break;
    case hle::k__dos_setdrive: if (a1) m.w32(a1, 26); ret = 0; break;
    case hle::k__dos_findfirst: ret = uint32_t(dos_findfirst(gstr(a0), a2)); break;
    case hle::k__dos_findnext: ret = uint32_t(dos_findnext(a0)); break;
    case hle::k__getdiskfree:                  // (drive, struct _diskfree_t *): plenty of space (1 GB)
        // four unsigned shorts in this Watcom version: GetDiskSpace reads words at offsets 0..6
        // (with 32-bit fields the game saw no free space and offered no saving at all)
        m.w16(a1 + 0, 65535);                  // total clusters
        m.w16(a1 + 2, 32768);                  // available clusters
        m.w16(a1 + 4, 64);                     // sectors per cluster
        m.w16(a1 + 6, 512);                    // bytes per sector
        ret = 0;
        break;
    case hle::k___get_errno_ptr: ret = errno_addr; break;

    // ---- process / console
    case hle::k_getenv: ret = 0; break;       // no WinDir: the game must not think it runs under Windows
    case hle::k_printf: {
        std::string s = format(gstr(m.r32(sp)), sp + 4);
        std::fputs(s.c_str(), stdout);
        ret = uint32_t(s.size());
        break;
    }
    case hle::k_puts: std::puts(gstr(a0).c_str()); ret = 1; break;
    case hle::k_exit: std::fflush(stdout); throw GuestExit{int(a0)};
    case hle::k__harderr: ret = 0; break;
    case hle::k_int386: {                      // (int no, union REGS *in, union REGS *out)
        Cpu t = r;
        t.eax = m.r32(a1 + 0); t.ebx = m.r32(a1 + 4); t.ecx = m.r32(a1 + 8);
        t.edx = m.r32(a1 + 12); t.esi = m.r32(a1 + 16); t.edi = m.r32(a1 + 20);
        t.f.cf = false;
        host_int(t, m, int(a0));
        m.w32(a2 + 0, t.eax); m.w32(a2 + 4, t.ebx); m.w32(a2 + 8, t.ecx);
        m.w32(a2 + 12, t.edx); m.w32(a2 + 16, t.esi); m.w32(a2 + 20, t.edi);
        m.w32(a2 + 24, t.f.cf ? 1 : 0);
        ret = t.eax;
        break;
    }

    // ---- strings and memory
    case hle::k_strcpy: {
        uint32_t i = 0;
        uint8_t ch;
        do { ch = m.r8(a1 + i); m.w8(a0 + i, ch); i++; } while (ch);
        ret = a0;
        break;
    }
    case hle::k_strncpy: {
        bool end = false;
        for (uint32_t i = 0; i < a2; i++) {
            uint8_t ch = end ? 0 : m.r8(a1 + i);
            if (!ch) end = true;
            m.w8(a0 + i, ch);
        }
        ret = a0;
        break;
    }
    case hle::k_strcat: {
        uint32_t d = a0;
        while (m.r8(d)) d++;
        uint32_t i = 0;
        uint8_t ch;
        do { ch = m.r8(a1 + i); m.w8(d + i, ch); i++; } while (ch);
        ret = a0;
        break;
    }
    case hle::k_strchr: case hle::k_strrchr: {
        const uint8_t want = uint8_t(a1);
        ret = 0;
        for (uint32_t p = a0;; p++) {
            const uint8_t ch = m.r8(p);
            if (ch == want) {
                ret = p;
                if (addr == hle::k_strchr) break;
            }
            if (!ch) break;
        }
        break;
    }
    case hle::k_strlen: { uint32_t n = 0; while (m.r8(a0 + n)) n++; ret = n; break; }
    case hle::k_stricmp: case hle::k_strnicmp: {
        const uint32_t limit = addr == hle::k_strnicmp ? a2 : 0xFFFFFFFFu;
        int d = 0;
        for (uint32_t i = 0; i < limit; i++) {
            const int x = lower(m.r8(a0 + i)), y = lower(m.r8(a1 + i));
            d = x - y;
            if (d || !x) break;
        }
        ret = uint32_t(d);
        break;
    }
    case hle::k_memset: std::memset(m.span(a0, a2), int(a1 & 0xFF), a2); ret = a0; break;
    case hle::k_memcpy: case hle::k_memmove: std::memmove(m.span(a0, a2), m.span(a1, a2), a2); ret = a0; break;

    // ---- numbers
    case hle::k_abs: ret = uint32_t(std::abs(int32_t(a0))); break;
    case hle::k_atoi: ret = uint32_t(std::atoi(gstr(a0, 64).c_str())); break;
    case hle::k_srand: break;
    case hle::k_sqrt: {                        // double on the stack, result in st(0), ret 8
        double v;
        m.read(sp, &v, 8);
        r.fpu.push(sqrtl((long double)v));
        pop = 8;
        break;
    }
    case hle::k___CHP: r.fpu.st(0) = truncl(r.fpu.st(0)); break;
    default:
        return false;
    }
    r.eax = ret;
    r.last_ret = m.r32(r.esp);
    r.esp += 4 + pop;
    return true;
}

}  // namespace blub
