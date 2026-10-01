#pragma once
// Runtime for code generated from DID.EXE: scripts/lift.py (the codecs) and scripts/recomp.py (the
// whole game). The generated functions work on a virtual 32-bit address space (Arena) that holds the
// code/data objects of DID.EXE at their link addresses, the stack and the heap.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace blub {

// A read or write outside the address space is a bug of the game, which on DOS ends with a page
// fault. The host layer reports it as a fatal error (host/machine.cpp).
[[noreturn]] void wild_access(uint32_t addr, size_t size);

class Arena {
public:
    static constexpr uint32_t SIZE = 0x04000000;       // 64 MB
    static constexpr uint32_t STACK_TOP = 0x00F00000;
    static constexpr uint32_t HEAP_BASE = 0x01000000;

    Arena() : mem_(SIZE + 4, 0) {}
    uint8_t *ptr(uint32_t a) { check(a); return &mem_[a]; }
    const uint8_t *ptr(uint32_t a) const { check(a); return &mem_[a]; }
    // n bytes from a, all of them inside the address space
    uint8_t *span(uint32_t a, size_t n) { check(a, n); return &mem_[a]; }
    const uint8_t *span(uint32_t a, size_t n) const { check(a, n); return &mem_[a]; }
    void write(uint32_t a, const void *src, size_t n) { std::memcpy(span(a, n), src, n); }
    void read(uint32_t a, void *dst, size_t n) const { std::memcpy(dst, span(a, n), n); }
    uint32_t alloc(uint32_t n) {
        uint32_t a = brk_;
        brk_ = (brk_ + n + 0xFFF) & ~0xFFFu;
        return a;
    }
    void reset_heap() { brk_ = HEAP_BASE; }
    uint32_t brk() const { return brk_; }

    // little-endian host assumed (x86/ARM); the 4 guard bytes make unaligned reads at the end safe
    uint8_t r8(uint32_t a) const { check(a); return mem_[a]; }
    uint16_t r16(uint32_t a) const { check(a); uint16_t v; std::memcpy(&v, &mem_[a], 2); return v; }
    uint32_t r32(uint32_t a) const { check(a); uint32_t v; std::memcpy(&v, &mem_[a], 4); return v; }
    void w8(uint32_t a, uint32_t v) { check(a); mem_[a] = uint8_t(v); }
    void w16(uint32_t a, uint32_t v) { check(a); uint16_t x = uint16_t(v); std::memcpy(&mem_[a], &x, 2); }
    void w32(uint32_t a, uint32_t v) { check(a); std::memcpy(&mem_[a], &v, 4); }

private:
    static void check(uint32_t a) {
        if (__builtin_expect(a >= SIZE, 0)) wild_access(a, 1);
    }
    static void check(uint32_t a, size_t n) {
        if (__builtin_expect(a > SIZE || n > SIZE - a, 0)) wild_access(a, n);
    }

    std::vector<uint8_t> mem_;
    uint32_t brk_ = HEAP_BASE;
};

// x86 arithmetic with the flags the generated code tests
struct Flags {
    bool cf = false, zf = false, sf = false, of = false, df = false;
    uint8_t res = 1;       // low byte of the last result (parity flag)

    bool pf() const { return !__builtin_parity(res); }
    template <int B> static constexpr uint32_t mask() { return B == 32 ? 0xFFFFFFFFu : ((1u << B) - 1); }
    template <int B> void szf(uint32_t r) { zf = (r & mask<B>()) == 0; sf = (r >> (B - 1)) & 1; res = uint8_t(r); }

    // EFLAGS image for pushf/popf (IF set, AF not modelled)
    uint32_t eflags() const {
        return uint32_t(cf) | 2u | (uint32_t(pf()) << 2) | (uint32_t(zf) << 6) | (uint32_t(sf) << 7) | 0x200u |
               (uint32_t(df) << 10) | (uint32_t(of) << 11);
    }
    void set_eflags(uint32_t v) {
        cf = v & 1; res = (v & 4) ? 0 : 1; zf = (v >> 6) & 1; sf = (v >> 7) & 1; df = (v >> 10) & 1; of = (v >> 11) & 1;
    }
    uint8_t low() const { return uint8_t(eflags()); }                    // lahf
    void set_low(uint8_t v) { bool o = of, d = df; set_eflags(v); of = o; df = d; }   // sahf

