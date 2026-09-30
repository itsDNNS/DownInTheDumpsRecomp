#include "util/bytes.h"

#include <fstream>

namespace blub {

Bytes read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    Bytes b(size_t(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char *>(b.data()), std::streamsize(b.size()));
    return b;
}

} // namespace blub
