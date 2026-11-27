%ifdef CONFIG
{
  "RegData": {
    "R15": "0",
    "RBX": "4"
  },
  "MemoryRegions": {
    "0x100000000": "65536"
  }
}
%endif

; madeira-duo leaf-long (MADEIRA_FEX_LEAF_MAX): callees of 13 to 32 instructions. Run with the variable unset
; (12: these stay real calls) and with MADEIRA_FEX_LEAF_MAX=24 (the 20-instruction leaf is compiled inline,
; the 26-instruction one is not). Either way the caller must see what a real call and return leave:
; results, flags, the stack pointer and the word below it. R15 counts failures, RBX the cases run.

  jmp main

align 16
leaf20:                          ; 19 instructions and the RET: a hash of rcx/rdx, ends with flags from a CMP
  mov rax, rcx
  imul rax, rax, 31
  add rax, rdx
  mov r9, rax
  shl r9, 7
  xor rax, r9
  mov r10, rax
  shr r10, 3
  sub rax, r10
  lea rax, [rax + rcx*2 + 5]
  mov r11, rdx
  and r11, 0xff
  or rax, r11
  movsxd r9, edx
  add rax, r9
  test rax, rax
  mov r10, [rsp + 8]
  add rax, r10
  cmp rax, rcx
  ret

align 16
leaf26:                          ; 25 instructions and the RET: above a 24 limit
  mov rax, rcx
  add rax, 1
  add rax, 2
  add rax, 3
  add rax, 4
  add rax, 5
  add rax, 6
  add rax, 7
  add rax, 8
  add rax, 9
  add rax, 10
  add rax, 11
  add rax, 12
  add rax, 13
  add rax, 14
  add rax, 15
  add rax, 16
  add rax, 17
  add rax, 18
  add rax, 19
  add rax, 20
  add rax, 21
  add rax, 22
  add rax, 23
  sub rax, rdx
  ret

; the reference for leaf20, computed with the same instructions inline
%macro REF20 0
  mov rax, rcx
  imul rax, rax, 31
  add rax, rdx
  mov r9, rax
  shl r9, 7
  xor rax, r9
  mov r10, rax
  shr r10, 3
  sub rax, r10
  lea rax, [rax + rcx*2 + 5]
  mov r11, rdx
  and r11, 0xff
  or rax, r11
  movsxd r9, edx
  add rax, r9
  test rax, rax
  mov r10, [rsp]
  add rax, r10
  cmp rax, rcx
%endmacro

main:
  mov rsp, 0x100008000
  xor r15, r15
  xor rbx, rbx

  ; 1: leaf20 many times with varying inputs; result, the flags of its CMP (via SETcc) and RSP checked
  mov r14, 500
  mov rcx, 0x123456789
  mov rdx, -77
.loop1:
  push 0x55aa                    ; [rsp] for REF20, [rsp + 8] inside the leaf
  REF20
  setb r12b
  setz r13b
  mov rdi, rax
  mov rsi, rsp
  call leaf20
  setb al
  setz r8b
  cmp al, r12b
  jne .bad1
  cmp r8b, r13b
  jne .bad1
  cmp rax, rax                   ; (al was overwritten: compare the full result separately)
  cmp rsi, rsp
  jne .bad1
  pop rax
  add rcx, 0x9e3779b97f4a7c15 & 0x7fffffff
  add rdx, 1234567
  dec r14
  jnz .loop1
  jmp .ok1
.bad1:
  inc r15
  pop rax
.ok1:
  inc rbx

  ; 2: leaf20's result value (setcc above checked flags only)
  push 0x55aa
  mov rcx, 1000
  mov rdx, 7
  REF20
  mov r12, rax
  call leaf20
  pop r9
  cmp rax, r12
  je .ok2
  inc r15
.ok2:
  inc rbx

  ; 3: the word the CALL pushed is below RSP afterwards, as with a real call
  mov rcx, 5
  mov rdx, 6
  lea r13, [rel .ret3]
  call leaf20
.ret3:
  cmp [rsp - 8], r13
  je .ok3
  inc r15
.ok3:
  inc rbx

  ; 4: leaf26 (stays a call under a 24 limit), result checked
  mov rcx, 100
  mov rdx, 50
  call leaf26
  cmp rax, 100 + 276 - 50
  je .ok4
  inc r15
.ok4:
  inc rbx

  hlt
