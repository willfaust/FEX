%ifdef CONFIG
{
  "RegData": {
    "R15": "0",
    "RBX": "12"
  },
  "Env": { "MADEIRA_FEX_MOVE_NOPUSH": "0" },
  "MemoryRegions": {
    "0x100000000": "65536"
  }
}
%endif

; The call-site copy between the CALL's push and the routine's RET (the default form).
%include "madeira_crt_memmove_site.inc"
