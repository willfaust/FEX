%ifdef CONFIG
{
  "RegData": { "RAX": "0x0020000000000000" },
  "Env": { "FEX_X87REDUCEDPRECISION": "1" }
}
%endif

; Two values round to the same double. Reuse the physical x87 slot for a
; double loaded after the integer is popped, then cross a translation block.
; The earlier integer must not be reused for the later double.
mov rdi, 0xe0000000
mov rax, 0x0020000000000001
mov [rdi], rax
mov rax, 0x4340000000000000
mov [rdi + 8], rax
fild qword [rdi]
test rdi, rdi
jz .first
nop
.first:
fstp st0
fld qword [rdi + 8]
test rdi, rdi
jz .second
nop
.second:
fistp qword [rdi + 16]
mov rax, [rdi + 16]
hlt
