// SPDX-License-Identifier: MIT
#pragma once

#include <FEXCore/Utils/IntervalList.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <string_view>

namespace FEXCore::Core {
struct InternalThreadState;
}

namespace FEXCore::Context {
class Context;
}

namespace FEX::Windows {
/**
 * @brief Handles SMC and regular code invalidation
 */
class InvalidationTracker {
public:
#if !defined(ARCHITECTURE_arm64ec)
  // Guest window (WOW64 module on the iOS host): address namespaces.
  //
  // Every address this class stores, queries and hands to the OS (VirtualQuery,
  // NtQueryVirtualMemory, NtProtectVirtualMemory) is a HOST address: its callers - wow64.dll's
  // BTCpuNotify* callbacks, the exception path's fault address, the image-map notifications - all
  // speak host addresses, and the interval bookkeeping has to line up with what the OS reports.
  // FEXCore keys code invalidation, the lookup caches and executable-range queries on GUEST
  // addresses. So the conversion happens at the calls into FEXCore (InvalidateIntervalInternalLocked,
  // DetectMonoBackpatcherBlock) using GuestBase, and in the WOW64 module's SyscallHandler overrides.
  //
  // GuestBase is 0 when identity mapped, which makes all of this a no-op. The ARM64EC module does
  // not compile any of it.
  InvalidationTracker(FEXCore::Context::Context& CTX, const std::unordered_map<DWORD, FEXCore::Core::InternalThreadState*>& Threads,
                      uint64_t GuestBase = 0);
#else
  InvalidationTracker(FEXCore::Context::Context& CTX, const std::unordered_map<DWORD, FEXCore::Core::InternalThreadState*>& Threads);
#endif
  void HandleMemoryProtectionNotification(uint64_t Address, uint64_t Size, ULONG Prot);
  void HandleProcessExecuteFlagsChange(ULONG Flags);
  void HandleImageMap(std::string_view Name, uint64_t Address);
  struct InvalidateContainingSectionResult {
    uint64_t SectionStart;
    uint64_t SectionSize;
  };
  InvalidateContainingSectionResult InvalidateContainingSection(uint64_t Address, bool Free);
  void InvalidateAlignedInterval(uint64_t Address, uint64_t Size, bool Free);
  void ReprotectRWXIntervals(uint64_t Address, uint64_t Size);
  bool HandleRWXAccessViolation(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC, uint64_t FaultAddress);

  // Unprotects any RWX intervals in the input interval and invalidates code
  // NOTE: CodeInvalidationMutex must be locked when calling this, and if true is returned, kept locked until the write ends.
  bool BeginUntrackedWriteLocked(uint64_t Address, uint64_t Size);

  FEXCore::HLE::ExecutableRangeInfo QueryExecutableRange(uint64_t Address);

private:
#if !defined(ARCHITECTURE_arm64ec)
  // Lazy DEP-off promotion under a guest window: registers [Address's committed region] as
  // executable because DEP is off, called only from QueryExecutableRange, i.e. only when the guest
  // actually tries to execute there. Returns a zero-size interval when the address is not a
  // committed, readable, non-executable page inside the window (a genuine wild branch, which must
  // still fault). NOTE: IntervalsLock must be held exclusively.
  FEXCore::IntervalList<uint64_t>::Interval PromoteDEPRegionLocked(uint64_t Address);
  // The interval-list half of QueryExecutableRange. NOTE: IntervalsLock must be held.
  FEXCore::HLE::ExecutableRangeInfo QueryExecutableRangeLocked(uint64_t Address);
#endif

  void DetectMonoBackpatcherBlock(FEXCore::Core::InternalThreadState* Thread, uint64_t HostPC);
  void DisableSMCDetection();
  void InvalidateIntervalInternal(uint64_t Address, uint64_t Size);
  // NOTE: This assumed CodeInvalidationMutex is locked by the caller
  void InvalidateIntervalInternalLocked(uint64_t Address, uint64_t Size);

  // NOTE: If ForWriteLocked is true then this assumes CodeInvalidationMutex is locked by the caller,
  // and any code in the range will be invalidated before protection as RWX, otherwise protects as RX if false.
  bool ProtectRWXIntervalsInternal(uint64_t Address, uint64_t Size, bool ForWriteLocked);

  // Returns the correct protection for trapping (removing write) or untrapping (restoring write) an RWX interval.
  // For DEP-promoted regions (originally non-exec), uses PAGE_READONLY/PAGE_READWRITE instead of PAGE_EXECUTE_READ/PAGE_EXECUTE_READWRITE.
  // NOTE: Must be called with IntervalsLock held.
  ULONG GetTrapProt(uint64_t Address) const;
  ULONG GetUntrapProt(uint64_t Address) const;

  FEXCore::IntervalList<uint64_t> XIntervals;
  FEXCore::IntervalList<uint64_t> RWXIntervals;
  // MADEIRA: a std::shared_mutex that remembers which thread holds it exclusively. Anything that
  // grows or shrinks FEX's own heap under the lock (a log line, an interval insert) comes back on
  // the same thread through the memory notifications (HandleMemoryProtectionNotification,
  // InvalidateAlignedInterval, InvalidateContainingSection); they check HeldByThisThread() and
  // return instead of waiting on a lock that is not recursive.
  class OwnedSharedMutex {
  public:
    void lock() {
      Mutex.lock();
      Owner.store(Self(), std::memory_order_relaxed);
    }
    bool try_lock() {
      if (!Mutex.try_lock()) {
        return false;
      }
      Owner.store(Self(), std::memory_order_relaxed);
      return true;
    }
    void unlock() {
      Owner.store(0, std::memory_order_relaxed);
      Mutex.unlock();
    }
    void lock_shared() {
      Mutex.lock_shared();
    }
    bool try_lock_shared() {
      return Mutex.try_lock_shared();
    }
    void unlock_shared() {
      Mutex.unlock_shared();
    }
    // Only the owning thread can find its own identity here, so relaxed accesses suffice.
    bool HeldByThisThread() const {
      return Owner.load(std::memory_order_relaxed) == Self();
    }
  private:
    static uint64_t Self();
    std::shared_mutex Mutex;
    std::atomic<uint64_t> Owner {0};
  };
  OwnedSharedMutex IntervalsLock;
  FEXCore::Context::Context& CTX;
  const std::unordered_map<DWORD, FEXCore::Core::InternalThreadState*>& Threads;
#if !defined(ARCHITECTURE_arm64ec)
  const uint64_t GuestBase {0}; // Host address of guest address 0, or 0 when identity mapped
#endif
  bool SMCDetectionDisabled {false};                    // Protected by IntervalsLock
  bool DEPDisabled {false};                             // Protected by IntervalsLock
  FEXCore::IntervalList<uint64_t> DEPPromotedIntervals; // Protected by IntervalsLock

  bool MonoBackpatcherDetectionPending {false};
  uint64_t MonoBase {0};
  uint64_t MonoEnd {0};
};
} // namespace FEX::Windows
