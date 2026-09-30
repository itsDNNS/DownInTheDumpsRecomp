#include "data/gamefs.h"
#include "util/i18n.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace blub {

namespace {

std::string upper(std::string s) {
    for (auto &ch : s) ch = char(std::toupper(uint8_t(ch)));
    return s;
}

// ---------------------------------------------------------------- files on the host

class HostFile : public ReadFile {
public:
    explicit HostFile(std::FILE *f) : f_(f) {
        seek64(0, SEEK_END);
        size_ = tell64();
        seek64(0, SEEK_SET);
    }
    ~HostFile() override { std::fclose(f_); }
    size_t read(void *dst, size_t n) override { return std::fread(dst, 1, n, f_); }
    bool seek(uint64_t pos) override { return seek64(int64_t(pos), SEEK_SET); }
    uint64_t tell() const override { return tell64(); }
    uint64_t size() const override { return size_; }

private:
    bool seek64(int64_t pos, int whence) {
#ifdef _WIN32
        return _fseeki64(f_, pos, whence) == 0;
#else
        return fseeko(f_, off_t(pos), whence) == 0;
#endif
    }
    uint64_t tell64() const {
#ifdef _WIN32
        return uint64_t(_ftelli64(f_));
#else
        return uint64_t(ftello(f_));
#endif
    }
    std::FILE *f_;
    uint64_t size_ = 0;
};

std::FILE *fopen_utf8(const std::string &path, const char *mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(fs::u8path(path).wstring().c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// a file inside an image: window [base, base + size) of the image file
class SliceFile : public ReadFile {
public:
    SliceFile(std::unique_ptr<ReadFile> image, uint64_t base, uint64_t size)
        : img_(std::move(image)), base_(base), size_(size) {
        img_->seek(base_);
    }
    size_t read(void *dst, size_t n) override {
        if (pos_ >= size_) return 0;
        n = size_t(std::min<uint64_t>(n, size_ - pos_));
        if (!img_->seek(base_ + pos_)) return 0;
        const size_t got = img_->read(dst, n);
        pos_ += got;
        return got;
    }
    bool seek(uint64_t pos) override {
        pos_ = pos;
        return true;
    }
    uint64_t tell() const override { return pos_; }
    uint64_t size() const override { return size_; }

private:
    std::unique_ptr<ReadFile> img_;
    uint64_t base_, size_, pos_ = 0;
};

// ---------------------------------------------------------------- folder

class FolderSource : public FileSource {
public:
    explicit FolderSource(std::string dir) : dir_(std::move(dir)) {}
    std::string description() const override { return tr("Ordner ", "folder ") + fs::u8path(dir_).filename().u8string(); }
    std::string label() const override { return fs::u8path(dir_).filename().u8string(); }

    const DirEntry *find(const std::vector<std::string> &path) override {
        std::string real;
        return resolve(path, real);
    }
    std::vector<DirEntry> list(const std::vector<std::string> &dir) override {
        std::string real;
        if (!dir.empty()) {
            const DirEntry *e = resolve(dir, real);
            if (!e || !e->dir) return {};
        } else {
            real = dir_;
        }
        std::vector<DirEntry> out;
        for (auto &kv : listing(real)) out.push_back(kv.second);
        return out;
    }
    std::unique_ptr<ReadFile> open(const std::vector<std::string> &path) override {
        std::string real;
        const DirEntry *e = resolve(path, real);
        if (!e || e->dir) return nullptr;
        return open_host_file(real);
    }

private:
    const std::unordered_map<std::string, DirEntry> &listing(const std::string &dir) {
        auto it = cache_.find(dir);
        if (it != cache_.end()) return it->second;
        auto &l = cache_[dir];
        std::error_code ec;
        for (auto &e : fs::directory_iterator(fs::u8path(dir), ec)) {
            DirEntry d;
            d.name = e.path().filename().u8string();
            d.dir = e.is_directory(ec);
            d.size = d.dir ? 0 : uint64_t(e.file_size(ec));
            l.emplace(upper(d.name), d);
        }
        return l;
    }
    const DirEntry *resolve(const std::vector<std::string> &path, std::string &real) {
        real = dir_;
        const DirEntry *e = nullptr;
        for (auto &c : path) {
            if (e && !e->dir) return nullptr;
            const auto &l = listing(real);
            auto it = l.find(upper(c));
            if (it == l.end()) return nullptr;
            e = &it->second;
            real += "/" + e->name;
        }
        return e;
    }
    std::string dir_;
    std::unordered_map<std::string, std::unordered_map<std::string, DirEntry>> cache_;
};

// ---------------------------------------------------------------- ISO 9660

class IsoSource : public FileSource {
public:
    bool open(const std::string &path, std::string *error) {
        path_ = path;
        img_ = open_host_file(path);
        if (!img_) return fail(error, tr("kann nicht geöffnet werden", "cannot be opened"));
        Bytes pvd = img_->read_at(16 * 2048, 2048);
        if (pvd.size() < 2048 || pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5) != 0) {
            // raw 2352-byte sectors (BIN/CUE) are not supported
            Bytes raw = img_->read_at(16 * 2352 + 16, 8);
            if (raw.size() == 8 && std::memcmp(&raw[1], "CD001", 5) == 0)
                return fail(error, tr("ist ein Rohabbild (2352 Byte/Sektor, BIN); bitte als ISO (2048 Byte/Sektor) anlegen",
                                "is a raw image (2352 bytes/sector, BIN); please create an ISO (2048 bytes/sector)"));
            return fail(error, tr("ist kein ISO-9660-Abbild", "is not an ISO 9660 image"));
        }
        block_ = le16(&pvd[128]);
        if (block_ != 2048) return fail(error, tr("hat eine ungewöhnliche Blockgröße", "has an unusual block size"));
        label_ = std::string(reinterpret_cast<const char *>(&pvd[40]), 32);
        while (!label_.empty() && (label_.back() == ' ' || label_.back() == '\0')) label_.pop_back();
        root_.dir = true;
        root_lba_ = le32(&pvd[156 + 2]);
        root_size_ = le32(&pvd[156 + 10]);
        return true;
    }
    std::string description() const override {
        return label_ + " (" + fs::u8path(path_).filename().u8string() + ")";
    }
    std::string label() const override { return label_; }

    const DirEntry *find(const std::vector<std::string> &path) override {
        const Node *n = resolve(path);
        return n ? &n->entry : nullptr;
    }
    std::vector<DirEntry> list(const std::vector<std::string> &dir) override {
        std::vector<DirEntry> out;
        const Node *n = dir.empty() ? nullptr : resolve(dir);
        if (!dir.empty() && (!n || !n->entry.dir)) return out;
        for (auto &kv : children(n ? n->lba : root_lba_, n ? uint32_t(n->entry.size) : root_size_))
            out.push_back(kv.second.entry);
        return out;
    }
    std::unique_ptr<ReadFile> open(const std::vector<std::string> &path) override {
        const Node *n = resolve(path);
        if (!n || n->entry.dir) return nullptr;
        auto f = open_host_file(path_);           // every open file gets its own handle
        if (!f) return nullptr;
        return std::make_unique<SliceFile>(std::move(f), uint64_t(n->lba) * block_, n->entry.size);
    }

private:
    struct Node {
        DirEntry entry;
        uint32_t lba = 0;
    };
    static bool fail(std::string *error, const std::string &m) {
        if (error) *error = m;
        return false;
    }
    // directory records of the directory at lba (cached)
    const std::unordered_map<std::string, Node> &children(uint32_t lba, uint32_t size) {
        auto it = dirs_.find(lba);
        if (it != dirs_.end()) return it->second;
        auto &out = dirs_[lba];
        Bytes d = img_->read_at(uint64_t(lba) * block_, size);
        size_t p = 0;
        while (p < d.size()) {
            const uint8_t len = d[p];
            if (len == 0) {                     // records do not cross sector boundaries
                p = (p / block_ + 1) * block_;
                continue;
            }
            if (p + 33 > d.size() || p + len > d.size()) break;
            const uint8_t nlen = d[p + 32];
            std::string name(reinterpret_cast<const char *>(&d[p + 33]), std::min<size_t>(nlen, len - 33));
            if (!(nlen == 1 && (name[0] == 0 || name[0] == 1))) {   // skip "." and ".."
                if (auto semi = name.find(';'); semi != std::string::npos) name.resize(semi);
                if (!name.empty() && name.back() == '.') name.pop_back();
                Node n;
                n.entry.name = name;
                n.entry.dir = (d[p + 25] & 2) != 0;
                n.entry.size = le32(&d[p + 10]);
                n.lba = le32(&d[p + 2]);
                out.emplace(upper(name), n);
            }
            p += len;
        }
        return out;
    }
    const Node *resolve(const std::vector<std::string> &path) {
        uint32_t lba = root_lba_, size = root_size_;
        const Node *n = nullptr;
        for (auto &c : path) {
            if (n && !n->entry.dir) return nullptr;
            const auto &ch = children(lba, size);
            auto it = ch.find(upper(c));
            if (it == ch.end()) return nullptr;
            n = &it->second;
            lba = n->lba;
            size = uint32_t(n->entry.size);
        }
        return n;
    }

    std::string path_, label_;
    std::unique_ptr<ReadFile> img_;
    uint32_t block_ = 2048, root_lba_ = 0, root_size_ = 0;
    DirEntry root_;
    std::unordered_map<uint32_t, std::unordered_map<std::string, Node>> dirs_;
};

bool is_iso(const fs::path &p) {
    std::string e = upper(p.extension().u8string());
    return e == ".ISO";
}

}  // namespace

Bytes ReadFile::read_at(uint64_t pos, size_t n) {
    Bytes b(n);
    if (!seek(pos)) return {};
    b.resize(read(b.data(), n));
    return b;
}

std::unique_ptr<ReadFile> open_host_file(const std::string &path) {
    std::FILE *f = fopen_utf8(path, "rb");
    if (!f) return nullptr;
    return std::make_unique<HostFile>(f);
}

std::unique_ptr<FileSource> open_folder_source(const std::string &dir) { return std::make_unique<FolderSource>(dir); }

std::unique_ptr<FileSource> open_iso_source(const std::string &iso_path, std::string *error) {
    auto s = std::make_unique<IsoSource>();
    if (!s->open(iso_path, error)) return nullptr;
    return s;
}

std::vector<std::string> dos_components(const std::string &path, const std::string &cwd) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '/', '\\');
    if (p.size() >= 2 && p[1] == ':') p = p.substr(2);
    if (p.empty() || p[0] != '\\') p = cwd + (!cwd.empty() && cwd.back() == '\\' ? "" : "\\") + p;
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('\\', i);
        if (j == std::string::npos) j = p.size();
        std::string part = upper(p.substr(i, j - i));
        if (part == "..") {
            if (!out.empty()) out.pop_back();
        } else if (!part.empty() && part != ".") {
            out.push_back(part);
        }
        i = j + 1;
    }
    return out;
}

