%ifdef CONFIG
{
  "RegData": {
    "R15": "0",
    "RBX": "12"
  },
  "Env": { "MADEIRA_FEX_MOVE_NOPUSH": "2", "MADEIRA_FEX_MOVE_FORM": "0", "MADEIRA_FEX_MOVE_CENSUS": "1" },
  "MemoryRegions": {
    "0x100000000": "65536"
  }
}
%endif

; Both forms compiled, the thread runs the one with the push.
%include "madeira_crt_memmove_site.inc"
