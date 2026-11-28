// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <chrono>
#include "Interface/Context/Context.h"
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/SHMStats.h>
#include <FEXCore/Utils/WritePriorityMutex.h>

#include <FEXCore/fextl/map.h>
#include <FEXCore/fextl/memory_resource.h>
#include <FEXCore/fextl/robin_map.h>
#include <FEXCore/fextl/robin_set.h>
#include <FEXCore/fextl/vector.h>
#include <FEXCore/fextl/memory_resource.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stddef.h>
#include <utility>
#include <mutex>

#ifdef FEX_IOS_HOST
extern "C" uint32_t IosTebTsdOffset; // ARM64EC/Module.cpp, see Arm64Emitter.h
#endif

namespace FEXCore {
struct LookupCacheBaseLockToken {
protected:
  // Protected constructor - only derived classes can construct
  LookupCacheBaseLockToken() = default;
};

struct LookupCacheWriteLockToken : public LookupCacheBaseLockToken {
private:
  // Only constructible by GuestToHostMap
  friend struct GuestToHostMap;
  /* iOS-Madeira ml452 (#74): manual RAII instead of lock_guard so a token
   * constructed while this thread ALREADY write-owns the mutex no-ops (the
   * fault-handler re-entry chain: outer swap holds L'-write → nested compile
   * → nested swap re-acquires — the write→write self-park that stalled
   * ml449/ml451 runs).  Pairing is per-token: unlock iff this token locked. */
  /* ml453: nested-aware acquire DISABLED — the ml452 grant let the nested
   * emission reach its buffer-full branch, whose ClearCodeCache swap loops
   * against the outer emission's state: 196k tail refusals + 65k AV retries
   * in 2 minutes, phys to 4085MB (jetsam kill).  The self-park it replaced is
   * the lesser harm until the re-entry path (ra=0x1369f718c, task #69) gets a
   * nested-safe buffer-full strategy. */
  LookupCacheWriteLockToken(FEXCore::Utils::WritePriorityMutex::Mutex& Mutex)
    : LockRef {Mutex}
    , Locked {(Mutex.lock(), true)} {}
  FEXCore::Utils::WritePriorityMutex::Mutex& LockRef;
  bool Locked;

public:
  ~LookupCacheWriteLockToken() {
    if (Locked) {
      LockRef.unlock();
    }
  }
  LookupCacheWriteLockToken(const LookupCacheWriteLockToken&) = delete;
  LookupCacheWriteLockToken& operator=(const LookupCacheWriteLockToken&) = delete;
};

struct LookupCacheReadLockToken : public LookupCacheBaseLockToken {
private:
  // Only constructible by GuestToHostMap
  friend struct GuestToHostMap;
  LookupCacheReadLockToken(FEXCore::Utils::WritePriorityMutex::Mutex& Mutex)
    : Lock {Mutex} {}
  std::shared_lock<FEXCore::Utils::WritePriorityMutex::Mutex> Lock;
};

struct GuestToHostMap {
  FEXCore::Utils::WritePriorityMutex::Mutex Lock {};

  [[nodiscard]]
  LookupCacheWriteLockToken AcquireWriteLock() {
    return LookupCacheWriteLockToken {Lock};
  }

  [[nodiscard]]
  LookupCacheReadLockToken AcquireReadLock() {
    return LookupCacheReadLockToken {Lock};
  }

  struct BlockLinkTag {
    uint64_t GuestDestination;
    FEXCore::Context::ExitFunctionLinkData* HostLink;

    bool operator<(const BlockLinkTag& other) const {
      if (GuestDestination < other.GuestDestination) {
        return true;
      } else if (GuestDestination == other.GuestDestination) {
        return HostLink < other.HostLink;
      } else {
        return false;
      }
    }
  };

  // Use a monotonic buffer resource to allocate both the std::pmr::map and its members.
  // This allows us to quickly clear the block link map by clearing the monotonic allocator.
  // If we had allocated the block link map without the MBR, then clearing the map would require slowly
  // walking each block member and destructing objects.
  //
  // This makes `BlockLinks` look like a raw pointer that could memory leak, but since it is backed by the MBR, it won't.
  fextl::pmr::named_monotonic_page_buffer_resource BlockLinks_mbr;
  using BlockLinksMapType = std::pmr::map<BlockLinkTag, FEXCore::Context::BlockDelinkerFunc>;
  fextl::unique_ptr<std::pmr::polymorphic_allocator<std::byte>> BlockLinks_pma;
  BlockLinksMapType* BlockLinks;

  struct BlockEntry {
    uint64_t HostCode;
    fextl::vector<uint64_t> CodePages;
  };

  fextl::robin_map<uint64_t, BlockEntry> BlockList;

  fextl::map<uint64_t, fextl::vector<uint64_t>> CodePages;

  GuestToHostMap();

