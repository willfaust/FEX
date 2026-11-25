%ifdef CONFIG
{
  "RegData": {
    "R15": "0",
    "RBX": "9632"
  },
  "Env": { "MADEIRA_FEX_MOVE_NOPUSH": "2", "MADEIRA_FEX_MOVE_FORM": "1", "MADEIRA_FEX_MOVE_CENSUS": "1" },
  "MemoryRegions": {
    "0x100000000": "196608"
  }
}
%endif

; The sweep with both forms compiled, the thread on the one without the push.
%include "madeira_crt_memmove_fast.inc"
