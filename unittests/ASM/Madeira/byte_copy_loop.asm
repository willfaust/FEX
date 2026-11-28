%ifdef CONFIG
{
  "RegData": {
    "R15": "0",
    "RBX": "280"
  },
  "MemoryRegions": {
    "0x100000000": "196608"
  }
}
%endif

; madeira-duo: the byte-copy-loop idiom (OpDispatchBuilder::MatchByteCopyLoop).
; MSVC's byte-at-a-time copy loop runs for lengths 1..40 at seven distances, overlapping included
; (a forward byte loop over overlapping ranges repeats bytes; the idiom has to do the same).
; Registers and flags after the loop are compared with the same loop written so that the idiom
; does not match it (an extra NOP in the body). R15 counts mismatches, RBX the cases run.

  mov rsp, 0xe8000000
  xor r15, r15
  xor ebx, ebx

  mov rdi, 0x100000000
  xor ecx, ecx
.fill:
  mov eax, ecx
  imul eax, eax, 73
  add eax, 11
  mov [rdi + rcx], al
  inc ecx
  cmp ecx, 512
  jne .fill

  mov r12d, 1                     ; length
.len_loop:
  xor r13d, r13d
.delta_loop:
  cld
  mov rsi, 0x100000000
  mov rdi, 0x100010000
  mov ecx, 64
  rep movsq
  mov rsi, 0x100000000
  mov rdi, 0x100020000
  mov ecx, 64
  rep movsq

  lea rax, [rel deltas]
  movsx rbp, word [rax + r13*2]

  ; Under test.
  mov rdx, 0x100010000 + 200       ; source
  lea rcx, [rdx + rbp]            ; destination
  mov r8, r12
  mov eax, 0x11223344
  call idiom_loop
  pushfq
  pop r9
  mov r10, rax
  mov r11, rcx
  mov rsi, rdx
  mov rdi, r8
  push r9
  push r10
  push r11
  push rsi
  push rdi

  ; Reference.
  mov rdx, 0x100020000 + 200
  lea rcx, [rdx + rbp]
  mov r8, r12
  mov eax, 0x11223344
  call plain_loop
  pushfq
  pop r9

  pop rdi
  cmp rdi, r8                     ; count
  jne .bad5
  pop rsi
  add rsi, 0x10000                ; the same offset in the second buffer
  cmp rsi, rdx                    ; source pointer
  jne .bad4
  pop r11
  add r11, 0x10000
  cmp r11, rcx                    ; destination pointer
  jne .bad3
  pop r10
  cmp r10, rax                    ; the byte register (upper bits kept, low byte = last byte)
  jne .bad2
  pop r10
  xor r10, r9
  test r10, 0x8D5                 ; CF PF AF ZF SF OF
  jnz .bad1
  jmp .regs_ok
.bad5:
  pop rsi
.bad4:
  pop r11
.bad3:
  pop r10
.bad2:
  pop r10
.bad1:
  inc r15
.regs_ok:

  mov rsi, 0x100010000
  mov rdi, 0x100020000
  mov ecx, 64
  repe cmpsq
  je .same
  inc r15
.same:
  inc rbx

  inc r13d
  cmp r13d, 7
  jne .delta_loop
  inc r12d
  cmp r12d, 41
  jne .len_loop

  hlt

deltas:
  dw -150, -5, -1, 1, 2, 7, 150

; rcx = destination, rdx = source, r8 = count (not 0), al = byte register.
align 16
idiom_loop:
  jmp .loop
align 16
.loop:
  db 0x0F, 0xB6, 0x02             ; movzx eax, byte [rdx]
  db 0x88, 0x01                   ; mov [rcx], al
  db 0x48, 0x8D, 0x49, 0x01       ; lea rcx, [rcx + 1]
  db 0x48, 0x8D, 0x52, 0x01       ; lea rdx, [rdx + 1]
  db 0x49, 0x83, 0xE8, 0x01       ; sub r8, 1
  db 0x75, 0xED                   ; jne .loop
  ret

align 16
plain_loop:
  jmp .loop
align 16
.loop:
  movzx eax, byte [rdx]
  mov [rcx], al
  nop
  lea rcx, [rcx + 1]
  lea rdx, [rdx + 1]
  sub r8, 1
  jne .loop
  ret
