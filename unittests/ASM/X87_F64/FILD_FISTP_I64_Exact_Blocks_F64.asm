%ifdef CONFIG
{
  "RegData": {
    "RAX": "0x84280002320300a8",
    "RBX": "0x0047005c003a0043",
    "RCX": "0x005c00730065006d",
    "RDX": "0x84280002320300a8",
    "RSI": "0x8428000232030000"
  },
  "Env": { "FEX_X87REDUCEDPRECISION" : "1" }
}
%endif

; fild m64 and fistp m64 in different blocks: a branch target starts a new
; block, whose x87 stack is read back from the context. Delphi's Move copies
; 8 bytes this way around a branch or a loop.
mov rdi, 0xe0000000

mov rax, 0x84280002320300a8
mov [rdi + 8 * 0], rax
mov rax, 0x0047005c003a0043
mov [rdi + 8 * 1], rax
mov rax, 0x005c00730065006d
mov [rdi + 8 * 2], rax

; fild, a branch, fistp
fild qword [rdi + 8 * 0]
test rdi, rdi
jz .a
nop
.a:
fistp qword [rdi + 8 * 3]
mov rax, [rdi + 8 * 3]

; Delphi's small Move: two loads, a branch, the stores in reverse
fild qword [rdi + 8 * 1]
fild qword [rdi + 8 * 2]
test rdi, rdi
jz .b
nop
.b:
fistp qword [rdi + 8 * 5]
fistp qword [rdi + 8 * 4]
mov rbx, [rdi + 8 * 4]
mov rcx, [rdi + 8 * 5]

; Delphi's large Move: the first value loaded before a loop, stored after it
fild qword [rdi + 8 * 0]
mov r8, 1
.loop:
fild qword [rdi + 8 * r8]
fistp qword [rdi + 8 * 6]
inc r8
cmp r8, 3
jl .loop
fistp qword [rdi + 8 * 7]
mov rdx, [rdi + 8 * 7]

; A changed value goes through the double: fild, a branch, +0, fistp rounds.
fild qword [rdi + 8 * 0]
test rdi, rdi
jz .c
nop
.c:
fldz
faddp
fistp qword [rdi + 8 * 8]
mov rsi, [rdi + 8 * 8]

hlt
