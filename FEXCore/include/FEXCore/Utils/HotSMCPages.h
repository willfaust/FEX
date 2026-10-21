// SPDX-License-Identifier: MIT
#pragma once
/* Hot SMC pages (FEX_HOT_SMC_PAGES, off by default).
 *
 * An anti-tamper protector (seen in Dark Souls III) rewrites a few fixed dwords inside .text every frame. With
 * the default SMC tracking each write traps on the write-protected page, every translation on the
 * 4 KB page is thrown away (5-12 recompiles per trap) and every thread that cached code from the
 * page resets its call-ret stack.
 *
 * After a page has trapped Threshold times within Window, it is marked hot:
 *   - the frontend no longer re-arms its write trap (InvalidationTracker::ProtectRWXIntervalsInternal),
 *     so the guest keeps the RWX protection it asked for and its writes no longer reach FEX;
 *   - every guest instruction on the page is compiled with the existing SMCChecks=full byte check
 *     (Core.cpp GenerateIR, the same IR the Mono backpatcher uses per block): the instruction's bytes
 *     are compared against the copy taken at compile time, and on a mismatch the block's entry is
 *     invalidated (ThreadRemoveCodeEntry) and execution resumes at that instruction, recompiled.
 *
 * Marking happens with CodeInvalidationMutex held exclusively and is followed by an invalidation
 * of the page, so every block that can run from a hot page was compiled after it became hot and
 * carries the checks. Pages are never unmarked: a hot page stays correct by its checks alone,
 * whatever later happens to the memory.
 *
 * Off unless FEX_HOT_SMC_PAGES=1 (read once by the ARM64EC InvalidationTracker).
 *
 * v2 (FEX_HOT_SMC_PAGES=2, "hot write windows"): v1 measured 0 check failures on 11,069 checked
 * instructions, i.e. the protector's dwords are never executed code, so checking whole pages was pure cost.
 *   - While a page is counted, the 16-byte block of every trapped write address is recorded (<= MaxWindows).
 *     A page whose writes spread over more blocks, or whose Threshold traps came within MinSpan (a one-shot
 *     decrypt burst), is not marked.
 *   - Only instructions overlapping a recorded window [Base, Base+WindowSize) get the byte check.
 *   - The write trap stays armed as in stock FEX (re-armed on every compile from the page, plus a periodic
 *     audit re-arm of all hot pages). A trap at a KNOWN window is a fast trap: no invalidation, only the
 *     un-protect. A trap at an unknown address on a hot page learns the window and invalidates the page as
 *     stock FEX does, so its blocks recompile with the new check.
 *   Residual hole: after a fast trap the page (and, through Wine's 16 KB
 *   host page, its three 4 KB siblings) stays writable until the next compile from it or the next audit; a
 *   write to an unknown address in that interval is not seen. Stock FEX has the same interval for the
 *   siblings after every un-protect; v2 adds it for the hot page itself, bounded by AuditTicks. */
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace FEXCore::HotSMC {
constexpr size_t MaxPages = 256;
constexpr size_t MaxWindows = 8;         // v2: recorded write windows per hot page
constexpr uint64_t WindowAlign = 16;     // v2: a window starts at the 16-byte block of the write address
constexpr uint64_t WindowSize = 32;      // v2: and covers 32 bytes, so a store of <= 16 bytes from that block fits

struct State {
  std::atomic<bool> Enabled {false};
  uint32_t Mode {0};          // 1 = v1 (whole hot page checked, trap not re-armed), 2 = v2 (windows)
  uint64_t MinSpanTicks {0};  // v2: Threshold traps must take at least this long
  uint64_t AuditTicks {0};    // v2: re-arm the trap on every hot page this often (0 = never)
  uint32_t Threshold {8};
  uint64_t WindowTicks {0}; // 0 = no window (count since start)
  uint64_t ReportTicks {0}; // 0 = no periodic report

  // Insert-only list of hot page bases (4 KB aligned), Count is the number of published slots.
  std::atomic<uint32_t> Count {0};
  std::atomic<uint64_t> Pages[MaxPages] {};
  // v2: per hot slot, recorded window bases (0 = empty). Written only under the exclusive
  // CodeInvalidationMutex and followed by an invalidation of the page; read by compiles (shared lock).
  std::atomic<uint64_t> Windows[MaxPages][MaxWindows] {};

  // Counters for the periodic [hot-smc] line.
  std::atomic<uint64_t> PagesMarkedHot {0};
  std::atomic<uint64_t> MarkRefused {0};       // table full or page refused
  std::atomic<uint64_t> TrapsSeen {0};         // RWX write traps handled while enabled
  std::atomic<uint64_t> TrapsAvoided {0};      // write-trap re-arms skipped on hot pages
  std::atomic<uint64_t> BlocksValidated {0};   // compiled blocks containing checked instructions
  std::atomic<uint64_t> EntryChecksCompiled {0}; // instructions compiled with a byte check
  std::atomic<uint64_t> EntryMismatches {0};   // checks that failed at run time (block dropped)
  std::atomic<uint64_t> LastReport {0};
  // v2 counters
  std::atomic<uint64_t> FastTraps {0};       // traps at a known window: no invalidation
  std::atomic<uint64_t> NewWindowTraps {0};  // traps on a hot page at an unknown address (window learned, page invalidated)
  std::atomic<uint64_t> WindowOverflow {0};  // as above but the page already had MaxWindows (plain invalidation)
  std::atomic<uint64_t> WindowsLearned {0};
  std::atomic<uint64_t> RefusedBurst {0};    // threshold reached faster than MinSpan
  std::atomic<uint64_t> RefusedWide {0};     // writes spread over more than MaxWindows blocks
  std::atomic<uint64_t> Audits {0};
  std::atomic<uint64_t> LastAudit {0};
  void (*AuditFn)(void*) {nullptr};
  void* AuditCtx {nullptr};
};

inline State g_State;

inline uint64_t Ticks() {
#if defined(__arm64ec__) || defined(__aarch64__)
  uint64_t V;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(V));
  return V;
#else
  return 0;
#endif
}

