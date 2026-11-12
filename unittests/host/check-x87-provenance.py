#!/usr/bin/env python3
"""Execute the pass's provenance emission against a small host context model."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'FEXCore/Source/Interface/IR/Passes/x87StackOptimizationPass.cpp').read_text()
pos = source.index('void X87StackOptimization::FlushCachedRegs()')
start = source.index('  auto Size = ReducedPrecisionMode ? OpSize::i64Bit : OpSize::i128Bit;', pos)
end = source.index('  if (FlushValidPending)', start)
flush = source[start:end]
start = source.index('inline std::pair<Ref, Ref> X87StackOptimization::LoadExactInt_Slow(')
end = source.index('\n\ninline Ref X87StackOptimization::LoadStackValueAtOffset_Slow', start)
load_exact = source[start:end].replace('inline std::pair<Ref, Ref> X87StackOptimization::', 'static std::pair<Ref, Ref> ')
pos = source.index('case OP_READSTACKINT64:')
start = source.index('        } else {\n          auto [Exact, Valid] = LoadExactInt_Slow(0);', pos)
end = source.index('\n\n        IREmit->ReplaceUsesWithAfter', start)
read = source[start:end]
pos = source.index('case OP_COPYPUSHSTACK:')
start = source.index('          Ref St0 = LoadStackValueAtOffset_Slow(Offset);', pos)
end = source.index('\n        } else {', start)
copy = source[start:end]
pos = source.index('case OP_STORESTACKTOSTACK:')
start = source.index('            Ref Value = LoadStackValueAtOffset_Slow();', pos)
end = source.index('\n          } else {', start)
store_copy = source[start:end]
pos = source.index('case OP_F80STACKXCHANGE:')
start = source.index('          Ref ValueTop = LoadStackValue();', pos)
end = source.index('\n        } else {', start)
swap = source[start:end]
prelude = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
namespace Core { struct CPUState { uint64_t mm[8][2]{}; uint64_t x87_exact_int[8][2]{}; uint8_t x87_exact_valid{}; uint8_t ftw{}; }; }
struct Node { uint64_t value; };
using Ref = Node*; using OrderedNode = Node;
static Node* node(uint64_t v) { return new Node{v}; }
enum class OpSize { i8Bit, i32Bit, i64Bit, i128Bit };
enum class MemOffsetType { SXTX };
enum class CondClass { NEQ };
static unsigned physical_top;
static Core::CPUState state;
static uint32_t MMBaseOffset() { return offsetof(Core::CPUState, mm); }
struct Emitter {
  Ref _InlineConstant(uint64_t x) { return node(x); }
  Ref _LoadContextGPR(OpSize, size_t off) { return node(*((uint8_t*)&state + off)); }
  void _StoreContextGPR(OpSize, Ref v, size_t off) { *((uint8_t*)&state + off) = v->value; }
  Ref _Lshl(OpSize, Ref a, Ref b) { return node((uint32_t)a->value << b->value); }
  Ref _Lshr(OpSize, Ref a, Ref b) { return node((uint32_t)a->value >> b->value); }
  Ref _Andn(OpSize, Ref a, Ref b) { return node(a->value & ~b->value); }
  Ref _And(OpSize, Ref a, Ref b) { return node(a->value & b->value); }
  Ref _Or(OpSize, Ref a, Ref b) { return node(a->value | b->value); }
  void _StoreMemHostFPR(OpSize, Ref v, Ref a, Ref off, OpSize, MemOffsetType, int) { memcpy((void*)(a->value + off->value), &v->value, 8); }
  Ref _LoadMemHostGPR(OpSize, Ref a, Ref off, OpSize, MemOffsetType, int) { uint64_t v; memcpy(&v, (void*)(a->value + off->value), 8); return node(v); }
  Ref _LoadMemHostFPR(OpSize s, Ref a, Ref o, OpSize al, MemOffsetType t, int k) { return _LoadMemHostGPR(s, a, o, al, t, k); }
  Ref _VExtractToGPR(OpSize, OpSize, Ref v, int) { return v; }
  Ref _VCastFromGPR(OpSize, OpSize, Ref v) { return v; }
  Ref _Select(OpSize, OpSize, CondClass, Ref cond, Ref zero, Ref yes, Ref no) { return cond->value != zero->value ? yes : no; }
};
static Emitter emitter, *IREmit = &emitter;
static bool ReducedPrecisionMode = true;
static std::array<bool, 8> FlushValuesPending;
static Ref TopValueCache[8], TopInt64Cache[8], TopInt64ValidCache[8];
static Ref GetConstant(size_t n) { return node(n); }
static Ref GetOffsetTopWithCache_Slow(unsigned off) { return node((physical_top + off) & 7); }
static OrderedNode* GetOffsetTopAddressWithCache_Slow(unsigned off) { return node((uintptr_t)&state + ((physical_top + off) & 7)*16); }
static Ref GetX87ValidTag_Slow(unsigned off) { return node((state.ftw >> ((physical_top + off) & 7)) & 1); }
static Ref LoadStackValueAtOffset_Slow(unsigned off = 0) {
  return TopValueCache[off] ? TopValueCache[off] : node(state.mm[(physical_top + off) & 7][0]);
}
static Ref LoadStackValue(unsigned off = 0) { return LoadStackValueAtOffset_Slow(off); }
static void UpdateTopForPush_Slow() {
  physical_top = (physical_top + 7) & 7;
  std::rotate(TopValueCache, TopValueCache + 7, TopValueCache + 8);
  std::rotate(TopInt64Cache, TopInt64Cache + 7, TopInt64Cache + 8);
  std::rotate(TopInt64ValidCache, TopInt64ValidCache + 7, TopInt64ValidCache + 8);
  std::rotate(FlushValuesPending.begin(), FlushValuesPending.begin() + 7, FlushValuesPending.end());
  state.ftw |= (1 << physical_top);
}
static void StoreStackValueAtOffset_Slow(Ref v, unsigned off = 0) {
  TopValueCache[off] = v; TopInt64Cache[off] = nullptr; TopInt64ValidCache[off] = nullptr; FlushValuesPending[off] = true;
}
static void StoreStackValue(Ref v, unsigned off = 0) { StoreStackValueAtOffset_Slow(v, off); }

'''
middle = r'''
}
static void copy_push(unsigned Offset) {
'''
copy_end = r'''
}
static void store_copy(unsigned Offset) {
'''
store_end = r'''
}
static void swap_slots(unsigned Offset) {
'''
swap_end = r'''
}
static uint64_t read(uint64_t converted) {
  Ref Result = node(converted);
  if (false) {
'''
main = r'''
  return Result->value;
}
int main() {
  const uint64_t exact = 0x0020000000000001ULL, rounded = 0x0020000000000000ULL;
  physical_top = 7; state.ftw = 0x80; state.mm[7][1] = 0xfeedface;
  TopValueCache[0] = node(0x4340000000000000ULL); TopInt64Cache[0] = node(exact); TopInt64ValidCache[0] = node(1); FlushValuesPending[0] = true;
  flush(); assert(state.x87_exact_valid == 0x80 && state.mm[7][1] == 0xfeedface);
  TopInt64Cache[0] = nullptr; TopInt64ValidCache[0] = nullptr; assert(read(rounded) == exact);
  state.ftw = 0; TopInt64Cache[0] = nullptr; TopInt64ValidCache[0] = nullptr; assert(read(rounded) == rounded); state.ftw = 0x80;
  TopValueCache[0] = node(0x4340000000000000ULL); TopInt64Cache[0] = nullptr; TopInt64ValidCache[0] = nullptr; FlushValuesPending[0] = true;
  assert(read(rounded) == rounded); flush(); assert(state.x87_exact_valid == 0 && read(rounded) == rounded);
  TopValueCache[0] = node(0x4340000000000000ULL); TopInt64Cache[0] = node(exact); TopInt64ValidCache[0] = node(1); FlushValuesPending[0] = true;
  flush(); TopInt64Cache[0] = nullptr; TopInt64ValidCache[0] = nullptr; state.x87_exact_valid = 0; assert(read(rounded) == rounded);
  physical_top = 0; state.ftw = 3; TopValueCache[0] = node(1); TopInt64Cache[0] = node(exact); TopInt64ValidCache[0] = node(1); FlushValuesPending[0] = true;
  TopValueCache[1] = node(2); TopInt64Cache[1] = node(exact + 1); TopInt64ValidCache[1] = node(1); FlushValuesPending[1] = true;
  flush(); TopInt64Cache[0] = nullptr; TopInt64ValidCache[0] = nullptr; assert(state.x87_exact_valid == 3 && read(rounded) == exact);
  physical_top = 1; TopInt64Cache[0] = nullptr; TopInt64ValidCache[0] = nullptr; assert(read(rounded) == exact + 1);
  TopInt64Cache[0] = nullptr; TopInt64ValidCache[0] = nullptr;
  copy_push(0); assert(physical_top == 0 && read(rounded) == exact + 1);
  flush(); TopInt64Cache[0] = nullptr; TopInt64ValidCache[0] = nullptr;
  assert(read(rounded) == exact + 1);
  for (unsigned i = 0; i < 8; ++i) { TopValueCache[i] = TopInt64Cache[i] = TopInt64ValidCache[i] = nullptr; FlushValuesPending[i] = false; }
  state = {}; physical_top = 0; state.ftw = 3; state.x87_exact_valid = 3;
  state.x87_exact_int[0][0] = exact; state.x87_exact_int[1][0] = exact + 1;
  swap_slots(1); assert(read(rounded) == exact + 1); flush();
  TopInt64Cache[0] = TopInt64ValidCache[0] = nullptr; assert(read(rounded) == exact + 1);
  physical_top = 1; TopInt64Cache[0] = TopInt64ValidCache[0] = nullptr; assert(read(rounded) == exact);
  physical_top = 0; TopInt64Cache[0] = TopInt64ValidCache[0] = nullptr;
  store_copy(1); flush(); physical_top = 1; TopInt64Cache[0] = TopInt64ValidCache[0] = nullptr;
  assert(read(rounded) == exact + 1);
}
'''
with tempfile.TemporaryDirectory(prefix='fex-x87-provenance-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-x', 'c++', '-', '-o', str(binary)],
                   input=prelude + load_exact + '\nstatic void flush() {\n' + flush + middle + copy + copy_end + store_copy + store_end + swap + swap_end + read + main, text=True, check=True)
    subprocess.run([str(binary)], check=True)
print('x87 provenance emission preserves, invalidates, and isolates physical slots')