bool GameData::open(const std::string &location, std::string *error) {
    discs_.clear();
    std::error_code ec;
    const fs::path loc = fs::u8path(location);
    std::vector<std::string> problems;
    auto add_iso = [&](const fs::path &p) {
        std::string err;
        if (auto s = open_iso_source(p.u8string(), &err))
            discs_.push_back(std::move(s));
        else
            problems.push_back(p.filename().u8string() + " " + err);
    };
    if (fs::is_regular_file(loc, ec) && is_iso(loc)) {
        add_iso(loc);
    } else if (fs::is_directory(loc, ec)) {
        auto folder = open_folder_source(location);
        if (folder->find({"DID.EXE"})) discs_.push_back(std::move(folder));
        // ISO images in the folder and one level below
        std::vector<fs::path> isos;
        for (auto &e : fs::directory_iterator(loc, ec)) {
            if (e.is_regular_file(ec) && is_iso(e.path())) isos.push_back(e.path());
            else if (e.is_directory(ec))
                for (auto &f : fs::directory_iterator(e.path(), ec))
                    if (f.is_regular_file(ec) && is_iso(f.path())) isos.push_back(f.path());
        }
        std::sort(isos.begin(), isos.end());
        for (auto &p : isos) add_iso(p);
    }
    // the disc with DID.EXE first (it is the one the game starts from), then by volume label
    std::stable_sort(discs_.begin(), discs_.end(), [](const auto &a, const auto &b) {
        const bool ea = a->find({"DID.EXE"}) != nullptr, eb = b->find({"DID.EXE"}) != nullptr;
        if (ea != eb) return ea;
        return a->label() < b->label();
    });
    if (error) {
        error->clear();
        for (auto &p : problems) *error += (error->empty() ? "" : "; ") + p;
    }
    return !discs_.empty();
}