inline uint64_t TickFrequency() {
#if defined(__arm64ec__) || defined(__aarch64__)
  uint64_t V;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(V));
  return V ? V : 24000000;
#else
  return 0;
#endif
}

inline bool IsEnabled() {
  return g_State.Enabled.load(std::memory_order_relaxed);
}

inline bool AnyHot() {
  return IsEnabled() && g_State.Count.load(std::memory_order_acquire) != 0;
}

inline bool IsV2() {
  return g_State.Mode == 2;
}

// Hot slot index of PageBase, or -1.
inline int FindSlot(uint64_t PageBase) {
  const uint32_t N = g_State.Count.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < N; ++i) {
    if (g_State.Pages[i].load(std::memory_order_relaxed) == PageBase) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

inline bool IsHotPage(uint64_t PageBase) {
  return FindSlot(PageBase) >= 0;
}

inline uint64_t WindowBase(uint64_t WriteAddress) {
  return WriteAddress & ~(WindowAlign - 1);
}

// v2: true if the write at WriteAddress falls in a window already recorded for its page's slot.
inline bool WindowKnown(int Slot, uint64_t WriteAddress) {
  const uint64_t Base = WindowBase(WriteAddress);
  for (size_t w = 0; w < MaxWindows; ++w) {
    if (g_State.Windows[Slot][w].load(std::memory_order_relaxed) == Base) {
      return true;
    }
  }
  return false;
}

// v2: records the window of WriteAddress. NOTE: CodeInvalidationMutex must be held exclusively, and the
// page invalidated afterwards under the same lock. False if the slot is full.
inline bool AddWindowLocked(int Slot, uint64_t WriteAddress) {
  const uint64_t Base = WindowBase(WriteAddress);
  for (size_t w = 0; w < MaxWindows; ++w) {
    const uint64_t V = g_State.Windows[Slot][w].load(std::memory_order_relaxed);
    if (V == Base) {
      return true;
    }
    if (V == 0) {
      g_State.Windows[Slot][w].store(Base, std::memory_order_release);
      g_State.WindowsLearned.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

// v2: true if [Start, End) overlaps a recorded window. A window may reach 16 bytes past its page,
// so the slot of the page before Start is consulted too.
inline bool RangeTouchesWindow(uint64_t Start, uint64_t End) {
  const uint64_t First = (Start - WindowSize) & FEXCore::Utils::FEX_PAGE_MASK;
  for (uint64_t P = First; P < End; P += FEXCore::Utils::FEX_PAGE_SIZE) {
    const int Slot = FindSlot(P);
    if (Slot < 0) {
      continue;
    }
    for (size_t w = 0; w < MaxWindows; ++w) {
      const uint64_t Base = g_State.Windows[Slot][w].load(std::memory_order_acquire);
      if (Base && Base < End && Start < Base + WindowSize) {
        return true;
      }
    }
  }
  return false;
}

// True if any byte of [Start, End) lies on a hot page.
inline bool RangeTouchesHot(uint64_t Start, uint64_t End) {
  if (!AnyHot() || End <= Start) {
    return false;
  }
  for (uint64_t P = Start & FEXCore::Utils::FEX_PAGE_MASK; P < End; P += FEXCore::Utils::FEX_PAGE_SIZE) {
    if (IsHotPage(P)) {
      return true;
    }
  }
  return false;
}

// Compile-time decision for one guest instruction: v1 checks every instruction on a hot page,
// v2 only those overlapping a recorded write window.
inline bool InstructionNeedsCheck(uint64_t Start, uint64_t End) {
  if (!AnyHot() || End <= Start) {
    return false;
  }
  return IsV2() ? RangeTouchesWindow(Start, End) : RangeTouchesHot(Start, End);
}

// NOTE: CodeInvalidationMutex must be held exclusively, and the page invalidated afterwards under the same lock.
// v2: Windows/NumWindows are recorded into the new slot before it is published.
inline bool MarkHotLocked(uint64_t PageBase, const uint64_t* Windows = nullptr, uint32_t NumWindows = 0) {
  if (IsHotPage(PageBase)) {
    return true;
  }
  const uint32_t N = g_State.Count.load(std::memory_order_relaxed);
  if (N >= MaxPages) {
    g_State.MarkRefused.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_State.Pages[N].store(PageBase, std::memory_order_relaxed);
  for (size_t w = 0; w < MaxWindows; ++w) {
    g_State.Windows[N][w].store(w < NumWindows ? Windows[w] : 0, std::memory_order_relaxed);
  }
  g_State.WindowsLearned.fetch_add(NumWindows, std::memory_order_relaxed);
  g_State.Count.store(N + 1, std::memory_order_release);
  g_State.PagesMarkedHot.fetch_add(1, std::memory_order_relaxed);
  return true;
}

inline void Report(const char* Why) {
  auto& S = g_State;
  LogMan::Msg::EFmt("[hot-smc] {} pages_marked_hot={} entry_checks_compiled={} entry_mismatches={} traps_avoided={} | traps_seen={} "
                    "blocks_validated={} refused={}",
                    Why, S.PagesMarkedHot.load(std::memory_order_relaxed), S.EntryChecksCompiled.load(std::memory_order_relaxed),
                    S.EntryMismatches.load(std::memory_order_relaxed), S.TrapsAvoided.load(std::memory_order_relaxed),
                    S.TrapsSeen.load(std::memory_order_relaxed), S.BlocksValidated.load(std::memory_order_relaxed),
                    S.MarkRefused.load(std::memory_order_relaxed));
  if (IsV2()) {
    LogMan::Msg::EFmt("[hot-smc] v2 {} fast_traps={} new_window_traps={} window_overflow={} windows={} refused_burst={} refused_wide={} "
                      "audits={}",
                      Why, S.FastTraps.load(std::memory_order_relaxed), S.NewWindowTraps.load(std::memory_order_relaxed),
                      S.WindowOverflow.load(std::memory_order_relaxed), S.WindowsLearned.load(std::memory_order_relaxed),
                      S.RefusedBurst.load(std::memory_order_relaxed), S.RefusedWide.load(std::memory_order_relaxed),
                      S.Audits.load(std::memory_order_relaxed));
  }
}

// v2: periodic re-arm of the write trap on every hot page. Call only where no FEX lock is held.
// Re-arming is always safe (it can only add traps), so it does not take CodeInvalidationMutex.
inline void MaybeAudit() {
  if (!IsEnabled() || !IsV2() || !g_State.AuditTicks || !g_State.AuditFn || !AnyHot()) {
    return;
  }
  const uint64_t Now = Ticks();
  uint64_t Last = g_State.LastAudit.load(std::memory_order_relaxed);
  if (Now - Last < g_State.AuditTicks) {
    return;
  }
  if (!g_State.LastAudit.compare_exchange_strong(Last, Now, std::memory_order_relaxed)) {
    return;
  }
  g_State.Audits.fetch_add(1, std::memory_order_relaxed);
  g_State.AuditFn(g_State.AuditCtx);
}

// Periodic line. Call only where no FEX lock is held (logging may allocate).
inline void MaybeReport() {
  if (!IsEnabled() || !g_State.ReportTicks) {
    return;
  }
  const uint64_t Now = Ticks();
  uint64_t Last = g_State.LastReport.load(std::memory_order_relaxed);
  if (Now - Last < g_State.ReportTicks) {
    return;
  }
  if (!g_State.LastReport.compare_exchange_strong(Last, Now, std::memory_order_relaxed)) {
    return;
  }
  Report("stats");
}

inline void Periodic() {
  MaybeAudit();
  MaybeReport();
}
} // namespace FEXCore::HotSMC
