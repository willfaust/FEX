#!/usr/bin/env python3
"""Exercise the production x87 save/restore IR construction with a value model."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
src = (root / 'FEXCore/Source/Interface/Core/OpcodeDispatcher/X87.cpp').read_text()
start = src.index('Ref OpDispatchBuilder::SaveReducedX87Value(')
end = src.index('\nvoid OpDispatchBuilder::SetX87FTW(', start)
helpers = src[start:end]
pre = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <random>
#include <vector>
namespace Core { struct CPUState { uint64_t x87_exact_int[8][2]{}; uint8_t x87_exact_valid{}, AbridgedFTW{}; }; }
struct Node { uint64_t lo, hi; }; using Ref = Node*;
enum class OpSize { i8Bit, i32Bit, i64Bit, i128Bit };
enum class CondClass { SLT, EQ, NEQ, UGE, ULT };
static Core::CPUState state;
static std::vector<Node> nodes; static size_t pos;
static Ref n(uint64_t a, uint64_t b=0) { assert(pos<nodes.size()); nodes[pos]={a,b}; return &nodes[pos++]; }
struct OpDispatchBuilder {
 Ref Constant(uint64_t x) { return n(x); }
 Ref _LoadContextGPR(OpSize, size_t off) { return n(*((uint8_t*)&state+off)); }
 Ref _LoadContextGPRIndexed(Ref i,OpSize,size_t off,uint32_t stride) { return n(*(uint64_t*)((char*)&state+off+i->lo*stride)); }
 void _StoreContextGPRIndexed(Ref v,Ref i,OpSize,size_t off,uint32_t stride) { *(uint64_t*)((char*)&state+off+i->lo*stride)=v->lo; }
 Ref _And(OpSize,Ref a,Ref b) { return n(a->lo & b->lo); }
 Ref _Or(OpSize,Ref a,Ref b) { return n(a->lo | b->lo); }
 Ref _Lshr(OpSize size,Ref a,Ref b) { return n(size==OpSize::i32Bit ? uint32_t(a->lo)>>(b->lo&31) : a->lo>>(b->lo&63)); }
 Ref _Lshl(OpSize size,Ref a,Ref b) { return n(size==OpSize::i32Bit ? uint32_t(a->lo)<<(b->lo&31) : a->lo<<(b->lo&63)); }
 Ref Sub(OpSize,Ref a,Ref b) { return n(a->lo-b->lo); }
 Ref _FindMSB(OpSize,Ref a) { return n(a->lo ? 63-__builtin_clzll(a->lo) : uint64_t(-1)); }
 Ref _Select(OpSize,OpSize,CondClass c,Ref a,Ref b,Ref yes,Ref no) {
   bool test = c==CondClass::SLT ? int64_t(a->lo)<int64_t(b->lo) : c==CondClass::EQ ? a->lo==b->lo : c==CondClass::NEQ ? a->lo!=b->lo : c==CondClass::UGE ? a->lo>=b->lo : a->lo<b->lo;
   return test ? yes : no;
 }
 Ref _F80CVTTo(Ref,OpSize) { return n(0x1234,0x5678); }
 Ref _VExtractToGPR(OpSize,OpSize,Ref a,int i) { return n(i ? a->hi : a->lo); }
 Ref _VLoadTwoGPRs(Ref a,Ref b) { return n(a->lo,b->lo); }
 Ref SaveReducedX87Value(Ref,Ref); Ref RestoreReducedX87Value(Ref,Ref,Ref);
};
'''
main = r'''
int main() {
 nodes.resize(512); OpDispatchBuilder b; std::mt19937_64 rng(19);
 for (unsigned i=0;i<10000;i++) {
   uint64_t bits=i==0?0:i==1?0x8000000000000000ULL:i==2?0x7fffffffffffffffULL:rng();
   state={}; state.x87_exact_valid=state.AbridgedFTW=1<<3; state.x87_exact_int[3][0]=bits; pos=0;
   Ref saved=b.SaveReducedX87Value(n(3),n(0)); uint64_t low=saved->lo, high=saved->hi;
   uint64_t mag=int64_t(bits)<0 ? 0-bits : bits;
   uint64_t shift=mag?__builtin_clzll(mag):0;
   assert(low==(mag?mag<<shift:0));
   assert(high==((bits>>63?0x8000:0)|(mag?0x403e - shift:0)));
   state={}; pos=0; Ref mask=b.RestoreReducedX87Value(n(3),n(low,high),n(0));
   if (bits) { assert(mask->lo==(1<<3)); assert(state.x87_exact_int[3][0]==bits); }
   else assert(mask->lo==0);
 }
 state={}; pos=0; Ref fraction=b.RestoreReducedX87Value(n(2),n(0xc000000000000000ULL,0x3fff),n(0)); assert(!fraction->lo);
 pos=0; Ref huge=b.RestoreReducedX87Value(n(2),n(0x8000000000000000ULL,0x403e),n(0)); assert(!huge->lo);
 pos=0; Ref nan=b.RestoreReducedX87Value(n(2),n(0xc000000000000000ULL,0x7fff),n(0)); assert(!nan->lo);
 state={}; state.x87_exact_valid=1; state.AbridgedFTW=0; pos=0; Ref empty=b.SaveReducedX87Value(n(0),n(0)); assert(empty->lo==0x1234 && empty->hi==0x5678);
 puts("x87 80-bit save and restore integer provenance passes");
}
'''
with tempfile.TemporaryDirectory(prefix='fex-x87-state-') as directory:
    cpp = Path(directory) / 'test.cpp'
    exe = Path(directory) / 'test'
    cpp.write_text(pre + helpers + main)
    subprocess.run(['c++', '-std=c++20', '-O2', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
