// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace FEXCore::DualMap {

#ifdef __APPLE__
// Global write offset for iOS dual-mapped JIT memory.
// RX (executable) addresses are canonical. To write to JIT memory,
// add this offset to convert RX→RW.
// Set by the host application before FEXCore initialization.
extern int64_t WriteOffset;

inline void* WriteAddr(void* RXAddr) {
  return reinterpret_cast<uint8_t*>(RXAddr) + WriteOffset;
}

template<typename T>
inline T* WriteAddr(T* RXAddr) {
  return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(RXAddr) + WriteOffset);
}
#else
static constexpr int64_t WriteOffset = 0;

inline void* WriteAddr(void* Addr) { return Addr; }

template<typename T>
inline T* WriteAddr(T* Addr) { return Addr; }
#endif

} // namespace FEXCore::DualMap
