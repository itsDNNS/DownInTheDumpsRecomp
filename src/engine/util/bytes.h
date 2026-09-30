#pragma once
// Little-endian helpers and a bounds-checked reader for the original data formats.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace blub {

using Bytes = std::vector<uint8_t>;

inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
inline int16_t les16(const uint8_t *p) { return int16_t(le16(p)); }
inline uint32_t le32(const uint8_t *p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Sequential reader over a byte span; throws FormatError instead of reading past the end.
class Reader {
public:
    Reader(const uint8_t *data, size_t size, size_t pos = 0) : d_(data), n_(size), p_(pos) {}
    explicit Reader(const Bytes &b, size_t pos = 0) : Reader(b.data(), b.size(), pos) {}

    size_t pos() const { return p_; }
    size_t size() const { return n_; }
    size_t left() const { return p_ < n_ ? n_ - p_ : 0; }
    bool eof() const { return p_ >= n_; }
    void seek(size_t p) { need_at(p, 0); p_ = p; }
    void skip(size_t k) { need(k); p_ += k; }
    const uint8_t *ptr() const { return d_ + p_; }
    const uint8_t *data() const { return d_; }

    uint8_t u8() { need(1); return d_[p_++]; }
    uint16_t u16() { need(2); uint16_t v = le16(d_ + p_); p_ += 2; return v; }
    int16_t s16() { return int16_t(u16()); }
    uint32_t u32() { need(4); uint32_t v = le32(d_ + p_); p_ += 4; return v; }
    int32_t s32() { return int32_t(u32()); }
    uint16_t peek16(size_t k = 0) const { need_at(p_ + 2 * k, 2); return le16(d_ + p_ + 2 * k); }
    void bytes(uint8_t *out, size_t k) { need(k); std::memcpy(out, d_ + p_, k); p_ += k; }

private:
    void need(size_t k) const { need_at(p_, k); }
    void need_at(size_t at, size_t k) const {
        if (at > n_ || k > n_ - at) throw FormatError("read past end of data");
    }
    const uint8_t *d_;
    size_t n_, p_;
};

Bytes read_file(const std::string &path);

} // namespace blub
