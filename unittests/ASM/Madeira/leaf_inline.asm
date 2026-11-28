%ifdef CONFIG
{
  "RegData": {
    "R15": "0",
    "RBX": "13"
  },
  "MemoryRegions": {
    "0x100000000": "65536"
  }
}
%endif

; madeira-duo: leaf inline (Decoder::TryDecodeInlineLeaf, OpDispatchBuilder::InlineLeafReturn).
; Direct calls to tiny straight-line functions are compiled inline at the call site. Every case
; below checks that the caller sees exactly what a real call and return would leave: results,
; flags, the stack pointer, the word below it, and where a callee that rewrites its return
; address ends up. R15 counts failures, RBX the cases run.
; The leafs before `main` sit below the code that calls them, the ones at the end above it.

  jmp main

align 16
leaf_identity:                  ; mov rax, rcx; ret
  mov rax, rcx
  ret

align 16
leaf_stack_arg:                 ; reads the caller's stack through the return address
  mov rax, [rsp + 8]
  add rax, [rsp + 16]
  ret

align 16
leaf_cmp_setl:                  ; leaves flags for the caller
  cmp [rcx], edx
  setl al
  ret

align 16
leaf_store:                     ; memory writes and an SSE register
  xorps xmm0, xmm0
  movups [rcx], xmm0
  mov dword [rcx + 16], 0x12345678
  mov rax, rcx
  ret

align 16
leaf_push_pop:
  push rcx
  push rdx
  pop rax
  pop rdx
  ret

align 16
leaf_redirect:                  ; replaces its own return address
  lea rax, [rel redirect_target]
  mov [rsp], rax
  mov eax, 0x1111
  ret

align 16
leaf_rep_ret:
  lea rax, [rcx + rdx*2 + 5]
  db 0xF3, 0xC3                 ; rep ret

main:
  mov rsp, 0xe8000000
  xor r15, r15
  xor ebx, ebx
  mov rdi, 0x100000000

  ; 1: identity, many times, with the stack pointer and the pushed word checked.
  mov r12, 1000
  mov r13, rsp
.loop1:
  mov rcx, r12
  call leaf_identity
  cmp rax, r12
  jne .bad1
  cmp rsp, r13
  jne .bad1
  lea rax, [rel .after1]
  dec r12
  jnz .loop1
.after1:
  jmp .ok1
.bad1:
  inc r15
  mov rsp, r13
.ok1:
  inc rbx

  ; 2: the return address is really on the stack while the leaf runs.
  mov qword [rsp - 8], 0
  mov rcx, 7
  call leaf_identity
.ret2:
  lea rax, [rel .ret2]
  cmp [rsp - 8], rax
  je .ok2
  inc r15
.ok2:
  inc rbx

  ; 3: stack arguments.
  push 30
  push 12
  call leaf_stack_arg
  add rsp, 16
  cmp rax, 42
  je .ok3
  inc r15
.ok3:
  inc rbx

  ; 4: the caller's flags survive a leaf that sets none (carry in, carry out).
  mov rcx, 5
  mov r8d, 0
  stc
  call leaf_identity
  adc r8d, 0
  cmp r8d, 1
  jne .bad4
  clc
  call leaf_identity
  adc r8d, 0
  cmp r8d, 1
  je .ok4
.bad4:
  inc r15
.ok4:
  inc rbx

  ; 5: the leaf's flags reach the caller.
  mov dword [rdi], 3
  mov rcx, rdi
  mov edx, 9
  call leaf_cmp_setl
  jnl .bad5
  cmp al, 1
  jne .bad5
  mov edx, 2
  call leaf_cmp_setl
  jl .bad5
  jz .bad5
  cmp al, 0
  je .ok5
.bad5:
  inc r15
.ok5:
  inc rbx

  ; 6: stores and SSE.
  mov rax, 0xFFFFFFFFFFFFFFFF
  mov [rdi + 32], rax
  mov [rdi + 40], rax
  mov [rdi + 48], rax
  movq xmm0, rax
  lea rcx, [rdi + 32]
  call leaf_store
  lea rdx, [rdi + 32]
  cmp rax, rdx
  jne .bad6
  cmp qword [rdi + 32], 0
  jne .bad6
  cmp qword [rdi + 40], 0
  jne .bad6
  mov rax, 0xFFFFFFFF12345678
  cmp [rdi + 48], rax
  jne .bad6
  movq rax, xmm0
  test rax, rax
  jz .ok6
.bad6:
  inc r15
.ok6:
  inc rbx

  ; 7: pushes and pops inside the leaf.
  mov rcx, 0xAAAA
  mov rdx, 0xBBBB
  mov r13, rsp
  call leaf_push_pop
  cmp rax, 0xBBBB
  jne .bad7
  cmp rdx, 0xAAAA
  jne .bad7
  cmp rsp, r13
  je .ok7
.bad7:
  inc r15
  mov rsp, r13
.ok7:
  inc rbx

  ; 8: a leaf that replaces its return address returns there, not here.
  mov r13, rsp
  xor r14d, r14d
  call leaf_redirect
  inc r15                       ; skipped when the redirect is honoured
redirect_back:
  cmp r14d, 1
  jne .bad8
  cmp eax, 0x1111
  jne .bad8
  cmp rsp, r13
  je .ok8
.bad8:
  inc r15
  mov rsp, r13
.ok8:
  inc rbx

  ; 9: rep ret.
  mov rcx, 100
  mov rdx, 10
  call leaf_rep_ret
  cmp rax, 125
  je .ok9
  inc r15
.ok9:
  inc rbx

  ; 10: a leaf above the caller, called from a loop with a live flag across the call.
  mov r12d, 64
  xor r13d, r13d
.loop10:
  mov rcx, r12
  call leaf_high_add
  add r13, rax
  dec r12d
  jnz .loop10
  cmp r13, 64 * 65 / 2 + 64 * 3
  je .ok10
  inc r15
.ok10:
  inc rbx

  ; 11: twelve instructions (the most a leaf may have) and thirteen (one too many).
  mov rcx, 1
  call leaf_twelve
  cmp rax, 1 + 11
  jne .bad11
  mov rcx, 1
  call leaf_thirteen
  cmp rax, 1 + 12
  je .ok11
.bad11:
  inc r15
.ok11:
  inc rbx

  ; 12: a callee with a branch stays a real call.
  mov rcx, 5
  call not_a_leaf
  cmp rax, 10
  jne .bad12
  mov rcx, -5
  call not_a_leaf
  cmp rax, 5
  je .ok12
.bad12:
  inc r15
.ok12:
  inc rbx

  ; 13: two calls in a row and a leaf result feeding the next call.
  mov rcx, 20
  call leaf_high_add
  mov rcx, rax
  call leaf_high_add
  mov rcx, rax
  call leaf_identity
  cmp rax, 26
  je .ok13
  inc r15
.ok13:
  inc rbx

  hlt

redirect_target:
  inc r14d
  jmp redirect_back

align 16
leaf_high_add:
  lea rax, [rcx + 3]
  ret

align 16
leaf_twelve:
  mov rax, rcx
  inc rax
  inc rax
  inc rax
  inc rax
  inc rax
  inc rax
  inc rax
  inc rax
  inc rax
  add rax, 2
  ret

align 16
leaf_thirteen:
  mov rax, rcx
  inc rax
  inc rax
  inc rax
  inc rax
  inc rax
  inc rax
  inc rax
  inc rax
  inc rax
  inc rax
  add rax, 2
  ret

align 16
not_a_leaf:
  mov rax, rcx
  test rcx, rcx
  js .neg
  add rax, rax
  ret
.neg:
  neg rax
  ret
