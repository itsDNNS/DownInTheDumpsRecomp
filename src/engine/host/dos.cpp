// DOS file system on top of the host: DOS paths ("D:\TOON1\CARTOON1.GAP", relative paths, any drive
// letter) are looked up case-insensitively in the save directory first and then on the game discs
// (folders or ISO images, data/gamefs.h). Everything the game writes goes to the save directory
// (copy-on-write).
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <unordered_map>

#include "host/machine.h"

namespace fs = std::filesystem;

namespace blub {

namespace {

constexpr int ENOENT_ = 1, EACCES_ = 5, EBADF_ = 4, EEXIST_ = 7;   // Watcom errno values

std::string upper(std::string s) {
    for (auto &ch : s) ch = char(std::toupper(uint8_t(ch)));
    return s;
}

std::FILE *fopen_utf8(const std::string &path, const char *mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(fs::u8path(path).wstring().c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// case-insensitive lookup below a host directory (save directory); out = host path
bool find_ci(const std::string &root, const std::vector<std::string> &comps, std::string &out) {
    std::string cur = root;
    for (auto &c : comps) {
        bool found = false;
        std::error_code ec;
        for (auto &e : fs::directory_iterator(fs::u8path(cur), ec)) {
            std::string n = e.path().filename().u8string();
            if (upper(n) == c) {
                cur += "/" + n;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    out = cur;
    return true;
}

bool wild_match(const char *pat, const char *s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            for (const char *t = s;; t++) {
                if (wild_match(pat + 1, t)) return true;
                if (!*t) return false;
            }
        }
        if (!*s || (*pat != '?' && *pat != *s)) return false;
    }
    return !*s;
}

// DOS "*.*" matches names without an extension too
bool dos_match(const std::string &pat, const std::string &name) {
    if (pat == "*.*" || pat == "*") return true;
    if (wild_match(pat.c_str(), name.c_str())) return true;
    return name.find('.') == std::string::npos && wild_match(pat.c_str(), (name + ".").c_str());
}

}  // namespace

// host path of a DOS path in the save directory; exists: it is already there
std::string Machine::save_path(const std::vector<std::string> &comps, bool *exists) {
    std::string out;
    if (find_ci(cfg.save_dir, comps, out)) {
        if (exists) *exists = true;
        return out;
    }
    if (exists) *exists = false;
    std::string cur = cfg.save_dir;               // new file: keep the case of existing parents
    for (auto &c : comps) {
        std::string next;
        if (find_ci(cur, {c}, next)) cur = next;
        else cur += "/" + c;
    }
    return cur;
}

bool Machine::dos_exists(const std::string &path, bool *is_dir) {
    auto comps = dos_components(path, cwd);
    std::string hp;
    std::error_code ec;
    if (find_ci(cfg.save_dir, comps, hp)) {
        if (is_dir) *is_dir = fs::is_directory(fs::u8path(hp), ec);
        return true;
    }
    if (comps.empty()) {
        if (is_dir) *is_dir = true;
        return true;
    }
    const DirEntry *e = game.find(comps);
    if (e && is_dir) *is_dir = e->dir;
    return e != nullptr;
}

int Machine::dos_open(const std::string &path, int mode, bool create, bool truncate) {
    const auto comps = dos_components(path, cwd);
    DosFile f;
    bool in_save = false;
    if (create || truncate) {
        f.host_path = save_path(comps, &in_save);
        std::error_code ec;
        fs::create_directories(fs::u8path(f.host_path).parent_path(), ec);
        f.fp = fopen_utf8(f.host_path, "wb+");
        f.writable = true;
    } else if ((mode & 3) == 0) {
        f.host_path = save_path(comps, &in_save);
        if (in_save) f.fp = fopen_utf8(f.host_path, "rb");
        else f.ro = game.open_file(comps);
    } else {
        f.host_path = save_path(comps, &in_save);
        if (!in_save) {                              // copy-on-write from the disc
            auto src = game.open_file(comps);
            if (!src) {
                m.w32(errno_addr, ENOENT_);
                return -1;
            }
            std::error_code ec;
            fs::create_directories(fs::u8path(f.host_path).parent_path(), ec);
            if (std::FILE *out = fopen_utf8(f.host_path, "wb")) {
                Bytes data = src->read_at(0, size_t(src->size()));
                std::fwrite(data.data(), 1, data.size(), out);
                std::fclose(out);
            }
        }
        f.fp = fopen_utf8(f.host_path, "rb+");
        f.writable = true;
    }
    if (!f.fp && !f.ro) {
        trace("open %s -> failed", path.c_str());
        note("open %s (failed)", path.c_str());
        m.w32(errno_addr, in_save ? EACCES_ : ENOENT_);
        return -1;
    }
    int h = 5;
    while (files.count(h)) h++;
    trace("open %s (mode %d%s) -> %d %s", path.c_str(), mode, create ? " create" : "", h,
          f.ro ? "(disc)" : f.host_path.c_str());
    note("open %s%s", path.c_str(), create || truncate ? " (new)" : (mode & 3) ? " (write)" : "");
    files[h] = std::move(f);
    return h;
}

int Machine::dos_close(int h) {
    auto it = files.find(h);
    if (it == files.end()) {
        m.w32(errno_addr, EBADF_);
        return -1;
    }
    if (it->second.fp) std::fclose(it->second.fp);
    files.erase(it);
    return 0;
}

int64_t Machine::dos_read(int h, uint32_t buf, uint32_t n) {
    auto it = files.find(h);
    if (it == files.end()) {
        m.w32(errno_addr, EBADF_);
        return -1;
    }
    if (buf >= Arena::SIZE) wild_access(buf, n);
    n = std::min(n, Arena::SIZE - buf);      // "read up to n bytes" may name more than there is
    DosFile &f = it->second;
    const int64_t got = f.ro ? int64_t(f.ro->read(m.ptr(buf), n)) : int64_t(std::fread(m.ptr(buf), 1, n, f.fp));
    if (n >= 65536 || got < int64_t(n)) trace("read %d: %u bytes -> %lld", h, n, (long long)got);
    return got;
}

int64_t Machine::dos_write(int h, uint32_t buf, uint32_t n) {
    if (h == 1 || h == 2) {
        std::fwrite(m.span(buf, n), 1, n, stdout);
        return n;
    }
    auto it = files.find(h);
    if (it == files.end() || !it->second.writable) {
        m.w32(errno_addr, it == files.end() ? EBADF_ : EACCES_);
        return -1;
    }
    if (n == 0) {                            // DOS: writing 0 bytes truncates at the file position
        std::fflush(it->second.fp);
        long pos = std::ftell(it->second.fp);
        std::error_code ec;
        fs::resize_file(fs::u8path(it->second.host_path), uintmax_t(pos), ec);
        return 0;
    }
    return int64_t(std::fwrite(m.span(buf, n), 1, n, it->second.fp));
}

int64_t Machine::dos_seek(int h, int64_t off, int whence) {
    auto it = files.find(h);
    if (it == files.end()) {
        m.w32(errno_addr, EBADF_);
        return -1;
    }
    DosFile &f = it->second;
    if (f.ro) {
        const int64_t base = whence == 1 ? int64_t(f.ro->tell()) : whence == 2 ? int64_t(f.ro->size()) : 0;
        if (base + off < 0) return -1;
        f.ro->seek(uint64_t(base + off));
        return int64_t(f.ro->tell());
    }
    if (std::fseek(f.fp, long(off), whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET) != 0) return -1;
    return std::ftell(f.fp);
}

int64_t Machine::dos_length(int h) {
    auto it = files.find(h);
    if (it == files.end()) return -1;
    DosFile &f = it->second;
    if (f.ro) return int64_t(f.ro->size());
    long pos = std::ftell(f.fp);
    std::fseek(f.fp, 0, SEEK_END);
    long len = std::ftell(f.fp);
    std::fseek(f.fp, pos, SEEK_SET);
    return len;
}

int Machine::dos_unlink(const std::string &path) {
    bool exists = false;
    std::string p = save_path(dos_components(path, cwd), &exists);
    std::error_code ec;
    if (!exists || !fs::remove(fs::u8path(p), ec)) {
        m.w32(errno_addr, exists ? EACCES_ : ENOENT_);   // files on a disc cannot be deleted
        return -1;
    }
    return 0;
}

int Machine::dos_rename(const std::string &from, const std::string &to) {
    bool exists = false;
    std::string a = save_path(dos_components(from, cwd), &exists);
    if (!exists) {
        m.w32(errno_addr, ENOENT_);
        return -1;
    }
    if (dos_exists(to)) {
        m.w32(errno_addr, EEXIST_);
        return -1;
    }
    std::string b = save_path(dos_components(to, cwd));
    std::error_code ec;
    fs::rename(fs::u8path(a), fs::u8path(b), ec);
    return ec ? -1 : 0;
}

int Machine::dos_mkdir(const std::string &path) {
    if (dos_exists(path)) {
        m.w32(errno_addr, EEXIST_);
        return -1;
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(save_path(dos_components(path, cwd))), ec);
    return ec ? -1 : 0;
}

int Machine::dos_chdir(const std::string &path) {
    bool dir = false;
    if (!dos_exists(path, &dir) || !dir) {
        m.w32(errno_addr, ENOENT_);
        return -1;
    }
    std::string d;
    for (auto &c : dos_components(path, cwd)) d += "\\" + c;
    cwd = d.empty() ? "\\" : d;
    return 0;
}

// Watcom struct find_t: reserved[21], attrib, wr_time, wr_date, size, name[13]
int Machine::dos_findfirst(const std::string &pattern, uint32_t find_t) {
    auto comps = dos_components(pattern, cwd);
    if (comps.empty()) return 2;
    const std::string mask = comps.back();
    comps.pop_back();
    FindState st;
    auto add = [&](const std::string &name, bool dir, uint64_t size) {
        const std::string u = upper(name);
        if (!dos_match(mask, u)) return;
        if (std::any_of(st.hits.begin(), st.hits.end(), [&](auto &h) { return h.first == u; })) return;
        st.hits.emplace_back(u, dir ? UINT64_MAX : size);
    };
    std::string dir;
    if (find_ci(cfg.save_dir, comps, dir)) {
        std::error_code ec;
        for (auto &e : fs::directory_iterator(fs::u8path(dir), ec)) {
            const bool is_dir = e.is_directory(ec);
            add(e.path().filename().u8string(), is_dir, is_dir ? 0 : uint64_t(e.file_size(ec)));
        }
    }
    for (auto &e : game.list(comps)) add(e.name, e.dir, e.size);
    std::sort(st.hits.begin(), st.hits.end());
    finds[find_t] = std::move(st);
    return dos_findnext(find_t);
}

int Machine::dos_findnext(uint32_t find_t) {
    auto it = finds.find(find_t);
    if (it == finds.end() || it->second.next >= it->second.hits.size()) {
        if (it != finds.end()) finds.erase(it);
        return 18;                           // no more files
    }
    auto &h = it->second.hits[it->second.next++];
    bool dir = h.second == UINT64_MAX;
    m.w8(find_t + 21, dir ? 0x10 : 0x20);
    m.w16(find_t + 22, 0);
    m.w16(find_t + 24, (16 << 9) | (1 << 5) | 1);    // 1.1.1996
    m.w32(find_t + 26, dir ? 0 : uint32_t(h.second));
    std::string n = h.first.substr(0, 12);
    put_str(find_t + 30, n);
    return 0;
}

// ---------------------------------------------------------------- int 21h

void Machine::int21(Cpu &r) {
    const uint8_t ah = uint8_t(r.eax >> 8), al = uint8_t(r.eax);
    const uint32_t ds_edx = r.sb[3] + r.edx;
    auto ok = [&](uint32_t eax) { r.eax = eax; r.f.cf = false; };
    auto err = [&](uint16_t code) { r.eax = (r.eax & 0xFFFF0000u) | code; r.f.cf = true; };
    switch (ah) {
    case 0x09: {                            // print string terminated by '$'
        std::string s;
        for (uint32_t a = ds_edx; m.r8(a) != '$' && s.size() < 4096; a++) s += char(m.r8(a));
        std::fputs(s.c_str(), stdout);
        break;
    }
    case 0x0E: r.eax = (r.eax & 0xFFFFFF00u) | 26; break;          // select drive: 26 drives
    case 0x19: r.eax = (r.eax & 0xFFFFFF00u) | 2; break;           // current drive C:
    case 0x3B: if (dos_chdir(gstr(ds_edx)) == 0) ok(r.eax); else err(3); break;
    case 0x3C: {
        int h = dos_open(gstr(ds_edx), 2, true, true);
        if (h < 0) err(3); else ok(uint32_t(h));
        break;
    }
    case 0x3D: {
        int h = dos_open(gstr(ds_edx), al & 3, false, false);
        if (h < 0) err(2); else ok(uint32_t(h));
        break;
    }
    case 0x3E: if (dos_close(int(r.ebx & 0xFFFF)) == 0) ok(r.eax); else err(6); break;
    case 0x3F: {
        int64_t n = dos_read(int(r.ebx & 0xFFFF), ds_edx, r.ecx);
        if (n < 0) err(6); else ok(uint32_t(n));
        break;
    }
    case 0x40: {
        int64_t n = dos_write(int(r.ebx & 0xFFFF), ds_edx, r.ecx);
        if (n < 0) err(6); else ok(uint32_t(n));
        break;
    }
    case 0x41: if (dos_unlink(gstr(ds_edx)) == 0) ok(r.eax); else err(2); break;
    case 0x42: {
        int64_t off = int32_t(((r.ecx & 0xFFFF) << 16) | (r.edx & 0xFFFF));
        int64_t pos = dos_seek(int(r.ebx & 0xFFFF), off, al);
        if (pos < 0) {
            err(6);
        } else {
            ok(uint32_t(pos) & 0xFFFF);
            r.edx = (r.edx & 0xFFFF0000u) | (uint32_t(pos) >> 16);
        }
        break;
    }
    case 0x47: {                            // get current directory (without the leading '\')
        put_str(r.sb[3] + r.esi, cwd.size() > 1 ? cwd.substr(1) : "");
        ok(r.eax);
        break;
    }
    case 0x4C: throw GuestExit{al};
    default: trace("int 21h ah=%02X not implemented", ah); err(1); break;
    }
}

}  // namespace blub
