section .text

global tlss_avx2_memcpy_4

tlss_avx2_memcpy_4:

    ; ------------------------------------------------------------
    ; System V AMD64 ABI
    ;
    ; rdi = dst
    ; rsi = src
    ; rdx = size
    ;
    ; rax = return value
    ; ------------------------------------------------------------

    mov rax, rdi
    ; memcpy 返回原始 dst


    ; ============================================================
    ; 0 ~ 15 B
    ; ============================================================

    cmp rdx, 16
    jb .small15


    ; ============================================================
    ; 16 ~ 31 B
    ; ============================================================

    cmp rdx, 32
    jb .size16_31


    ; ============================================================
    ; 32 ~ 63 B
    ; ============================================================

    cmp rdx, 64
    jb .size32_63


    ; ============================================================
    ; 64 ~ 127 B
    ; ============================================================

    cmp rdx, 128
    jb .size64_127


    ; ============================================================
    ; 128 ~ 255 B
    ; ============================================================

    cmp rdx, 256
    jb .size128_255


    ; ============================================================
    ; >= 256B
    ;
    ; 进入 AVX2 主循环
    ; ============================================================


.loop256:
    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + 64]
    vmovdqu ymm3, [rsi + 96]

    vmovdqu ymm4, [rsi + 128]
    vmovdqu ymm5, [rsi + 160]
    vmovdqu ymm6, [rsi + 192]
    vmovdqu ymm7, [rsi + 224]

    vmovdqu [rdi],       ymm0
    vmovdqu [rdi + 32],  ymm1
    vmovdqu [rdi + 64],  ymm2
    vmovdqu [rdi + 96],  ymm3

    vmovdqu [rdi + 128], ymm4
    vmovdqu [rdi + 160], ymm5
    vmovdqu [rdi + 192], ymm6
    vmovdqu [rdi + 224], ymm7

    add rsi, 256
    add rdi, 256
    sub rdx, 256

    cmp rdx, 256
    jae .loop256


; ================================================================
; AVX 主循环之后的剩余数据
;
; 这里要注意：
; 前面已经使用过 YMM，
; 所以最终返回前需要 vzeroupper。
; ================================================================

.tail_after_avx:

    test rdx, rdx
    jz .done_avx

    cmp rdx, 128
    jae .size128_255

    cmp rdx, 64
    jae .tail64_127

    cmp rdx, 32
    jae .tail32_63

    cmp rdx, 16
    jae .tail16_31

    ; 0~15
    jmp .tail_small15


; ================================================================
; 128 ~ 255B
;
; first128 + last128
;
; 使用：
; YMM0~YMM3 = 前128B
; YMM4~YMM7 = 后128B
; ================================================================

.size128_255:

    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + 64]
    vmovdqu ymm3, [rsi + 96]

    vmovdqu ymm4, [rsi + rdx - 128]
    vmovdqu ymm5, [rsi + rdx - 96]
    vmovdqu ymm6, [rsi + rdx - 64]
    vmovdqu ymm7, [rsi + rdx - 32]

    vmovdqu [rdi], ymm0
    vmovdqu [rdi + 32], ymm1
    vmovdqu [rdi + 64], ymm2
    vmovdqu [rdi + 96], ymm3

    vmovdqu [rdi + rdx - 128], ymm4
    vmovdqu [rdi + rdx - 96], ymm5
    vmovdqu [rdi + rdx - 64], ymm6
    vmovdqu [rdi + rdx - 32], ymm7

    vzeroupper
    ret


; ================================================================
; 64 ~ 127B
;
; first64 + last64
;
; 2 × YMM = 64B
; ================================================================

.size64_127:

    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]

    vmovdqu ymm2, [rsi + rdx - 64]
    vmovdqu ymm3, [rsi + rdx - 32]

    vmovdqu [rdi], ymm0
    vmovdqu [rdi + 32], ymm1

    vmovdqu [rdi + rdx - 64], ymm2
    vmovdqu [rdi + rdx - 32], ymm3

    vzeroupper
    ret


