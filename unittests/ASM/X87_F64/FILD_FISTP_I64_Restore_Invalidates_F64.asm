%ifdef CONFIG
{
  "RegData": { "RAX": "0x0020000000000000" },
  "Env": { "FEX_X87REDUCEDPRECISION": "1" }
}
%endif

; The saved double and the later integer round to the same value. FXRSTOR
; must invalidate the integer provenance before reusing its physical slot.
mov rdi, 0xe0000000
mov rax, 0x4340000000000000
mov [rdi], rax
mov rax, 0x0020000000000001
mov [rdi + 8], rax
fld qword [rdi]
fxsave [rdi + 0x100]
fstp st0
fild qword [rdi + 8]
test rdi, rdi
jz .resume
nop
.resume:
fxrstor [rdi + 0x100]
fistp qword [rdi + 16]
mov rax, [rdi + 16]
hlt
