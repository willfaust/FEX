%ifdef CONFIG
{
  "RegData": {
    "RAX": "0x84280002320300a8",
    "RBX": "0x84280002320300a8",
    "RCX": "0x84280002320300a8",
    "RSI": "0x8428000232030000"
  },
  "Env": { "FEX_X87REDUCEDPRECISION" : "1" }
}
%endif

; 32-bit MSVC loads a 64-bit value atomically with fild m64; fistp m64.
; A double holds 53 bits, so the unchanged value must come back exactly.
mov rdx, 0xe0000000

mov rax, 0x84280002320300a8
mov [rdx + 8 * 0], rax

; fild then fistp
fild qword [rdx + 8 * 0]
fistp qword [rdx + 8 * 1]
mov rax, [rdx + 8 * 1]

; fild, moved by fxch, then fisttp
fild qword [rdx + 8 * 0]
fld1
fxch
fisttp qword [rdx + 8 * 2]
fstp st0
mov rbx, [rdx + 8 * 2]

; fild, copied by fld st0, then fistp of the copy
fild qword [rdx + 8 * 0]
fld st0
fistp qword [rdx + 8 * 3]
fstp st0
mov rcx, [rdx + 8 * 3]

; A changed value goes through the double: fild, +0, fistp rounds.
fild qword [rdx + 8 * 0]
fldz
faddp
fistp qword [rdx + 8 * 4]
mov rsi, [rdx + 8 * 4]

hlt