; ================================================================
; 32 ~ 63B
;
; first32 + last32
; ================================================================

.size32_63:

    vmovdqu ymm0, [rsi]

    vmovdqu ymm1, [rsi + rdx - 32]

    vmovdqu [rdi], ymm0

    vmovdqu [rdi + rdx - 32], ymm1

    vzeroupper
    ret


; ================================================================
; 16 ~ 31B
;
; first16 + last16
;
; 这里使用 XMM
; ================================================================

.size16_31:

    vmovdqu xmm0, [rsi]

    vmovdqu xmm1, [rsi + rdx - 16]

    vmovdqu [rdi], xmm0

    vmovdqu [rdi + rdx - 16], xmm1

    ret


; ================================================================
; 0 ~ 15B
; ================================================================

.small15:

    cmp rdx, 8
    jae .size8_15

    cmp rdx, 4
    jae .size4_7

    cmp rdx, 2
    jae .size2_3

    test rdx, rdx
    jz .done_small

    ; 1B
    mov cl, [rsi]
    mov [rdi], cl

.done_small:
    ret


; ================================================================
; 8 ~ 15B
; ================================================================

.size8_15:

    mov rcx, [rsi]
    mov r8, [rsi + rdx - 8]

    mov [rdi], rcx
    mov [rdi + rdx - 8], r8

    ret


; ================================================================
; 4 ~ 7B
; ================================================================

.size4_7:

    mov ecx, [rsi]
    mov r8d, [rsi + rdx - 4]

    mov [rdi], ecx
    mov [rdi + rdx - 4], r8d

    ret


; ================================================================
; 2 ~ 3B
; ================================================================

.size2_3:

    mov cx, [rsi]
    mov r8w, [rsi + rdx - 2]

    mov [rdi], cx
    mov [rdi + rdx - 2], r8w

    ret


; ================================================================
; AVX loop tail: 64~127B
; ================================================================

.tail64_127:

    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]

    vmovdqu ymm2, [rsi + rdx - 64]
    vmovdqu ymm3, [rsi + rdx - 32]

    vmovdqu [rdi], ymm0
    vmovdqu [rdi + 32], ymm1

    vmovdqu [rdi + rdx - 64], ymm2
    vmovdqu [rdi + rdx - 32], ymm3

    jmp .done_avx


; ================================================================
; AVX loop tail: 32~63B
; ================================================================

.tail32_63:

    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + rdx - 32]

    vmovdqu [rdi], ymm0
    vmovdqu [rdi + rdx - 32], ymm1

    jmp .done_avx


; ================================================================
; AVX loop tail: 16~31B
; ================================================================

.tail16_31:

    vmovdqu xmm0, [rsi]
    vmovdqu xmm1, [rsi + rdx - 16]

    vmovdqu [rdi], xmm0
    vmovdqu [rdi + rdx - 16], xmm1

    jmp .done_avx


; ================================================================
; AVX loop tail: 0~15B
; ================================================================

.tail_small15:

    cmp rdx, 8
    jae .tail8_15

    cmp rdx, 4
    jae .tail4_7

    cmp rdx, 2
    jae .tail2_3

    test rdx, rdx
    jz .done_avx

    mov cl, [rsi]
    mov [rdi], cl

    jmp .done_avx


.tail8_15:

    mov rcx, [rsi]
    mov r8, [rsi + rdx - 8]

    mov [rdi], rcx
    mov [rdi + rdx - 8], r8

    jmp .done_avx


.tail4_7:

    mov ecx, [rsi]
    mov r8d, [rsi + rdx - 4]

    mov [rdi], ecx
    mov [rdi + rdx - 4], r8d

    jmp .done_avx


.tail2_3:

    mov cx, [rsi]
    mov r8w, [rsi + rdx - 2]

    mov [rdi], cx
    mov [rdi + rdx - 2], r8w


.done_avx:

    vzeroupper
    ret


section .note.GNU-stack noalloc noexec nowrite progbits
