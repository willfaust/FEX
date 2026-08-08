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

// ml357: STALE ALIASES ARE FATAL, and the old de-dupe guaranteed them.
//
// The previous rule was "same PeBase = already registered, ignore" — so a DLL
// that unloaded and was replaced at the same (or an overlapping) PE VA kept
// FEX pointing at the ORIGINAL pool copy forever. That copy is tombstoned and
// eventually reclaimed/zeroed, so every guest RIP in the new module translated
// into dead memory: ml356 died executing a page of zeros (udf → c000001d) one
// instruction after wevtapi.dll loaded INSIDE a departed module's old range.
//
// Wine's own table (ios_jit_add_mapping) already purges by OVERLAP for exactly
// this reason; this is that rule's missing twin on the FEX side. Tombstone
// order matches wine's: Size = 0 first (a zero-size entry matches no range
// query, including Module.S's inline asm walk), barrier, then reuse the slot.
/* iOS-Mythic ml549: EXACT guest RIP from a host PC, for the unix-side fault probes.
 *
 * The Mach handlers in ntdll-unix read the guest RIP from CpuStateFrame+0x18, which FEX
 * only syncs at BLOCK boundaries -- it names the calling block, not the instruction that
 * ran. ml548 disassembled such a RIP and got call setup instead of the store it was
 * hunting, which is why the render-corruption writer could never be identified.
 *
 * FEX already stores a host-PC -> guest-RIP table in every JIT block tail and walks it
 * for exception reconstruction. ios_fex_rip_from_hostpc (Core.cpp) is that walk without
 * the Thread dependency; this is its EC export so the unix side can reach it through the
 * same bind-and-push path as BTCpu64IosAddAliasMapping.
 *
 * Zero runtime cost: nothing is instrumented, the table already exists. Returns 0 when
 * the PC is outside the block, so a caller can distinguish "no answer" from a real RIP. */
uint64_t BTCpu64IosRipFromHostPC(uint64_t BlockBegin, uint64_t HostPC) {
  extern uint64_t ios_fex_rip_from_hostpc(uint64_t, uint64_t);
  return ios_fex_rip_from_hostpc(BlockBegin, HostPC);
}

void BTCpu64IosAddAliasMapping(uint64_t PeBase, uint64_t JitBase, uint64_t Size) {
  int count = g_EntryCount;

  // Identical re-registration: nothing to do.
  for (int i = 0; i < count; i++) {
    if (g_Entries[i].PeBase == PeBase && g_Entries[i].JitBase == JitBase && g_Entries[i].Size == Size) {
      return;
    }
  }

  // Retire every entry whose PE range overlaps the incoming image: a live
  // image proves any overlapping entry is dead (two images cannot share a VA).
  for (int i = 0; i < count; i++) {
    const uint64_t pb = g_Entries[i].PeBase;
    const uint64_t sz = g_Entries[i].Size;
    if (!sz) {
      continue;
    }
    if (pb < PeBase + Size && PeBase < pb + sz) {
      g_Entries[i].Size = 0;
      __sync_synchronize();
    }
  }

  // Prefer a retired slot so long-running processes don't exhaust the table.
  for (int i = 0; i < count; i++) {
    if (!g_Entries[i].Size) {
      g_Entries[i].PeBase = PeBase;
      g_Entries[i].JitBase = JitBase;
      __sync_synchronize();
      g_Entries[i].Size = Size; // published last: readers see a complete entry
      return;
    }
  }

  if (count >= kMaxEntries) {
    return;
  }
  g_Entries[count].PeBase = PeBase;
  g_Entries[count].JitBase = JitBase;
  g_Entries[count].Size = Size;
  __sync_synchronize();
  g_EntryCount = count + 1;
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