    template <int B> uint32_t add(uint32_t a, uint32_t b) {
        uint64_t r = uint64_t(a & mask<B>()) + (b & mask<B>());
        cf = (r >> B) & 1;
        of = (((a ^ ~b) & (a ^ uint32_t(r))) >> (B - 1)) & 1;
        szf<B>(uint32_t(r));
        return uint32_t(r) & mask<B>();
    }
    template <int B> uint32_t sub(uint32_t a, uint32_t b) {
        a &= mask<B>(); b &= mask<B>();
        uint32_t r = (a - b) & mask<B>();
        cf = a < b;
        of = (((a ^ b) & (a ^ r)) >> (B - 1)) & 1;
        szf<B>(r);
        return r;
    }
    template <int B> uint32_t logic(uint32_t r) { cf = of = false; szf<B>(r); return r & mask<B>(); }
    template <int B> uint32_t inc(uint32_t a) { bool c = cf; uint32_t r = add<B>(a, 1); cf = c; return r; }
    template <int B> uint32_t dec(uint32_t a) { bool c = cf; uint32_t r = sub<B>(a, 1); cf = c; return r; }
    template <int B> uint32_t neg(uint32_t a) { uint32_t r = sub<B>(0, a); cf = (a & mask<B>()) != 0; return r; }
    template <int B> uint32_t shr(uint32_t a, uint32_t n) {
        n &= 31; a &= mask<B>();
        if (!n) return a;
        cf = n <= uint32_t(B) ? (a >> (n - 1)) & 1 : 0;
        uint32_t r = n < 32 ? a >> n : 0;
        of = (a >> (B - 1)) & 1;
        szf<B>(r);
        return r;
    }
    template <int B> uint32_t sar(uint32_t a, uint32_t n) {
        n &= 31;
        int32_t s = int32_t(a << (32 - B)) >> (32 - B);
        if (!n) return a & mask<B>();
        cf = (s >> (n - 1)) & 1;
        uint32_t r = uint32_t(s >> n) & mask<B>();
        of = false;
        szf<B>(r);
        return r;
    }
    template <int B> uint32_t shl(uint32_t a, uint32_t n) {
        n &= 31; a &= mask<B>();
        if (!n) return a;
        cf = n <= uint32_t(B) ? (a >> (B - n)) & 1 : 0;
        uint32_t r = (uint64_t(a) << n) & mask<B>();
        of = ((r >> (B - 1)) & 1) != cf;
        szf<B>(r);
        return r;
    }
    template <int B> uint32_t rol(uint32_t a, uint32_t n) {
        a &= mask<B>(); n &= 31; n %= B;
        if (!n) return a;
        uint32_t r = ((a << n) | (a >> (B - n))) & mask<B>();
        cf = r & 1;
        return r;
    }
    template <int B> uint32_t ror(uint32_t a, uint32_t n) {
        a &= mask<B>(); n &= 31; n %= B;
        if (!n) return a;
        uint32_t r = ((a >> n) | (a << (B - n))) & mask<B>();
        cf = (r >> (B - 1)) & 1;
        return r;
    }
    template <int B> uint32_t rcl(uint32_t a, uint32_t n) {
        n = (n & 31) % (B + 1);
        uint64_t v = a & mask<B>();
        while (n--) { uint64_t c = cf; cf = (v >> (B - 1)) & 1; v = ((v << 1) | c) & mask<B>(); }
        return uint32_t(v);
    }
    template <int B> uint32_t rcr(uint32_t a, uint32_t n) {
        n = (n & 31) % (B + 1);
        uint64_t v = a & mask<B>();
        while (n--) { uint64_t c = cf; cf = v & 1; v = (v >> 1) | (c << (B - 1)); }
        return uint32_t(v);
    }
    template <int B> uint32_t imul(uint32_t a, uint32_t b) {
        int64_t sa = int32_t(a << (32 - B)) >> (32 - B), sb = int32_t(b << (32 - B)) >> (32 - B);
        int64_t r = sa * sb;
        uint32_t t = uint32_t(r) & mask<B>();
        int64_t back = int32_t(t << (32 - B)) >> (32 - B);
        cf = of = back != r;
        szf<B>(t);
        return t;
    }
    uint32_t shld32(uint32_t a, uint32_t b, uint32_t n) {
        n &= 31;
        if (!n) return a;
        uint32_t r = (a << n) | (b >> (32 - n));
        cf = (a >> (32 - n)) & 1;
        szf<32>(r);
        return r;
    }
    uint32_t shrd32(uint32_t a, uint32_t b, uint32_t n) {
        n &= 31;
        if (!n) return a;
        uint32_t r = (a >> n) | (b << (32 - n));
        cf = (a >> (n - 1)) & 1;
        szf<32>(r);
        return r;
    }

#define BLUB_FLAG_OPS(B)                                                                  \
    uint32_t add##B(uint32_t a, uint32_t b) { return add<B>(a, b); }                      \
    uint32_t sub##B(uint32_t a, uint32_t b) { return sub<B>(a, b); }                      \
    uint32_t and##B(uint32_t a, uint32_t b) { return logic<B>(a & b); }                   \
    uint32_t or##B(uint32_t a, uint32_t b) { return logic<B>(a | b); }                    \
    uint32_t xor##B(uint32_t a, uint32_t b) { return logic<B>(a ^ b); }                   \
    uint32_t adc##B(uint32_t a, uint32_t b) { bool c = cf; uint32_t r = add<B>(a, b);     \
        if (c) { bool c1 = cf; r = add<B>(r, 1); cf = cf || c1; } return r; }            \
    uint32_t sbb##B(uint32_t a, uint32_t b) { bool c = cf; uint32_t r = sub<B>(a, b);     \
        if (c) { bool c1 = cf; r = sub<B>(r, 1); cf = cf || c1; } return r; }            \
    uint32_t inc##B(uint32_t a) { return inc<B>(a); }                                     \
    uint32_t dec##B(uint32_t a) { return dec<B>(a); }                                     \
    uint32_t neg##B(uint32_t a) { return neg<B>(a); }                                     \
    uint32_t not##B(uint32_t a) { return ~a & mask<B>(); }                                \
    uint32_t shr##B(uint32_t a, uint32_t n) { return shr<B>(a, n); }                      \
    uint32_t sar##B(uint32_t a, uint32_t n) { return sar<B>(a, n); }                      \
    uint32_t shl##B(uint32_t a, uint32_t n) { return shl<B>(a, n); }                      \
    uint32_t rol##B(uint32_t a, uint32_t n) { return rol<B>(a, n); }                      \
    uint32_t ror##B(uint32_t a, uint32_t n) { return ror<B>(a, n); }                      \
    uint32_t rcl##B(uint32_t a, uint32_t n) { return rcl<B>(a, n); }                      \
    uint32_t rcr##B(uint32_t a, uint32_t n) { return rcr<B>(a, n); }                      \
    uint32_t imul##B(uint32_t a, uint32_t b) { return imul<B>(a, b); }
    BLUB_FLAG_OPS(8)
    BLUB_FLAG_OPS(16)
    BLUB_FLAG_OPS(32)
