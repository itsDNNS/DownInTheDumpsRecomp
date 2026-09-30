#pragma once
// Loads the data object of the user's original DID.EXE (LE format, DOS/4GW) with its relocations
// applied.  The recompiled codecs read their lookup tables from it, so the port itself does not
// contain any data of the original program.
#include <cstdint>
#include <string>

#include "util/bytes.h"

namespace blub {

struct ExeImage {
    uint32_t code_base = 0, data_base = 0;
    Bytes code, data;
    bool load(const std::string &exe_path, std::string *error = nullptr);
    bool load_bytes(const Bytes &file, std::string *error = nullptr);
};

} // namespace blub
