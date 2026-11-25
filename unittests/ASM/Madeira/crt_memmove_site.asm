%ifdef CONFIG
{
  "RegData": {
    "R15": "0",
    "RBX": "12"
  },
  "MemoryRegions": {
    "0x100000000": "65536"
  }
}
%endif

; madeira-duo: memmove at call sites (OpDispatchBuilder::InlineCRTMemmoveCall), the parts the
; sweep in crt_memmove_fast.asm does not look at. Each case is its own call site.
; R15 counts failures, RBX the cases run.
;  1-7   fixed lengths 0, 1, 7, 24, 64, 256 and 257: the bytes, the return value, the stack
;        pointer, and the word under the stack pointer, which is the return address the CALL pushed
;  8     the callee-saved registers and the three arguments' registers after a copied call
;  9     a copy that lands on its own return address: the routine returns to what was copied
;  10    two calls back to back in one block
;  11    a call whose source and destination overlap (left to the routine)
;  12    the last call of a function that ends right after it

image_base:
  mov rsp, 0xe8000000
  xor r15, r15
  xor r14, r14
  xor ebx, ebx

  ; Source pattern: 512 bytes at 0x100000000, byte i = i * 7 + 3.
  mov rdi, 0x100000000
  xor ecx, ecx
.fill:
  mov eax, ecx
  imul eax, eax, 7
  add eax, 3
  mov [rdi + rcx], al
  inc ecx
  cmp ecx, 512
  jne .fill

%macro CLEAR_DEST 0
  cld
  mov rdi, 0x100001000
  mov ecx, 64
  mov rax, 0x5a5a5a5a5a5a5a5a
  rep stosq
%endmacro

; %1 = length. Checks bytes [0, length) equal the source, the byte after still 0x5a, RAX, RSP, [RSP-8].
%macro FIXED 1
  CLEAR_DEST
  mov rdx, 0x100000000 + 5
  mov rcx, 0x100001000 + 16
  mov r8d, %1
  mov rbp, rsp
  call crt_memmove
%%after:
  cmp rbp, rsp
  jne %%bad
  lea r9, [rel %%after]
  cmp [rsp - 8], r9
  jne %%bad
  mov r9, 0x100001000 + 16
  cmp rax, r9
  jne %%bad
  mov rsi, 0x100000000 + 5
  mov rdi, 0x100001000 + 16
  mov ecx, %1
  repe cmpsb
  jne %%bad
  cmp byte [rdi], 0x5a
  jne %%bad
  mov rdi, 0x100001000 + 15
  cmp byte [rdi], 0x5a
  je %%ok
%%bad:
  inc r15
%%ok:
  inc rbx
%endmacro

  FIXED 0
  FIXED 1
  FIXED 7
  FIXED 24
  FIXED 64
  FIXED 256
  FIXED 257

  ; 8: registers.
  CLEAR_DEST
  mov rsi, 0x1111111111111111
  mov rdi, 0x2222222222222222
  mov r12, 0x3333333333333333
  mov r13, 0x4444444444444444
  mov rbp, 0x5555555555555555
  mov rdx, 0x100000000 + 1
  mov rcx, 0x100001000 + 8
  mov r8d, 40
  call crt_memmove
  mov r9, 0x1111111111111111
  cmp rsi, r9
  jne .bad8
  mov r9, 0x2222222222222222
  cmp rdi, r9
  jne .bad8
  mov r9, 0x3333333333333333
  cmp r12, r9
  jne .bad8
  mov r9, 0x4444444444444444
  cmp r13, r9
  jne .bad8
  mov r9, 0x5555555555555555
  cmp rbp, r9
  jne .bad8
  mov r9, 0x100001000 + 8
  cmp rax, r9
  je .ok8
.bad8:
  inc r15
.ok8:
  inc rbx

  ; 9: the destination is the slot the CALL pushes its return address into.
  lea rax, [rel .landed]
  mov rdx, 0x100002000
  mov [rdx], rax
  lea rcx, [rsp - 8]
  mov r8d, 8
  call crt_memmove
  inc r15                         ; the pushed return address was used: wrong
  jmp .after9
.landed:
  nop
.after9:
  inc rbx

  ; 10: back to back.
  CLEAR_DEST
  mov rdx, 0x100000000
  mov rcx, 0x100001000
  mov r8d, 16
  call crt_memmove
  mov rdx, 0x100000000 + 100
  lea rcx, [rax + 16]
  mov r8d, 9
  call crt_memmove
  mov r9, 0x100001000 + 16
  cmp rax, r9
  jne .bad10
  mov rsi, 0x100000000
  mov rdi, 0x100001000
  mov ecx, 16
  repe cmpsb
  jne .bad10
  mov rsi, 0x100000000 + 100
  mov ecx, 9
  repe cmpsb
  jne .bad10
  cmp byte [rdi], 0x5a
  je .ok10
.bad10:
  inc r15
.ok10:
  inc rbx

  ; 11: overlapping, destination 3 bytes above the source, 32 bytes.
  mov rsi, 0x100000000
  mov rdi, 0x100003000
  mov ecx, 64
  rep movsb
  mov rdx, 0x100003000
  mov rcx, 0x100003000 + 3
  mov r8d, 32
  mov r12, r14
  call crt_memmove
  mov rsi, 0x100000000
  mov rdi, 0x100003000 + 3
  mov ecx, 32
  repe cmpsb
  jne .bad11
  mov rsi, 0x100000000 + 35
  mov ecx, 29
  repe cmpsb
  je .ok11
.bad11:
  inc r15
.ok11:
  inc rbx

  ; 12: a function whose last instruction before RET is the call.
  CLEAR_DEST
  call tail_caller
  mov rsi, 0x100000000 + 2
  mov rdi, 0x100001000 + 4
  mov ecx, 20
  repe cmpsb
  jne .bad12
  cmp byte [rdi], 0x5a
  je .ok12
.bad12:
  inc r15
.ok12:
  inc rbx

  hlt

tail_caller:
  sub rsp, 40
  mov rdx, 0x100000000 + 2
  mov rcx, 0x100001000 + 4
  mov r8d, 20
  call crt_memmove
  add rsp, 40
  ret

align 16
crt_memmove:
  db 0x48, 0x8B, 0xC1                         ; mov rax, rcx
  db 0x4C, 0x8D, 0x15                         ; lea r10, [rel image_base]
  dd image_base - ($ + 4)
  db 0x49, 0x83, 0xF8, 0x0F                   ; cmp r8, 15
  db 0x0F, 0x87                               ; ja larger
  dd larger - ($ + 4)
  db 0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00
  db 0x47, 0x8B, 0x8C, 0x82                   ; mov r9d, [r10 + r8*4 + small_table]
  dd small_table - image_base
  db 0x4D, 0x03, 0xCA                         ; add r9, r10
  db 0x41, 0xFF, 0xE1                         ; jmp r9

larger:
small:
  inc r14
  test r8, r8
  jz .done
  mov r9, rcx
  sub r9, rdx
  cmp r9, r8
  jb .backward
  xor r9d, r9d
.forward:
  movzx r10d, byte [rdx + r9]
  mov [rcx + r9], r10b
  add r9, 1
  cmp r9, r8
  jne .forward
  ret
.backward:
  mov r9, r8
.back_loop:
  movzx r10d, byte [rdx + r9 - 1]
  mov [rcx + r9 - 1], r10b
  sub r9, 1
  jnz .back_loop
.done:
  ret

align 4
small_table:
  times 16 dd small - image_base