#undef BLUB_FLAG_OPS
};

// x87 register stack (long double; the game only uses a handful of FPU instructions)
struct Fpu {
    long double r[8] = {};
    int top = 0;
    uint16_t sw = 0, cw = 0x037F;

    long double &st(int i) { return r[(top + i) & 7]; }
    void push(long double v) { top = (top - 1) & 7; r[top] = v; }
    void pop() { top = (top + 1) & 7; }
    void init() { top = 0; sw = 0; cw = 0x037F; }
    uint16_t status() const { return uint16_t((sw & ~0x3800) | (top << 11)); }

    long double round(long double v) const {
        switch ((cw >> 10) & 3) {
        case 0: return nearbyintl(v);         // host default rounding mode is round-to-nearest-even
        case 1: return floorl(v);
        case 2: return ceill(v);
        default: return truncl(v);
        }
    }
    template <typename T> static T get(const Arena &m, uint32_t a) { T v; m.read(a, &v, sizeof v); return v; }
    template <typename T> static void put(Arena &m, uint32_t a, T v) { m.write(a, &v, sizeof v); }
    long double load_f4(const Arena &m, uint32_t a) { return get<float>(m, a); }
    long double load_f8(const Arena &m, uint32_t a) { return get<double>(m, a); }
    long double load_i2(const Arena &m, uint32_t a) { return get<int16_t>(m, a); }
    long double load_i4(const Arena &m, uint32_t a) { return get<int32_t>(m, a); }
    long double load_i8(const Arena &m, uint32_t a) { return (long double)get<int64_t>(m, a); }
    void store_f4(Arena &m, uint32_t a) { put<float>(m, a, float(st(0))); }
    void store_f8(Arena &m, uint32_t a) { put<double>(m, a, double(st(0))); }
    template <typename T> T to_int(long double v) const {
        long double x = round(v);
        if (!(x >= (long double)INT64_MIN && x <= (long double)INT64_MAX)) return T(1ull << (sizeof(T) * 8 - 1));
        int64_t i = int64_t(x);
        if (i < int64_t(T(1ull << (sizeof(T) * 8 - 1))) || (sizeof(T) < 8 && i > int64_t((1ull << (sizeof(T) * 8 - 1)) - 1)))
            return T(1ull << (sizeof(T) * 8 - 1));          // integer indefinite
        return T(i);
    }
    void store_i2(Arena &m, uint32_t a) { put<int16_t>(m, a, to_int<int16_t>(st(0))); }
    void store_i4(Arena &m, uint32_t a) { put<int32_t>(m, a, to_int<int32_t>(st(0))); }
    void store_i8(Arena &m, uint32_t a) { put<int64_t>(m, a, to_int<int64_t>(st(0))); }