  // Adds to Guest -> Host code mapping
  const BlockEntry& AddBlockMapping(uint64_t Address, const fextl::vector<uint64_t>& CodePages, void* HostCode, const LookupCacheWriteLockToken&) {
    // This may replace an existing mapping
    // NOTE: Generally no previous entry should exist, however there is one exception:
    //       If the backend updates the active thread's CodeBuffer, the new associated LookupCache
    //       may already contain the block address. Since is comparatively rare, we'll just leak
    //       one of the two blocks in this case.
    return BlockList.insert_or_assign(Address, BlockEntry {(uintptr_t)HostCode, CodePages}).first->second;
  }

  const BlockEntry* FindBlock(uint64_t Address, const LookupCacheReadLockToken&) {
    auto HostCode = BlockList.find(Address);
    if (HostCode == BlockList.end()) {
      return nullptr;
    }
    return &HostCode->second;
  }

  bool Erase(uint64_t Address, const LookupCacheWriteLockToken&) {
    // Sever any links to this block
    auto lower = BlockLinks->lower_bound({Address, nullptr});
    auto upper = BlockLinks->upper_bound({Address, reinterpret_cast<FEXCore::Context::ExitFunctionLinkData*>(UINTPTR_MAX)});
    for (auto it = lower; it != upper; it = BlockLinks->erase(it)) {
      it->second(it->first.HostLink);
    }

    // Remove from BlockList
    return BlockList.erase(Address) != 0;
  }

  void InvalidateRange(uint64_t Start, uint64_t Length) {
    auto lk = AcquireWriteLock();

    auto lower = CodePages.lower_bound(Start >> 12);
    auto upper = CodePages.upper_bound((Start + Length - 1) >> 12);

    for (auto it = lower; it != upper; it++) {
      for (const auto& Entry : it->second) {
        Erase(Entry, lk);
      }
    }
    CodePages.erase(lower, upper);
  }

  void AddBlockLink(uint64_t GuestDestination, FEXCore::Context::ExitFunctionLinkData* HostLink,
                    const FEXCore::Context::BlockDelinkerFunc& delinker, const LookupCacheWriteLockToken&) {
    BlockLinks->insert({{GuestDestination, HostLink}, delinker});
  }

  bool AddBlockExecutableRange(const std::ranges::input_range auto& Addresses, uint64_t Start, uint64_t Length, const LookupCacheWriteLockToken&) {
    bool rv = false;

    for (auto CurrentPage = Start >> 12, EndPage = (Start + Length - 1) >> 12; CurrentPage <= EndPage; CurrentPage++) {
      auto& CodePage = CodePages[CurrentPage];
      rv |= CodePage.empty();
      CodePage.insert(CodePage.end(), Addresses.begin(), Addresses.end());
    }

    return rv;
  }

  void ClearCache(const LookupCacheWriteLockToken&);
};

class LookupCache {
public:
  struct LookupCacheEntry {
    uintptr_t HostCode;
    uintptr_t GuestCode;
  };

  /* MADEIRA: the L1 index is (rip ^ rip >> 17) & mask instead of rip & mask. 92% of x64 block entries
   * are 16-byte aligned, so the plain mask used 1/16 of the slots and most L1 misses were conflicts
   * between blocks that were already compiled (dig2 C2). Every site that indexes L1 must use the same
   * expression: the C++ lookups and fills below (L1Index), and the emitted lookups in Dispatcher.cpp and
   * BranchOps.cpp (L1HashEnabled at code-generation time). If an invalidation looked at a different slot
   * from the fill, a stale translation would survive in L1. MADEIRA_FEX_L1_HASH=0 restores the plain
   * mask; the variable is read once, the first time any of those sites asks. */
  static constexpr unsigned L1_HASH_SHIFT = 17;
  static bool L1HashEnabled() {
    static const bool Enabled = [] {
      const char* Env = getenv("MADEIRA_FEX_L1_HASH");
      const bool On = !(Env && Env[0] == '0');
      LogMan::Msg::EFmt("[l1-hash] index={} (MADEIRA_FEX_L1_HASH={})", On ? "rip^rip>>17" : "rip", Env ? Env : "unset");
      return On;
    }();
    return Enabled;
  }
  static uint64_t L1Index(uint64_t Address) {
    return L1HashEnabled() ? (Address ^ (Address >> L1_HASH_SHIFT)) : Address;
  }

  /* MADEIRA: MADEIRA_FEX_FREE_SKIP=0 turns off InvalidateCacheRange's cached-page bounds check (and,
   * in InvalidationTracker, the skip for frees of never-executable memory). Read once. */
  static bool FreeSkipEnabled() {
    static const bool Enabled = [] {
      const char* Env = getenv("MADEIRA_FEX_FREE_SKIP");
      return !(Env && Env[0] == '0');
    }();
    return Enabled;
  }

  LookupCache(FEXCore::Context::ContextImpl* CTX);
  ~LookupCache();

  // Swaps out the underlying GuestToHostMap and clears all associated caches.
  // This interface requires the previous CodeBuffer to be provided despite not using it. This ensures the shared write lock is still valid.
  void ChangeGuestToHostMapping([[maybe_unused]] CPU::CodeBuffer& Prev, GuestToHostMap& NewMap, const LookupCacheWriteLockToken& lk) {
    ClearThreadLocalCaches(lk);
    Shared = &NewMap;
  }

