%ifdef CONFIG
{
  "RegData": {
    "R15": "0",
    "RBX": "9632"
  },
  "MemoryRegions": {
    "0x100000000": "196608"
  }
}
%endif

; The sweep with the default settings.
%include "madeira_crt_memmove_fast.inc"
