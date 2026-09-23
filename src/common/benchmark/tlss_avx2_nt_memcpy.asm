section .text

global tlss_avx2_nt_memcpy

tlss_avx2_nt_memcpy:

    mov rax, rdi

    ; --------------------------------------------------
    ; 实验版本：
    ; 暂时要求 size 是 256B 的整数倍
    ; dst 32B aligned
    ; --------------------------------------------------

.loop256:

    cmp rdx, 256
    jb .done

    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + 64]
    vmovdqu ymm3, [rsi + 96]

    vmovdqu ymm4, [rsi + 128]
    vmovdqu ymm5, [rsi + 160]
    vmovdqu ymm6, [rsi + 192]
    vmovdqu ymm7, [rsi + 224]

    vmovntdq [rdi],       ymm0
    vmovntdq [rdi + 32],  ymm1
    vmovntdq [rdi + 64],  ymm2
    vmovntdq [rdi + 96],  ymm3

    vmovntdq [rdi + 128], ymm4
    vmovntdq [rdi + 160], ymm5
    vmovntdq [rdi + 192], ymm6
    vmovntdq [rdi + 224], ymm7

    add rsi, 256
    add rdi, 256
    sub rdx, 256

    jmp .loop256

.done:

    sfence
    vzeroupper
    ret


section .note.GNU-stack noalloc noexec nowrite progbits