  uintptr_t FindBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t Address) {
    // Try L1, no lock needed
    auto& L1Entry = reinterpret_cast<LookupCacheEntry*>(L1Pointer)[L1Index(Address) & L1PointerMask];
    if (L1Entry.GuestCode == Address) {
      return L1Entry.HostCode;
    }
#ifdef FEX_IOS_HOST
    CountL1Miss();
#endif

    // L2 and L3 need to be locked
    uintptr_t HostPtr {};
    {
      std::optional<FEXCore::SHMStats::AccumulationBlock<uint64_t>> LockTime(
        Thread->ThreadStats ? &Thread->ThreadStats->AccumulatedCacheReadLockTime : nullptr);
      auto lk = Shared->AcquireReadLock();
      LockTime.reset();

      if (!DisableL2Cache()) {
        // Try L2
        const auto PageIndex = (Address & (VirtualMemSize - 1)) >> 12;
        const auto PageOffset = Address & (0x0FFF);

        const auto Pointers = reinterpret_cast<uintptr_t*>(PagePointer);
        auto LocalPagePointer = Pointers[PageIndex];

        // Do we a page pointer for this address?
        if (LocalPagePointer) {
          // Find there pointer for the address in the blocks
          auto BlockPointers = reinterpret_cast<LookupCacheEntry*>(LocalPagePointer);

          if (BlockPointers[PageOffset].GuestCode == Address) {
            L1Entry.GuestCode = Address;
            L1Entry.HostCode = BlockPointers[PageOffset].HostCode;
            HostPtr = L1Entry.HostCode;
          }
        }
      }

      if (!HostPtr) {
        // Try L3
        auto Entry = Shared->FindBlock(Address, lk);
        if (Entry) {
          CacheBlockMapping(Address, *Entry, false, lk);
          HostPtr = Entry->HostCode;
        }
      }
    }

    if (HostPtr && DynamicL1Cache()) {
      UpdateDynamicL1Stats(Thread);
    }

    FEXCORE_PROFILE_INSTANT_INCREMENT(Thread, AccumulatedCacheMissCount, 1);

    return HostPtr;
  }

