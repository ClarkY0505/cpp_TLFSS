section .text

global avx2_memcpy

avx2_memcpy:

    ; rdi = dst
    ; rsi = src
    ; rdx = size
    ;
    ; rax = original dst

    mov rax, rdi


    ; ============================================================
    ; < 32B:
    ; 完全不进入 YMM 主循环
    ; ============================================================

    cmp rdx, 32
    jb .small


; ================================================================
; 128B main loop
;
; 从这里开始会真正使用 YMM0~YMM3。
; 所以离开这些路径之前需要考虑 vzeroupper。
; ================================================================

.loop128:

    cmp rdx, 128
    jb .loop32

    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + 64]
    vmovdqu ymm3, [rsi + 96]

    vmovdqu [rdi],      ymm0
    vmovdqu [rdi + 32], ymm1
    vmovdqu [rdi + 64], ymm2
    vmovdqu [rdi + 96], ymm3

    add rsi, 128
    add rdi, 128
    sub rdx, 128

    jmp .loop128


; ================================================================
; 32B blocks
;
; 这里同样使用 YMM0。
; ================================================================

.loop32:

    cmp rdx, 32
    jb .small_after_avx

    vmovdqu ymm0, [rsi]
    vmovdqu [rdi], ymm0

    add rsi, 32
    add rdi, 32
    sub rdx, 32

    jmp .loop32


; ================================================================
; 从 YMM 路径进入 small tail
;
; 注意这里和直接进入 .small 不一样。
;
; 如果前面已经执行过：
;
; vmovdqu ymm0,...
;
; 就应该在函数返回之前 vzeroupper。
; ================================================================

.small_after_avx:

    ; 先处理剩余的 0~31B。
    ;
    ; 但不能直接 ret，
    ; 最终要跳到 .done_avx。


    cmp rdx, 16
    jb .small_after_avx_8


    ; ------------------------------------------------------------
    ; 16~31B
    ; ------------------------------------------------------------

    vmovdqu xmm0, [rsi]
    vmovdqu xmm1, [rsi + rdx - 16]

    vmovdqu [rdi], xmm0
    vmovdqu [rdi + rdx - 16], xmm1

    jmp .done_avx


.small_after_avx_8:

    cmp rdx, 8
    jb .small_after_avx_4

    mov rcx, [rsi]
    mov r8, [rsi + rdx - 8]

    mov [rdi], rcx
    mov [rdi + rdx - 8], r8

    jmp .done_avx


.small_after_avx_4:

    cmp rdx, 4
    jb .small_after_avx_2

    mov ecx, [rsi]
    mov r8d, [rsi + rdx - 4]

    mov [rdi], ecx
    mov [rdi + rdx - 4], r8d

    jmp .done_avx


.small_after_avx_2:

    cmp rdx, 2
    jb .small_after_avx_1

    mov cx, [rsi]
    mov r8w, [rsi + rdx - 2]

    mov [rdi], cx
    mov [rdi + rdx - 2], r8w

    jmp .done_avx


.small_after_avx_1:

    test rdx, rdx
    jz .done_avx

    mov cl, [rsi]
    mov [rdi], cl


.done_avx:

    vzeroupper
    ret


; ================================================================
; 直接从函数入口进入的小数据路径
;
; 这里保证：
;
; 原始 size < 32
;
; 因此前面从来没有执行过 256-bit YMM 指令。
;
; 所以这些路径可以直接 ret，
; 不需要 vzeroupper。
; ================================================================

.small:

    cmp rdx, 16
    jb .small8


    ; ------------------------------------------------------------
    ; 16 ~ 31B
    ; ------------------------------------------------------------

    vmovdqu xmm0, [rsi]
    vmovdqu xmm1, [rsi + rdx - 16]

    vmovdqu [rdi], xmm0
    vmovdqu [rdi + rdx - 16], xmm1

    ret


.small8:

    cmp rdx, 8
    jb .small4


    ; ------------------------------------------------------------
    ; 8 ~ 15B
    ; ------------------------------------------------------------

    mov rcx, [rsi]
    mov r8, [rsi + rdx - 8]

    mov [rdi], rcx
    mov [rdi + rdx - 8], r8

    ret


.small4:

    cmp rdx, 4
    jb .small2


    ; ------------------------------------------------------------
    ; 4 ~ 7B
    ; ------------------------------------------------------------

    mov ecx, [rsi]
    mov r8d, [rsi + rdx - 4]

    mov [rdi], ecx
    mov [rdi + rdx - 4], r8d

    ret


.small2:

    cmp rdx, 2
    jb .small1


    ; ------------------------------------------------------------
    ; 2 ~ 3B
    ; ------------------------------------------------------------

    mov cx, [rsi]
    mov r8w, [rsi + rdx - 2]

    mov [rdi], cx
    mov [rdi + rdx - 2], r8w

    ret


.small1:

    test rdx, rdx
    jz .done_small


    ; ------------------------------------------------------------
    ; 1B
    ; ------------------------------------------------------------

    mov cl, [rsi]
    mov [rdi], cl


.done_small:

    ret


section .note.GNU-stack noalloc noexec nowrite progbits
