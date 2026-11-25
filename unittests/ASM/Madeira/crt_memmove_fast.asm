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

; madeira-duo: the memmove fast path (OpDispatchBuilder::MatchCRTMemmove), at the routine's entry
; and at call sites (OpDispatchBuilder::InlineCRTMemmoveCall).
; A routine that starts like MSVC's memcpy/memmove is called for every length 0..300 at sixteen
; distances between source and destination, overlapping both ways included. Each result is compared
; with a plain REP MOVSB copy of the same bytes in a second buffer, bytes around the copy included.
; Every case runs twice: through a direct CALL (the call-site copy) and through a CALL of a
; register (the copy at the routine's entry).
; R15 counts mismatches (and wrong return values, and a stack pointer that moved), RBX the cases run.
; R14 counts calls that reached the routine's own code: every call when the fast path is not
; compiled in, only the long and overlapping ones when it is. It is not checked here, since it
; depends on the block size the test runs with.

image_base:
  mov rsp, 0xe8000000
  xor r15, r15
  xor r14, r14
  xor ebx, ebx

  ; Master pattern: 1536 bytes at 0x100000000.
  mov rdi, 0x100000000
  xor ecx, ecx
.fill:
  mov eax, ecx
  imul eax, eax, 73
  add eax, 11
  mov edx, ecx
  shr edx, 8
  xor eax, edx
  mov [rdi + rcx], al
  inc ecx
  cmp ecx, 1536
  jne .fill

  xor r12d, r12d                  ; length
.len_loop:
  xor r13d, r13d                  ; index into deltas
.delta_loop:
  ; Both working buffers start as the master pattern.
  cld
  mov rsi, 0x100000000
  mov rdi, 0x100010000
  mov ecx, 192
  rep movsq
  mov rsi, 0x100000000
  mov rdi, 0x100020000
  mov ecx, 192
  rep movsq

  lea rax, [rel deltas]
  movsx rbp, word [rax + r13*2]

  ; Under test: crt_memmove(dst, src, length) in the first buffer.
  mov rdx, 0x100010000 + 600
  lea rcx, [rdx + rbp]
  mov r8, r12
  mov rsi, rsp
  mov rax, 0x100000000 + 0x2fff8  ; the pass counter, past the buffers
  cmp qword [rax], 0
  jne .by_register
  call crt_memmove
  jmp .called
.by_register:
  lea rax, [rel crt_memmove]
  call rax
.called:
  cmp rsi, rsp
  je .sp_ok
  inc r15
.sp_ok:
  mov rdx, 0x100010000 + 600
  lea rcx, [rdx + rbp]
  cmp rax, rcx
  je .ret_ok
  inc r15
.ret_ok:

  ; Reference: the same move in the second buffer.
  mov rsi, 0x100020000 + 600
  lea rdi, [rsi + rbp]
  mov rcx, r12
  call ref_move

  mov rsi, 0x100010000
  mov rdi, 0x100020000
  mov ecx, 192
  repe cmpsq
  je .same
  inc r15
.same:
  inc rbx

  inc r13d
  cmp r13d, 16
  jne .delta_loop
  inc r12d
  cmp r12d, 301
  jne .len_loop

  ; Second pass: the same cases through a register.
  mov rax, 0x100000000 + 0x2fff8
  inc qword [rax]
  cmp qword [rax], 2
  jne .restart

  hlt

.restart:
  xor r12d, r12d
  jmp .len_loop

deltas:
  dw -590, -256, -255, -100, -17, -16, -8, -1, 0, 1, 3, 16, 31, 100, 256, 590

; rdi = destination, rsi = source, rcx = length. memmove semantics with REP MOVSB.
ref_move:
  mov rax, rdi
  sub rax, rsi
  cmp rax, rcx
  jb .back
  cld
  rep movsb
  ret
.back:
  lea rsi, [rsi + rcx - 1]
  lea rdi, [rdi + rcx - 1]
  std
  rep movsb
  cld
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