  void UpdateDynamicL1Stats(FEXCore::Core::InternalThreadState* Thread) {
    // If host pointer was found in L2 or L3, then add it to the counter.
    // Keeping track not L1 misses, but specifically L2/L3 hits.
    ++L2L3CacheHits;
#ifdef FEX_IOS_HOST
    {   /* ml1124: process-wide L1-miss rate (every 2^20 misses, with the time) */
      static std::atomic<uint64_t> IosL1Misses {0};
      static std::atomic<int64_t> IosL1LastNs {0};
      const uint64_t M = IosL1Misses.fetch_add(1, std::memory_order_relaxed) + 1;
      if ((M & 0xfffff) == 0) {
        const int64_t Now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t Last = IosL1LastNs.exchange(Now, std::memory_order_relaxed);
        if (Last) LogMan::Msg::EFmt("[l1-miss] ml1124 {} L1 misses total, {:.0f}/s over the last 2^20", M, 1048576.0 * 1e9 / double(Now - Last));
      }
    }
#endif

#ifdef FEX_IOS_HOST
    /* ml1059: this runs on EVERY L1 miss, and system_clock::now() is a call through a
     * GetProcAddress'd kernelbase pointer. On iOS such a pointer names the image's
     * non-executable backing, so each call is an exec fault -> Mach exception ->
     * redirect to the pool copy: measured as the exception thread pinned at 32-54%
     * CPU and the hottest guest threads stalling a kernel round trip per lookup miss
     * (>= 1.38M redirects at this one pc in a 10-minute run). The heuristic only
     * needs a coarse rate, so look at the clock once per 256 hits; the hit COUNT
     * stays exact. */
    if (L2L3CacheHits & 0xff) return;
    IosReportMoves(Thread);
#endif
    const auto CurrentTime = std::chrono::system_clock::now();
    const auto Period = CurrentTime - LastPeriod;
    if (Period >= SamplePeriod) {
      // If larger than the sample period then check if we need to increase L1 cache size.
      const double AveragePerSecond = static_cast<double>(L2L3CacheHits) /
                                      static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(Period).count()) * 1000.0;

      if (AveragePerSecond >= DynamicL1CacheIncreaseCountHeuristic()) {
        if (CurrentL1Entries < MAX_L1_ENTRIES) {
          CurrentL1Entries <<= 1;
          L1PointerMask = CurrentL1Entries - 1;

          // Update the thread's L1 pointer mask to increase how much cache it uses.
          // Since we're in C-code, this is safe to update here.
          Thread->CurrentFrame->State.L1Mask = GetScaledL1PointerMask();
        }
      } else if (AveragePerSecond < DynamicL1CacheDecreaseCountHeuristic()) {
        if (CurrentL1Entries > MIN_L1_ENTRIES) {
          CurrentL1Entries >>= 1;
          L1PointerMask = CurrentL1Entries - 1;

          // Madvise the entries that we are dropping. Gives the memory back to the OS.
          LookupCacheEntry* FirstZeroL1Entry = &reinterpret_cast<LookupCacheEntry*>(L1Pointer)[CurrentL1Entries];
          size_t ZeroMemorySize = (MAX_L1_ENTRIES - CurrentL1Entries) * sizeof(LookupCacheEntry);
          FEXCore::Allocator::VirtualDontNeed(FirstZeroL1Entry, ZeroMemorySize, false);

          // Update the thread's L1 pointer mask to increase how much cache it uses.
          // Since we're in C-code, this is safe to update here.
          Thread->CurrentFrame->State.L1Mask = GetScaledL1PointerMask();
        }
      }

      // Update Last period to start again.
      LastPeriod = CurrentTime;
      L2L3CacheHits = 0;
    }
  }

  GuestToHostMap* Shared = nullptr;

  // Appends a list of Block {Address} to CodePages [Start, Start + Length)
  // Returns true if new pages are marked as containing code
  bool AddBlockExecutableRange(FEXCore::Core::InternalThreadState* Thread, const fextl::set<uint64_t>& Addresses, uint64_t Start, uint64_t Length) {
    std::optional<FEXCore::SHMStats::AccumulationBlock<uint64_t>> LockTime(
      Thread->ThreadStats ? &Thread->ThreadStats->AccumulatedCacheWriteLockTime : nullptr);
    auto lk = Shared->AcquireWriteLock();
    LockTime.reset();

    return Shared->AddBlockExecutableRange(Addresses, Start, Length, lk);
  }

  // Adds to Guest -> Host code mapping
  void AddBlockMapping(FEXCore::Core::InternalThreadState* Thread, uint64_t Address, const fextl::vector<uint64_t>& CodePages, void* HostCode) {
    std::optional<FEXCore::SHMStats::AccumulationBlock<uint64_t>> LockTime(
      Thread->ThreadStats ? &Thread->ThreadStats->AccumulatedCacheWriteLockTime : nullptr);
    auto lk = Shared->AcquireWriteLock();
    LockTime.reset();

    const auto& Entry = Shared->AddBlockMapping(Address, CodePages, HostCode, lk);

    // There is no need to update L1 or L2, they will get updated on first lookup
    // However, adding to L1 here increases performance
    CacheBlockMapping(Address, Entry, true, lk);
  }

  // Invalidates L1/L2 for a given guest block
  void InvalidateCache(uint64_t Address, const LookupCacheWriteLockToken& lk) {
    // Do L1
#if defined(_WIN32)
    // A dynamic L1 changes size, and an entry stays at the index the size of its time gave it: check
    // the index under every size up to the current one. Missed, the entry came back to life when the
    // cache returned to that size and sent the address to its old translation (HotSpot, rewriting
    // code it runs, failed one launch in three). Entries past the current size were decommitted when
    // the cache shrank.
    const uint64_t Index = L1Index(Address);
    for (size_t Entries = DynamicL1Cache() ? MIN_L1_ENTRIES : L1PointerMask + 1; Entries <= L1PointerMask + 1; Entries <<= 1) {
      auto& L1Entry = reinterpret_cast<LookupCacheEntry*>(L1Pointer)[Index & (Entries - 1)];
#else
    {
      auto& L1Entry = reinterpret_cast<LookupCacheEntry*>(L1Pointer)[L1Index(Address) & L1PointerMask];
#endif
      if (L1Entry.GuestCode == Address) {
        L1Entry.GuestCode = 0;
        // Leave L1Entry.HostCode as is, so that concurrent lookups won't read a null pointer
        // This is a soft guarantee for cross thread invalidation, as atomics are not used
        // and it hasn't been thoroughly tested
      }
    }

    if (!DisableL2Cache()) {
      // Do full map
      Address = Address & (VirtualMemSize - 1);
      uint64_t PageOffset = Address & (0x0FFF);
      Address >>= 12;

      uintptr_t* Pointers = reinterpret_cast<uintptr_t*>(PagePointer);
      uint64_t LocalPagePointer = Pointers[Address];
      if (!LocalPagePointer) {
        // Page for this code didn't even exist, nothing to do
        return;
      }

      // Page exists, just set the offset to zero
      auto BlockPointers = reinterpret_cast<LookupCacheEntry*>(LocalPagePointer);
      BlockPointers[PageOffset].GuestCode = 0;
      BlockPointers[PageOffset].HostCode = 0;
    }
  }

  // Invalidates all L1/L2 entries for all guest block that intersect the given range
  bool InvalidateCacheRange(uint64_t Start, uint64_t Length) {
    auto lk = Shared->AcquireWriteLock();

    if (Length == 0) {
      return false;
    }

    // CachedCodePages is a hash map (see its declaration), so there is no ordered range to erase.
    // Walk whichever is shorter: the pages of the range, or the map's entries.
    const uint64_t FirstPage = Start >> 12;
    const uint64_t LastPage = (Start + Length - 1) >> 12;
    bool ret = false;

    // MADEIRA: every thread's cache is walked for every invalidation, inside the exclusive
    // CodeInvalidationMutex section, and most ranges (guest frees of heap memory) are nowhere near the
    // pages this thread has cached. MinCachedPage/MaxCachedPage bound every page that was ever added
    // since the map was last empty, so a range outside them has nothing here.
    if (FreeSkipEnabled() && (CachedCodePages.empty() || LastPage < MinCachedPage || FirstPage > MaxCachedPage)) {
      return false;
    }

    if (LastPage - FirstPage < CachedCodePages.size()) {
      for (uint64_t Page = FirstPage; Page <= LastPage; Page++) {
        auto it = CachedCodePages.find(Page);
        if (it == CachedCodePages.end()) {
          continue;
        }
        for (const auto& Entry : it->second) {
          InvalidateCache(Entry, lk);
        }
        CachedCodePages.erase(it);
        ret = true;
      }
    } else {
      for (auto it = CachedCodePages.begin(); it != CachedCodePages.end();) {
        if (it->first < FirstPage || it->first > LastPage) {
          ++it;
          continue;
        }
        for (const auto& Entry : it->second) {
          InvalidateCache(Entry, lk);
        }
        it = CachedCodePages.erase(it);
        ret = true;
      }
    }
    if (CachedCodePages.empty()) {
      ResetCachedPageBounds();
    }
    return ret;
  }

  void AddBlockLink(uint64_t GuestDestination, FEXCore::Context::ExitFunctionLinkData* HostLink,
                    const FEXCore::Context::BlockDelinkerFunc& delinker, const LookupCacheWriteLockToken& lk) {
    Shared->AddBlockLink(GuestDestination, HostLink, delinker, lk);
  }

  void ClearCache(const LookupCacheWriteLockToken&);
  void ClearL2Cache(const LookupCacheBaseLockToken&);
  void ClearThreadLocalCaches(const LookupCacheWriteLockToken&);

  uintptr_t GetL1Pointer() const {
    return L1Pointer;
  }
  uintptr_t GetScaledL1PointerMask() const {
    return L1PointerMask << FEXCore::ilog2(sizeof(LookupCache::LookupCacheEntry));
  }
  uintptr_t GetPagePointer() const {
    // ml606: publish NULL when there is no L2 region. The JIT never loads
    // Pointers.L2Pointer in this mode (Dispatcher.cpp:279), so a null here is
    // inert — and if anything ever does dereference it, it faults at 0 rather
    // than wandering into whatever now lives at the old address.
    return L2Enabled ? PagePointer : 0;
  }
  uintptr_t GetVirtualMemorySize() const {
    return VirtualMemSize;
  }

  // This needs to be taken before reads or writes to L2, L3, CodePages,
  // and before writes to L1. Concurrent access from a thread that this LookupCache doesn't belong to
  // may only happen during cross thread invalidation (::Erase).
  // All other operations must be done from the owning thread.
  // Some care is taken so that L1 lookups can be done without locks, and even tearing is unlikely to lead to a crash.
  // This approach has not been fully vetted yet.
  // Also note that L1 lookups might be inlined in the JIT Dispatcher and/or block ends.
  auto AcquireWriteLock() {
    return Shared->AcquireWriteLock();
  }

private:
  void CacheBlockMapping(uint64_t Address, const GuestToHostMap::BlockEntry& Entry, bool L1Only, const LookupCacheBaseLockToken& lk) {
    for (const auto& CodePage : Entry.CodePages) {
      const uint64_t Page = CodePage >> 12;
      CachedCodePages[Page].insert(Address);
      MinCachedPage = std::min(MinCachedPage, Page);
      MaxCachedPage = std::max(MaxCachedPage, Page);
    }

    // Do L1
    auto& L1Entry = reinterpret_cast<LookupCacheEntry*>(L1Pointer)[L1Index(Address) & L1PointerMask];
    L1Entry.GuestCode = Address;
    L1Entry.HostCode = Entry.HostCode;

    if (!DisableL2Cache() && !L1Only) {
      // Do ful map
      auto FullAddress = Address;
      Address = Address & (VirtualMemSize - 1);

      uint64_t PageOffset = Address & (0x0FFF);
      Address >>= 12;

      uintptr_t* Pointers = reinterpret_cast<uintptr_t*>(PagePointer);
      uint64_t LocalPagePointer = Pointers[Address];
      if (!LocalPagePointer) {
        // We don't have a page pointer for this address
        // Allocate one now if we can
        uintptr_t NewPageBacking = AllocateBackingForPage();
        if (!NewPageBacking) {
          // Couldn't allocate, clear L2 and retry
          ClearL2Cache(lk);
          CacheBlockMapping(FullAddress, Entry, false, lk);
          return;
        }
        Pointers[Address] = NewPageBacking;
        LocalPagePointer = NewPageBacking;
      }

      // Add the new pointer to the page block
      auto BlockPointers = reinterpret_cast<LookupCacheEntry*>(LocalPagePointer);

      // This silently replaces existing mappings
      BlockPointers[PageOffset].GuestCode = FullAddress;
      BlockPointers[PageOffset].HostCode = Entry.HostCode;
    }
  }

  uintptr_t AllocateBackingForPage() {
    uintptr_t NewBase = AllocateOffset;
    uintptr_t NewEnd = AllocateOffset + SIZE_PER_PAGE;

    if (NewEnd >= CODE_SIZE) {
      // We ran out of block backing space. Need to clear the block cache and tell the JIT cores to clear their caches as well
      // Tell whatever is calling this that it needs to do it.
      return 0;
    }

    AllocateOffset = NewEnd;
    return PageMemory + NewBase;
  }

  // Maps from a page index to all blocks in the page that have at some point been fetched into L1/L2
  // MADEIRA: a hash map, not fextl::map. With L2 disabled (the iOS default) every L1 miss that L3
  // answers lands in CacheBlockMapping, and the tree descent here was 1.25% of all CPU in a capped
  // PoE2 profile (prof11, about 70k misses/s). InvalidateCacheRange no longer needs the ordering.
  fextl::robin_map<uint64_t, fextl::robin_set<uint64_t>> CachedCodePages;
  // MADEIRA: lowest and highest page added to CachedCodePages since it was last empty (see
  // InvalidateCacheRange). Only widened while entries exist, reset when the map empties.
  uint64_t MinCachedPage = UINT64_MAX;
  uint64_t MaxCachedPage = 0;
  void ResetCachedPageBounds() {
    MinCachedPage = UINT64_MAX;
    MaxCachedPage = 0;
  }

#ifdef FEX_IOS_HOST
  /* MADEIRA: per-thread L1 miss rate. Every C++ lookup that misses L1 counts (the emitted lookups
   * only come here after their own L1 probe missed), and about every 30 s the thread logs its rate.
   * The clock is read once per 4096 misses and is the counter register, not a library call (see
   * ml1059 below). */
  uint64_t IosL1Misses {};
  uint64_t IosL1MissesAtReport {};
  uint64_t IosL1ReportTicks {};
  void CountL1Miss() {
    if ((++IosL1Misses & 0xfff) != 0) {
      return;
    }
    uint64_t Now, Frequency;
    __asm volatile("mrs %0, cntvct_el0" : "=r"(Now));
    __asm volatile("mrs %0, cntfrq_el0" : "=r"(Frequency));
    if (!IosL1ReportTicks) {
      IosL1ReportTicks = Now;
      IosL1MissesAtReport = IosL1Misses;
      return;
    }
    const uint64_t Elapsed = Now - IosL1ReportTicks;
    if (Elapsed < Frequency * 30) {
      return;
    }
    // The TEB as the dispatcher finds it (x18 is not reliable on the iOS host): TPIDRRO_EL0 plus Wine's
    // published TSD offset, then ClientId.UniqueThread at +0x48, the tid the [thr-name] lines use.
    uint32_t Tid = 0;
    if (IosTebTsdOffset) {
      uint64_t Tpidrro;
      __asm volatile("mrs %0, TPIDRRO_EL0" : "=r"(Tpidrro));
      const uint64_t Teb = *reinterpret_cast<const uint64_t*>((Tpidrro & ~uint64_t(7)) + IosTebTsdOffset);
      Tid = Teb ? *reinterpret_cast<const uint32_t*>(Teb + 0x48) : 0;
    }
    LogMan::Msg::EFmt("[l1-miss-t] tid={:04x} {:.0f}/s over {:.1f}s, total={} l1-entries={} hash={}", Tid,
                      double(IosL1Misses - IosL1MissesAtReport) * double(Frequency) / double(Elapsed),
                      double(Elapsed) / double(Frequency), IosL1Misses, L1PointerMask + 1, L1HashEnabled() ? 1 : 0);
    IosL1ReportTicks = Now;
    IosL1MissesAtReport = IosL1Misses;
  }


  /* madeira-duo [move-t]: this thread's inline-copy counters (emitted code counts them when
   * MADEIRA_FEX_MOVE_CENSUS=1), about every 10 s while they move. Totals since the thread started. */
  uint64_t IosMoveReportTicks {};
  uint64_t IosMoveReported {};
  void IosReportMoves(FEXCore::Core::InternalThreadState* Thread) {
    const auto* Frame = Thread->CurrentFrame;
    const uint64_t Total = Frame->MadeiraMoveCount[0] + Frame->MadeiraMoveCount[1] + Frame->MadeiraMoveCount[2];
    if (Total == IosMoveReported) {
      return;
    }
    uint64_t Now, Frequency;
    __asm volatile("mrs %0, cntvct_el0" : "=r"(Now));
    __asm volatile("mrs %0, cntfrq_el0" : "=r"(Frequency));
    if (IosMoveReportTicks && Now - IosMoveReportTicks < Frequency * 10) {
      return;
    }
    IosMoveReportTicks = Now;
    IosMoveReported = Total;
    LogMan::Msg::EFmt("[move-t] tid={:04x} form={} copies/bytes since the thread started: at entry {}/{}, call site with push {}/{}, "
                      "call site without push {}/{}",
                      IosCurrentTid(), Frame->MadeiraMoveForm, Frame->MadeiraMoveCount[0], Frame->MadeiraMoveBytes[0],
                      Frame->MadeiraMoveCount[1], Frame->MadeiraMoveBytes[1], Frame->MadeiraMoveCount[2], Frame->MadeiraMoveBytes[2]);
  }

public:
  /* madeira-duo (20 Nov 2026): pool-copy alias redirects on this thread (Core.cpp alias-first and
   * ml315). About every 30 s the thread logs its rate and its heaviest PE targets, so the log names
   * which DLL's code the game reaches through alias addresses (map a target to its module with the
   * "Load module" lines). The target table is 16 slots, each a frequent-items counter: a different
   * target landing on a held slot takes a count away and replaces the holder only at zero, so a
   * target that dominates its slot stays. Thread-local state only; no atomics. */
  void IosNoteAliasRedirect(uint64_t PeRIP) {
    const uint32_t Slot = static_cast<uint32_t>((PeRIP >> 4) ^ (PeRIP >> 12)) & 15;
    if (IosAliasTarget[Slot] == PeRIP) {
      ++IosAliasHits[Slot];
    } else if (IosAliasHits[Slot] == 0) {
      IosAliasTarget[Slot] = PeRIP;
      IosAliasHits[Slot] = 1;
    } else {
      --IosAliasHits[Slot];
    }
    if ((++IosAliasRedirects & 0xfff) != 0) {
      return;
    }
    uint64_t Now, Frequency;
    __asm volatile("mrs %0, cntvct_el0" : "=r"(Now));
    __asm volatile("mrs %0, cntfrq_el0" : "=r"(Frequency));
    if (!IosAliasReportTicks) {
      IosAliasReportTicks = Now;
      IosAliasRedirectsAtReport = IosAliasRedirects;
      return;
    }
    const uint64_t Elapsed = Now - IosAliasReportTicks;
    if (Elapsed < Frequency * 30) {
      return;
    }
    const uint64_t Window = IosAliasRedirects - IosAliasRedirectsAtReport;
    uint32_t Order[16];
    for (uint32_t i = 0; i < 16; i++) {
      Order[i] = i;
    }
    for (uint32_t i = 0; i < 6; i++) {
      for (uint32_t j = i + 1; j < 16; j++) {
        if (IosAliasHits[Order[j]] > IosAliasHits[Order[i]]) {
          const uint32_t Swap = Order[i];
          Order[i] = Order[j];
          Order[j] = Swap;
        }
      }
    }
    LogMan::Msg::EFmt("[alias-redir] tid={:04x} {:.0f}/s over {:.1f}s, total={}; top PE targets (approximate counts): "
                      "{:#x} {}, {:#x} {}, {:#x} {}, {:#x} {}, {:#x} {}, {:#x} {}",
                      IosCurrentTid(), double(Window) * double(Frequency) / double(Elapsed), double(Elapsed) / double(Frequency),
                      IosAliasRedirects, IosAliasTarget[Order[0]], IosAliasHits[Order[0]], IosAliasTarget[Order[1]],
                      IosAliasHits[Order[1]], IosAliasTarget[Order[2]], IosAliasHits[Order[2]], IosAliasTarget[Order[3]],
                      IosAliasHits[Order[3]], IosAliasTarget[Order[4]], IosAliasHits[Order[4]], IosAliasTarget[Order[5]],
                      IosAliasHits[Order[5]]);
    for (uint32_t i = 0; i < 16; i++) {
      IosAliasHits[i] = 0;
      IosAliasTarget[i] = 0;
    }
    IosAliasReportTicks = Now;
    IosAliasRedirectsAtReport = IosAliasRedirects;
  }

private:
  uint64_t IosAliasRedirects {};
  uint64_t IosAliasRedirectsAtReport {};
  uint64_t IosAliasReportTicks {};
  uint64_t IosAliasTarget[16] {};
  uint64_t IosAliasHits[16] {};

  static uint32_t IosCurrentTid() {
    if (!IosTebTsdOffset) {
      return 0;
    }
    uint64_t Tpidrro;
    __asm volatile("mrs %0, TPIDRRO_EL0" : "=r"(Tpidrro));
    const uint64_t Teb = *reinterpret_cast<const uint64_t*>((Tpidrro & ~uint64_t(7)) + IosTebTsdOffset);
    return Teb ? *reinterpret_cast<const uint32_t*>(Teb + 0x48) : 0;
  }
#endif

  uintptr_t PagePointer;
  uintptr_t PageMemory;
  uintptr_t L1Pointer;
  uintptr_t L1PointerMask;

  size_t TotalCacheSize;

  // iOS-Madeira ml606: L1-ONLY LAYOUT when the L2 cache is disabled.
  //
  // The full layout is [L2 page table][CODE_SIZE arena][L1] contiguous — on iOS
  // 16MB + 32MB + 2MB = 50MB per guest thread, and FEX_IOS_HOST COMMITS it all
  // up front (the auto-commit-on-AV path doesn't work here). DisableL2Cache
  // defaults to TRUE and Dispatcher.cpp:279 emits `b(&NoBlock)` instead of any
  // L2 lookup when it is set, so on the shipping config those 48MB per thread
  // were allocated, committed and scrubbed but never read. ml605 had ~100 live
  // caches when it was jetsam-killed at 4080/4096MB with fex=1234MB dirty.
  //
  // L2Enabled is snapshotted ONCE here. Do not re-consult the config later:
  // Dispatcher and each LookupCache would otherwise sample it independently and
  // could disagree, which would emit an L2 lookup against an L1-only allocation.
  //
  // AllocationBase/AllocationSize describe what was ACTUALLY mapped, so the
  // destructor and the clear paths stay correct in both layouts rather than
  // operating on PagePointer/TotalCacheSize which only describe the full one.
  bool L2Enabled;
  uintptr_t AllocationBase;
  size_t AllocationSize;

  // Start with 8k entries in L1 to give 128KB of L1 cache to each thread.
  // Max out at 1 million entries to give each thread 16MB of L1 cache maximum.
  constexpr static size_t MIN_L1_ENTRIES = 8 * 1024;        // Must be a power of 2
#ifdef FEX_IOS_HOST
  /* iOS-Madeira ml363: cap the dynamic-L1 growth ceiling at 128K entries (2MB)
   * instead of 1M (16MB). A hot thread's L1 inserts hash-scatter across the
   * whole array, so a few thousand cached blocks dirty nearly every 16KB page
   * of whatever the ceiling allows — ml362 [phys-map] measured ~29MB dirty
   * per thread cache with ~40 guest threads ≈ 1GB against the 4096MB jetsam
   * limit, even with construction memsets removed (zero-scrub stale=0 on all
   * 42 threads). 2MB bounds that at ~80MB fleet-wide; hot threads fall back
   * to L2 more often (hit rate was 77% at the 8K MINIMUM, so the ceiling cut
   * is expected to cost little). Also shrinks each per-thread allocation by
   * 14MB ([TI-IC] lookupcache-alloc size drops 0x6000000 → 0x5200000, which
   * doubles as the deploy marker). */
  constexpr static size_t MAX_L1_ENTRIES = 128 * 1024; // Must be a power of 2
#else
  constexpr static size_t MAX_L1_ENTRIES = 1 * 1024 * 1024; // Must be a power of 2
#endif

#ifdef FEX_IOS_HOST
  /* iOS-Madeira: halve the per-thread block-backing arena — it's committed
   * upfront on iOS (see LookupCache.cpp) and 128MB × ~19 threads was OOM-ing
   * the process. 64MB still backs 1024 guest code pages per thread; on
   * overflow AllocateBackingForPage returns 0 → clean ClearCodeCache.
   *
   * ml387: halved again 64→32MB — VA, not footprint, is now the wall: the
   * guest band hit 38MB free with ~50 live lookupcaches at 82MB each
   * (0x5200000). 32MB still backs 512 guest code pages per thread; overflow
   * remains a clean ClearCodeCache. New [TI-IC] lookupcache-alloc marker:
   * 0x3200000. */
  constexpr static size_t CODE_SIZE = 32 * 1024 * 1024;
#else
  constexpr static size_t CODE_SIZE = 128 * 1024 * 1024;
#endif
  constexpr static size_t SIZE_PER_PAGE = FEXCore::Utils::FEX_PAGE_SIZE * sizeof(LookupCacheEntry);
  constexpr static size_t MAX_L1_SIZE = MAX_L1_ENTRIES * sizeof(LookupCacheEntry);

  size_t AllocateOffset {};

  FEXCore::Context::ContextImpl* ctx;
  uint64_t VirtualMemSize {};

  size_t CurrentL1Entries = MIN_L1_ENTRIES;
  uint64_t L2L3CacheHits {};
  std::chrono::time_point<std::chrono::system_clock> LastPeriod {};
  constexpr static std::chrono::seconds SamplePeriod {1};
  FEX_CONFIG_OPT(DynamicL1CacheIncreaseCountHeuristic, DYNAMICL1CACHEINCREASECOUNTHEURISTIC);
  FEX_CONFIG_OPT(DynamicL1CacheDecreaseCountHeuristic, DYNAMICL1CACHEDECREASECOUNTHEURISTIC);

  FEX_CONFIG_OPT(DynamicL1Cache, DYNAMICL1CACHE);
  FEX_CONFIG_OPT(DisableL2Cache, DISABLEL2CACHE);
};
} // namespace FEXCore
