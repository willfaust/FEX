// SPDX-License-Identifier: MIT
// iOS JIT-pool alias table.
//
// On iOS, ARM64EC PE images cannot run from their loaded address (iOS rejects
// mprotect(RX) on file-backed mmaps). Wine's ntdll-unix copies each PE image
// into the dual-mapped JIT pool. Code in the copied image executes from the
// alias address, not the original PE address.
//
// FEX's range trackers only know the ORIGINAL PE address ranges, so they
// report alias addresses as non-executable. The lookup wrappers in
// Module.cpp call IosJitReverseTranslate to map alias → PE-original for
// the tracker query, then IosJitTranslate to map the result back.
//
// Lives in a separate TU from Module.cpp because accessing the table's
// global storage directly from ARM64EC class methods in Module.cpp produced
// a "misaligned ldr/str offset" link error — probably an ADRP+LDR
// page-offset issue in the hybrid emit. Putting the data + accessors in
// their own TU sidesteps the issue.

#include <cstdint>

#ifdef FEX_IOS_HOST

struct IosAliasEntry {
  uint64_t PeBase;
  uint64_t JitBase;
  uint64_t Size;
  uint64_t _Padding;  // Keeps the struct 32-byte to make array stride a power of 2.
};
constexpr int kMaxEntries = 256;

// extern "C" storage (not anonymous-namespace) so Module.S's ExitFunctionEC
// can reach the table via adrp/:lo12: — it translates the branch target
// PE VA → pool VA inline before jumping to native EC code. That asm relies
// on the 32-byte stride and the PeBase/JitBase/Size field order.
extern "C" {
IosAliasEntry IosAliasEntries[kMaxEntries];
volatile int IosAliasCount = 0;
}

namespace {
IosAliasEntry* const g_Entries = IosAliasEntries;
volatile int& g_EntryCount = IosAliasCount;
}  // namespace

extern "C" {

void BTCpu64IosAddAliasMapping(uint64_t PeBase, uint64_t JitBase, uint64_t Size) {
  int idx = g_EntryCount;
  // De-dupe: same PE base = already registered.
  for (int i = 0; i < idx; i++) {
    if (g_Entries[i].PeBase == PeBase) return;
  }
  if (idx >= kMaxEntries) return;
  g_Entries[idx].PeBase = PeBase;
  g_Entries[idx].JitBase = JitBase;
  g_Entries[idx].Size = Size;
  __sync_synchronize();
  g_EntryCount = idx + 1;
}

uint64_t IosJitTranslate(uint64_t Addr) {
  int count = g_EntryCount;
  for (int i = 0; i < count; i++) {
    uint64_t pb = g_Entries[i].PeBase;
    uint64_t sz = g_Entries[i].Size;
    if (Addr >= pb && Addr < pb + sz) {
      return g_Entries[i].JitBase + (Addr - pb);
    }
  }
  return Addr;
}

uint64_t IosJitReverseTranslate(uint64_t Addr) {
  int count = g_EntryCount;
  for (int i = 0; i < count; i++) {
    uint64_t jb = g_Entries[i].JitBase;
    uint64_t sz = g_Entries[i].Size;
    if (Addr >= jb && Addr < jb + sz) {
      return g_Entries[i].PeBase + (Addr - jb);
    }
  }
  return Addr;
}

}  // extern "C"

#endif  // FEX_IOS_HOST
