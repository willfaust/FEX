%ifdef CONFIG
{
  "RegData": { "RAX": "0x0020000000000001", "RBX": "0x1" },
  "Env": { "FEX_X87REDUCEDPRECISION": "0" },
  "HostFeatures": ["LINUX"]
}
%endif

; Install a guest signal handler that returns through rt_sigreturn.
mov rsp, 0xe8000000
mov rdi, 0xe0000000
lea rax, [rel .handler]
mov [rdi + 0x100], rax
mov qword [rdi + 0x108], 0x04000000
lea rax, [rel .restorer]
mov [rdi + 0x110], rax
mov qword [rdi + 0x118], 0
mov qword [rdi + 0x120], 0
mov eax, 13
mov edi, 10
mov rsi, 0xe0000100
xor edx, edx
mov r10d, 8
syscall

mov rdi, 0xe0000000
mov rax, 0x0020000000000001
mov [rdi], rax
fild qword [rdi]
mov eax, 39
syscall
mov edi, eax
mov esi, 10
mov eax, 62
syscall
mov rdi, 0xe0000000
fistp qword [rdi + 8]
mov rax, [rdi + 8]
mov rbx, [rdi + 0x120]
hlt
.handler:
mov rbx, 0xe0000000
mov qword [rbx + 0x120], 1
ret
.restorer:
mov eax, 15
syscall
ud2
