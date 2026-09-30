#include "data/gap.h"

namespace blub {

bool GapArchive::open(const std::string &path) { return open(open_host_file(path), path); }

bool GapArchive::open(std::unique_ptr<ReadFile> file, const std::string &name) {
    path_ = name;
    f_ = std::move(file);
    if (!f_) return false;
    const uint64_t size = f_->size();
    Bytes hdr = f_->read_at(0, 4);
    if (hdr.size() < 4) return false;
    index_start_ = le32(hdr.data());
    if (index_start_ >= size) return false;
    Bytes idx = f_->read_at(index_start_, size_t(size - index_start_));
    offs_.resize(idx.size() / 4);
    for (size_t i = 0; i < offs_.size(); i++) offs_[i] = le32(&idx[4 * i]);
    return offs_.size() >= 2;
}

bool GapArchive::valid(int id) const {
    const int i = id + 1;
    if (i < 0 || i + 1 >= int(offs_.size())) return false;
    return offs_[i] > 0 && offs_[i] + 18 <= offs_[i + 1] && offs_[i + 1] <= index_start_;
}

ResHead GapArchive::head(int id) {
    ResHead h;
    if (!valid(id)) return h;
    Bytes hb = f_->read_at(offs_[id + 1], 18);
    if (hb.size() < 18) return h;
    const uint8_t *b = hb.data();
    h.type = le16(b);
    h.attr = le16(b + 2);
    h.x = les16(b + 4);
    h.y = les16(b + 6);
    h.w = les16(b + 8);
    h.h = les16(b + 10);
    h.depth = les16(b + 12);
    h.size = le32(b + 14);
    return h;
}

Bytes GapArchive::payload(int id) {
    if (!valid(id)) return {};
    return read_at(offs_[id + 1] + 18, offs_[id + 2] - offs_[id + 1] - 18);
}

Bytes GapArchive::read_at(uint32_t file_offset, uint32_t size) { return f_->read_at(file_offset, size); }

} // namespace blub
