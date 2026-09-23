section .text

global rep_memcpy

rep_memcpy:
    ; System V AMD64 ABI
    ;
    ; rdi = dst
    ; rsi = src
    ; rdx = size
    ;
    ; return:
    ; rax = original dst

    mov rax, rdi
    mov rcx, rdx
    rep movsb

    ret

section .note.GNU-stack noalloc noexec nowrite progbits
