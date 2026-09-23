section .text

global tlss_avx2_nt_memcpy_prefetch

tlss_avx2_nt_memcpy_prefetch:

    mov rax, rdi

    ; 当前实验约束：
    ;
    ; dst 32B aligned
    ; size % 256 == 0

.prefetch_loop:

    ;
    ; 至少还剩 768B：
    ;
    ; 当前处理 256B
    ; 同时 prefetch 当前地址 +512B
    ;
    cmp rdx, 768
    jb .loop256

    prefetcht0 [rsi + 512]
    prefetcht0 [rsi + 576]
    prefetcht0 [rsi + 640]
    prefetcht0 [rsi + 704]

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

    jmp .prefetch_loop


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