    // st(d) = st(d) <op> v;  'R' / 'Q' are the reversed forms (fsubr / fdivr)
    void arith(int d, long double v, char op) {
        long double &x = st(d);
        switch (op) {
        case '+': x = x + v; break;
        case '-': x = x - v; break;
        case '*': x = x * v; break;
        case '/': x = x / v; break;
        case 'R': x = v - x; break;
        case 'Q': x = v / x; break;
        }
    }
    void compare(long double a, long double b) {
        sw &= ~0x4500;
        if (std::isnan(a) || std::isnan(b)) sw |= 0x4500;
        else if (a < b) sw |= 0x0100;
        else if (a == b) sw |= 0x4000;
    }
    void sincos() { long double v = st(0); st(0) = sinl(v); push(cosl(v)); }
    void fyl2x() { long double x = st(0); pop(); st(0) = st(0) * log2l(x); }
};

constexpr long double kPi = 3.14159265358979323846264338327950288L;
constexpr long double kLg2 = 0.30102999566398119521373889472449303L;
constexpr long double kLn2 = 0.69314718055994530941723212145817657L;
constexpr long double kL2e = 1.44269504088896340735992468100189214L;
constexpr long double kL2t = 3.32192809488736234787031942948939018L;

struct Cpu {
    uint32_t eax = 0, ecx = 0, edx = 0, ebx = 0, esp = Arena::STACK_TOP, ebp = 0, esi = 0, edi = 0;
    Flags f;
    uint32_t fault = 0;      // lift.py codecs: != 0 after a jump to an address outside the lifted code

    // --- used by the whole-program recompilation (scripts/recomp.py)
    Fpu fpu;
    uint32_t last_ret = 0;   // return address popped by the last `ret`
    uint16_t sel[6] = {};    // es cs ss ds fs gs
    uint32_t sb[6] = {};     // their base addresses
    int32_t budget = 0;      // backward branches until the next host poll

    uint32_t step(uint32_t size) const { return f.df ? uint32_t(0) - size : size; }

