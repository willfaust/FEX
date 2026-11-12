%ifdef CONFIG
{
  "RegData": { "RAX": "0x0020000000000001", "RBX": "0x8000000000000000", "RCX": "0x0000000000000002", "RDX": "0x8000000000000400", "RSI": "0x4034", "R8": "0x8000000000000000", "R9": "0xc03e" },
  "Env": { "FEX_X87REDUCEDPRECISION": "1" }
}
%endif

mov rdi, 0xe0000000
mov rax, 0x0020000000000001
mov [rdi], rax
fild qword [rdi]
test rdi, rdi
jz .save
nop
.save:
fxsave [rdi + 0x100]
mov rdx, [rdi + 0x120]
movzx rsi, word [rdi + 0x128]
fstp st0
fld1
fxrstor [rdi + 0x100]
fistp qword [rdi + 8]
mov rax, [rdi + 8]
mov rbx, 0x8000000000000000
mov [rdi + 16], rbx
fild qword [rdi + 16]
test rdi, rdi
jz .legacy
nop
.legacy:
fnsave [rdi + 0x300]
mov r8, [rdi + 0x31c]
movzx r9, word [rdi + 0x324]
frstor [rdi + 0x300]
fistp qword [rdi + 24]
mov rbx, [rdi + 24]
mov rcx, 0x3ff8000000000000
mov [rdi + 32], rcx
fld qword [rdi + 32]
fxsave [rdi + 0x100]
fstp st0
fxrstor [rdi + 0x100]
fistp qword [rdi + 40]
mov rcx, [rdi + 40]
hlt
