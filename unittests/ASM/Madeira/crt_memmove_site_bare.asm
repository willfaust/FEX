%ifdef CONFIG
{
  "RegData": {
    "R15": "0",
    "RBX": "12"
  },
  "Env": { "MADEIRA_FEX_MOVE_NOPUSH": "1", "MADEIRA_FEX_MOVE_CENSUS": "1" },
  "MemoryRegions": {
    "0x100000000": "65536"
  }
}
%endif

; The call-site copy with no push on the copied path, with the census counting.
%define MOVE_BARE 1
%include "madeira_crt_memmove_site.inc"