    void mul8(uint32_t v) { uint32_t r = (eax & 0xFF) * (v & 0xFF); eax = (eax & 0xFFFF0000u) | r; f.cf = f.of = r > 0xFF; }
    void mul16(uint32_t v) {
        uint32_t r = (eax & 0xFFFF) * (v & 0xFFFF);
        eax = (eax & 0xFFFF0000u) | (r & 0xFFFF); edx = (edx & 0xFFFF0000u) | (r >> 16); f.cf = f.of = (r >> 16) != 0;
    }
    void mul32(uint32_t v) { uint64_t r = uint64_t(eax) * v; eax = uint32_t(r); edx = uint32_t(r >> 32); f.cf = f.of = edx != 0; }
    void imul8(uint32_t v) {
        int32_t r = int8_t(eax) * int8_t(v); eax = (eax & 0xFFFF0000u) | (uint16_t)r; f.cf = f.of = r != int8_t(r);
    }
    void imul16(uint32_t v) {
        int32_t r = int16_t(eax) * int16_t(v);
        eax = (eax & 0xFFFF0000u) | uint16_t(r); edx = (edx & 0xFFFF0000u) | uint16_t(r >> 16); f.cf = f.of = r != int16_t(r);
    }
    void imul32(uint32_t v) {
        int64_t r = int64_t(int32_t(eax)) * int32_t(v);
        eax = uint32_t(r); edx = uint32_t(uint64_t(r) >> 32); f.cf = f.of = r != int32_t(r);
    }
    void div8(uint32_t v) {
        uint32_t n = eax & 0xFFFF, d = v & 0xFF;
        if (!d || n / d > 0xFF) return divide_error();
        eax = (eax & 0xFFFF0000u) | ((n % d) << 8) | (n / d);
    }
    void div16(uint32_t v) {
        uint32_t n = ((edx & 0xFFFF) << 16) | (eax & 0xFFFF), d = v & 0xFFFF;
        if (!d || n / d > 0xFFFF) return divide_error();
        eax = (eax & 0xFFFF0000u) | (n / d); edx = (edx & 0xFFFF0000u) | (n % d);
    }
    void div32(uint32_t v) {
        uint64_t n = (uint64_t(edx) << 32) | eax;
        if (!v || n / v > 0xFFFFFFFFull) return divide_error();
        eax = uint32_t(n / v); edx = uint32_t(n % v);
    }
    void idiv8(uint32_t v) {
        int32_t n = int16_t(eax), d = int8_t(v);
        if (!d || n / d > 127 || n / d < -128) return divide_error();
        eax = (eax & 0xFFFF0000u) | (uint8_t(n % d) << 8) | uint8_t(n / d);
    }
    void idiv16(uint32_t v) {
        int32_t n = int32_t(((edx & 0xFFFF) << 16) | (eax & 0xFFFF)), d = int16_t(v);
        if (!d || n / d > 32767 || n / d < -32768) return divide_error();
        eax = (eax & 0xFFFF0000u) | uint16_t(n / d); edx = (edx & 0xFFFF0000u) | uint16_t(n % d);
    }
    void idiv32(uint32_t v) {
        int64_t n = int64_t((uint64_t(edx) << 32) | eax), d = int32_t(v);
        if (!d || (n == INT64_MIN && d == -1) || n / d > INT32_MAX || n / d < INT32_MIN) return divide_error();
        eax = uint32_t(int32_t(n / d)); edx = uint32_t(int32_t(n % d));
    }

    // implemented by the host layer (host/cpu_host.cpp)
    void load_seg(int reg, uint16_t selector);
    void divide_error();
    void bad_return(uint32_t call_site, uint32_t expected);
    void bad_target(uint32_t addr);
    void unsupported(uint32_t addr, const char *why);
    void halt();
};

// Generated routines (src/engine/codec/lifted_*.cpp)
void lifted_fil_frame(Cpu &c, Arena &m);   // FIL_DecodeFrame with all FIL codecs (decomp, Biz, FLC)
void lifted_decomp(Cpu &c, Arena &m);      // decomp (LZ stage + expand stage)

} // namespace blub
