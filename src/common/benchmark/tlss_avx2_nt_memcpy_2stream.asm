section .text

global tlss_avx2_nt_memcpy_2stream

tlss_avx2_nt_memcpy_2stream:

    mov rax, rdi

    ; ------------------------------------------------------------
    ; Experimental NT 2-stream kernel
    ;
    ; requirement:
    ;   dst  % 32   == 0
    ;   size % 8192 == 0
    ;
    ; 每次处理两个相隔 4 KiB 的 stream
    ; ------------------------------------------------------------

.loop8k:

    cmp rdx, 8192
    jb .done

    ;
    ; 每次 inner iteration:
    ;
    ; stream0: 128B
    ; stream1: 128B
    ;
    ; total = 256B
    ;
    ; 32 iterations:
    ;
    ; 32 * 128 = 4096B / stream
    ;
    mov ecx, 32

.inner:

    ; ============================================================
    ; stream 0
    ; ============================================================

    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + 64]
    vmovdqu ymm3, [rsi + 96]

    ; ============================================================
    ; stream 1: +4 KiB
    ; ============================================================

    vmovdqu ymm4, [rsi + 4096]
    vmovdqu ymm5, [rsi + 4096 + 32]
    vmovdqu ymm6, [rsi + 4096 + 64]
    vmovdqu ymm7, [rsi + 4096 + 96]

    ; ============================================================
    ; NT stores
    ; ============================================================

    vmovntdq [rdi],      ymm0
    vmovntdq [rdi + 32], ymm1
    vmovntdq [rdi + 64], ymm2
    vmovntdq [rdi + 96], ymm3

    vmovntdq [rdi + 4096],      ymm4
    vmovntdq [rdi + 4096 + 32], ymm5
    vmovntdq [rdi + 4096 + 64], ymm6
    vmovntdq [rdi + 4096 + 96], ymm7

    ;
    ; 两个 stream 同时向前走 128B
    ;
    add rsi, 128
    add rdi, 128

    dec ecx
    jnz .inner

    ;
    ; inner 结束时：
    ;
    ; rsi/rdi 已经前进 4096B
    ;
    ; 但实际上我们已经复制：
    ;
    ; base + 0..4095
    ; base + 4096..8191
    ;
    ; 所以还要跳过第二个 4 KiB。
    ;

    add rsi, 4096
    add rdi, 4096

    sub rdx, 8192

    jmp .loop8k


.done:

    sfence
    vzeroupper
    ret


section .note.GNU-stack noalloc noexec nowrite progbits
