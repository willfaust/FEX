// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FEXCore::Utils {

// Castagnoli CRC32C starting from 0, bit-exact match with ARM64 crc32cb/h/w/x.
static inline uint32_t crc32(const uint8_t* Data, size_t Length) {
  uint32_t CRC = 0;
#if defined(__ARM_FEATURE_CRC32)
  while (Length >= 8) {
    uint64_t V;
    std::memcpy(&V, Data, sizeof(V));
    CRC = __builtin_arm_crc32cd(CRC, V);
    Data += 8;
    Length -= 8;
  }
  if (Length >= 4) {
    uint32_t V;
    std::memcpy(&V, Data, sizeof(V));
    CRC = __builtin_arm_crc32cw(CRC, V);
    Data += 4;
    Length -= 4;
  }
  if (Length >= 2) {
    uint16_t V;
    std::memcpy(&V, Data, sizeof(V));
    CRC = __builtin_arm_crc32ch(CRC, V);
    Data += 2;
    Length -= 2;
  }
  if (Length >= 1) {
    CRC = __builtin_arm_crc32cb(CRC, *Data);
  }
#else
  static constexpr auto Table = []() {
    std::array<uint32_t, 256> T {};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int b = 0; b < 8; ++b) {
        c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
      }
      T[i] = c;
    }
    return T;
  }();
  for (size_t i = 0; i < Length; ++i) {
    CRC = Table[(CRC ^ Data[i]) & 0xFFu] ^ (CRC >> 8);
  }
#endif
  return CRC;
}

} // namespace FEXCore::Utils
