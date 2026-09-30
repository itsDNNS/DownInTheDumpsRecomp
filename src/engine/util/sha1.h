#pragma once
// SHA-1 (only used to recognize the original DID.EXE)
#include <cstddef>
#include <cstdint>
#include <string>

namespace blub {

std::string sha1_hex(const uint8_t *data, size_t size);

}  // namespace blub