const DirEntry *GameData::find(const std::vector<std::string> &path, FileSource **on) {
    for (auto &d : discs_)
        if (const DirEntry *e = d->find(path)) {
            if (on) *on = d.get();
            return e;
        }
    return nullptr;
}

std::unique_ptr<ReadFile> GameData::open_file(const std::vector<std::string> &path) {
    for (auto &d : discs_)
        if (auto f = d->open(path)) return f;
    return nullptr;
}

Bytes GameData::read_all(const std::string &dos_path) {
    auto f = open_file(dos_components(dos_path));
    if (!f) return {};
    return f->read_at(0, size_t(f->size()));
}

std::vector<DirEntry> GameData::list(const std::vector<std::string> &dir) {
    std::vector<DirEntry> out;
    for (auto &d : discs_)
        for (auto &e : d->list(dir))
            if (std::none_of(out.begin(), out.end(), [&](const DirEntry &x) { return upper(x.name) == upper(e.name); }))
                out.push_back(e);
    return out;
}

std::string find_portable_game_data(const std::string &program_dir) {
    for (const char *sub : {"ISOs", "ISO", "Spieldaten", "CD"}) {
        const fs::path p = fs::u8path(program_dir) / sub;
        std::error_code ec;
        if (!fs::is_directory(p, ec)) continue;
        GameData g;
        if (g.open(p.u8string())) return p.u8string();
    }
    return "";
}

}  // namespace blub
