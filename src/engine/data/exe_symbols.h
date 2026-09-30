#pragma once
// Addresses in the original DID.EXE (German release, 575949 bytes, sha1 9556506e3160fc05b4fb0dfa7c3e49e95727cbe2),
// taken from its Watcom debug information. Generated - see scripts/gen_exe_symbols.py.
#include <cstdint>

namespace blub::exesym {

constexpr uint32_t EXE_SIZE = 575949u;
constexpr const char *EXE_SHA1 = "9556506e3160fc05b4fb0dfa7c3e49e95727cbe2";
constexpr uint32_t esp_col = 0x564EAu;
constexpr uint32_t Buff_Dcp_FID = 0x5650Eu;
constexpr uint32_t pixi = 0x564FEu;
constexpr uint32_t qspr = 0x56506u;
constexpr uint32_t pspr = 0x56502u;
constexpr uint32_t feoption = 0x56520u;
constexpr uint32_t FILSize1 = 0x564A8u;
constexpr uint32_t FR_Disp = 0x56512u;
constexpr uint32_t SzBlockAdpcm = 0x5119Eu;
constexpr uint32_t decomp = 0x2623Eu;
constexpr uint32_t FIL_DecodeFrame = 0x2CB15u;

}  // namespace blub::exesym
