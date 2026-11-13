%ifdef CONFIG
{
  "RegData": { "RAX": "0x0020000000000001", "RBX": "0x0020000000000001", "RCX": "0x0020000000000001" },
  "Env": { "FEX_X87REDUCEDPRECISION": "1" }
}
%endif

; Copy and exchange a value after its integer provenance crossed a block.
mov rdi, 0xe0000000
mov rax, 0x0020000000000001
mov [rdi], rax
fild qword [rdi]
test rdi, rdi
jz .one
nop
.one:
fld st0
test rdi, rdi
jz .two
nop
.two:
fistp qword [rdi + 8]
fistp qword [rdi + 16]
mov rax, [rdi + 8]
mov rbx, [rdi + 16]
fild qword [rdi]
fld1
test rdi, rdi
jz .three
nop
.three:
fxch st1
test rdi, rdi
jz .four
nop
.four:
fistp qword [rdi + 24]
fstp st0
mov rcx, [rdi + 24]
hlt
