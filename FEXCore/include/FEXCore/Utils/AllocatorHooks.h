// SPDX-License-Identifier: MIT
#pragma once
#include <FEXCore/Utils/CompilerDefs.h>
#include <FEXCore/Utils/EnumOperators.h>
#include <FEXCore/Utils/LogManager.h>

#ifdef _WIN32
// Windows
#elif defined(__APPLE__)
#include <stdlib.h>
#include <sys/mman.h>
#else
// Linux
#include <stdlib.h>
#include <malloc.h>
#include <sys/mman.h>
#endif

#ifdef _WIN32
#define NTDDI_VERSION 0x0A000005
#include <memoryapi.h>
#endif

#include <new>
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace FEXCore::Allocator {
enum class ProtectOptions : uint32_t {
  None = 0,
  Read = (1U << 0),
  Write = (1U << 1),
  Exec = (1U << 2),
};
FEX_DEF_NUM_OPS(ProtectOptions)

enum class THPControl {
  Enable,
  Disable,
};

#ifndef _WIN32
FEX_DEFAULT_VISIBILITY void SetupHooks(size_t PageSize);
#else
using VirtualNamePtr = void (*)(const char*, const void*, size_t);
using VirtualTHPPtr = void (*)(const void*, size_t, THPControl);
struct HookPtrs {
  VirtualNamePtr VirtualName;
  VirtualTHPPtr VirtualTHPControl;
};
FEX_DEFAULT_VISIBILITY void SetupHooks(size_t PageSize, HookPtrs Ptrs);
#endif
FEX_DEFAULT_VISIBILITY void ClearHooks();

#ifdef _WIN32
inline void* VirtualAlloc(void* Base, size_t Size, bool Execute = false, bool Commit = true) {
  // Allocate top-down to avoid polluting the lower VA space, as even on 64-bit some programs (i.e. LuaJIT) require allocations below 4GB.
  DWORD Flags = (Commit ? MEM_COMMIT : 0) | MEM_RESERVE | MEM_TOP_DOWN;
#ifdef ARCHITECTURE_arm64ec
#ifdef FEX_IOS_HOST
  /* iOS-Mythic ml321: keep FEX's host structures OUT of the guest VA band.
   *
   * Every FEXMem_* region (Lookup/L1, BlockLinks, CallRetStacks, OpDispatcher,
   * Frontend, ThreadState, ...) allocates through here, and by default wine's VA
   * scanner places them in the same sub-ceiling band as guest allocations. ml308
   * caught FEXMem_BlockLinks mapped DIRECTLY above a guest thread stack: a guest
   * over-read there silently returns host JIT code labels instead of faulting,
   * and ml316's webhelper died branching to exactly such a value ([iOS-bogusrip]
   * band=FEX-CODE-BUFFER, "loaded from guest memory"). On Windows the space above
   * a stack is a guard/unmapped region; here it was a table of host code pointers.
   *
   * Steer non-exec, any-address allocations into [0x7c00000000, 0x8000000000) --
   * the band FEX's own jemalloc arenas already occupy, so it is host-only memory
   * with no guest allocation in it.
   *
   * ml325 CORRECTION: the first cut used [0x7400000000, 0x7800000000), which broke
   * CEF. PartitionAlloc reserves four 16GB jumbo pools and two of them land at
   * 0x7400000000 / 0x77ffff0000; ~3.8GB of FEXMem reserves scattered through that
   * window fragmented it, so only 2 of 4 pools were obtained ("jumbo fail" x23,
   * ml320 got all four). The guest then hit STATUS_NO_MEMORY, ignored it, and
   * dereferenced NULL -- run depth fell 48k -> 18k calls. Keep FEX out of
   * [0x7400000000, 0x7c00000000): that whole span belongs to CEF's pools.
   *
   * On ANY failure fall through to the unconstrained path -- placement is a
   * hardening, never a new fatal (#43).
   * Exec allocations are excluded: EC_CODE buffers have their own JIT-pool
   * steering that must keep control of placement. */
  if (!Base && !Execute) {
    MEM_ADDRESS_REQUIREMENTS AddrReq {};
    MEM_EXTENDED_PARAMETER AddrParam {};
    AddrReq.LowestStartingAddress = reinterpret_cast<void*>(0x7C00000000ULL);
    AddrReq.HighestEndingAddress = reinterpret_cast<void*>(0x7FFFFFFFFFULL);
    AddrParam.Type = MemExtendedParameterAddressRequirements;
    AddrParam.Pointer = &AddrReq;
    // No MEM_TOP_DOWN here: Windows rejects it in combination with address requirements.
    void* Ret = ::VirtualAlloc2(nullptr, nullptr, Size, (Commit ? MEM_COMMIT : 0) | MEM_RESERVE, PAGE_READWRITE, &AddrParam, 1);
    if (Ret) {
      return Ret;
    }
  }
#endif
  MEM_EXTENDED_PARAMETER Parameter {};
  if (Execute) {
    Parameter.Type = MemExtendedParameterAttributeFlags;
    Parameter.ULong64 = MEM_EXTENDED_PARAMETER_EC_CODE;
  };
  return ::VirtualAlloc2(nullptr, Base, Size, Flags, Execute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, Execute ? &Parameter : nullptr,
                         Execute ? 1 : 0);
#else
  return ::VirtualAlloc(Base, Size, Flags, Execute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
#endif
}

inline void* VirtualAlloc(size_t Size, bool Execute = false, bool Commit = true) {
  return VirtualAlloc(nullptr, Size, Execute, Commit);
}

inline void VirtualFree(void* Ptr, size_t Size) {
  ::VirtualFree(Ptr, 0, MEM_RELEASE);
}

inline void VirtualDontNeed(void* Ptr, size_t Size, bool Recommit = true) {
  // Zero the page-aligned region, preserving permissions.
  MEMORY_BASIC_INFORMATION Info;
  ::VirtualQuery(Ptr, &Info, sizeof(Info));
  ::VirtualFree(Ptr, Size, MEM_DECOMMIT);
  if (Recommit) {
    ::VirtualAlloc(Ptr, Size, MEM_COMMIT, Info.Protect);
  }
}

inline bool VirtualProtect(void* Ptr, size_t Size, ProtectOptions options) {
  DWORD prot {PAGE_NOACCESS};

  if (options == ProtectOptions::None) {
    prot = PAGE_NOACCESS;
  } else if (options == ProtectOptions::Read) {
    prot = PAGE_READONLY;
  } else if (options == (ProtectOptions::Read | ProtectOptions::Write)) {
    prot = PAGE_READWRITE;
  } else if (options == (ProtectOptions::Read | ProtectOptions::Exec)) {
    prot = PAGE_EXECUTE_READ;
  } else if (options == (ProtectOptions::Read | ProtectOptions::Write | ProtectOptions::Exec)) {
    prot = PAGE_EXECUTE_READWRITE;
  } else {
    LOGMAN_MSG_A_FMT("Unknown VirtualProtect options combination");
  }

  return ::VirtualProtect(Ptr, Size, prot, nullptr) == 0;
}

FEX_DEFAULT_VISIBILITY extern VirtualNamePtr VirtualName;
FEX_DEFAULT_VISIBILITY extern VirtualTHPPtr VirtualTHPControl;
#else
using MMAP_Hook = void* (*)(void*, size_t, int, int, int, off_t);
using MUNMAP_Hook = int (*)(void*, size_t);

FEX_DEFAULT_VISIBILITY extern MMAP_Hook mmap;
FEX_DEFAULT_VISIBILITY extern MUNMAP_Hook munmap;
FEX_DEFAULT_VISIBILITY extern void VirtualName(const char* Name, void* Ptr, size_t Size);

// All commit parameters are ignored here, they are unnecessary as Linux supports overcommit

inline void* VirtualAlloc(size_t Size, bool Execute = false, bool Commit = true) {
  return FEXCore::Allocator::mmap(nullptr, Size, PROT_READ | PROT_WRITE | (Execute ? PROT_EXEC : 0), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

inline void* VirtualAlloc(void* Base, size_t Size, bool Execute = false, bool Commit = true) {
  return FEXCore::Allocator::mmap(Base, Size, PROT_READ | PROT_WRITE | (Execute ? PROT_EXEC : 0), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

inline void VirtualFree(void* Ptr, size_t Size) {
  FEXCore::Allocator::munmap(Ptr, Size);
}
inline void VirtualDontNeed(void* Ptr, size_t Size, bool Recommit = true) {
  ::madvise(reinterpret_cast<void*>(Ptr), Size, MADV_DONTNEED);
}
inline bool VirtualProtect(void* Ptr, size_t Size, ProtectOptions options) {
  int prot {PROT_NONE};
  if ((options & ProtectOptions::Read) == ProtectOptions::Read) {
    prot |= PROT_READ;
  }
  if ((options & ProtectOptions::Write) == ProtectOptions::Write) {
    prot |= PROT_WRITE;
  }
  if ((options & ProtectOptions::Exec) == ProtectOptions::Exec) {
    prot |= PROT_EXEC;
  }

  return ::mprotect(Ptr, Size, prot) == 0;
}

inline void VirtualTHPControl(const void* Ptr, size_t Size, THPControl Control) {
#if defined(MADV_HUGEPAGE) && defined(MADV_NOHUGEPAGE)
  ::madvise(const_cast<void*>(Ptr), Size, Control == THPControl::Enable ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
#else
  // Darwin/iOS has no transparent-hugepage madvise advice; no-op.
  (void)Ptr;
  (void)Size;
  (void)Control;
#endif
}

#endif

// Memory allocation routines to be defined externally.
// This allows to use jemalloc for emulation while using the normal allocator
// for host tools without building FEXCore twice.
void* malloc(size_t size);
void* calloc(size_t n, size_t size);
void* memalign(size_t align, size_t s);
void* valloc(size_t size);
int posix_memalign(void** r, size_t a, size_t s);
void* realloc(void* ptr, size_t size);
void free(void* ptr);
size_t malloc_usable_size(void* ptr);
void* aligned_alloc(size_t a, size_t s);
void aligned_free(void* ptr);

FEX_DEFAULT_VISIBILITY extern void InitializeThread();

#ifndef _WIN32
void SetupAllocatorHooks(void* (*)(void* addr, size_t length, int prot, int flags, int fd, off_t offset), int (*)(void* addr, size_t length));
#endif

struct FEXAllocOperators {
  FEXAllocOperators() = default;

  void* operator new(size_t size) {
    return FEXCore::Allocator::malloc(size);
  }

  void* operator new(size_t size, std::align_val_t align) {
    return FEXCore::Allocator::aligned_alloc(static_cast<size_t>(align), size);
  }

  void operator delete(void* ptr) {
    return FEXCore::Allocator::free(ptr);
  }

  void operator delete(void* ptr, std::align_val_t align) {
    return FEXCore::Allocator::aligned_free(ptr);
  }
};
} // namespace FEXCore::Allocator
