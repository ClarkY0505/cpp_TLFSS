section .text

global tlss_avx2_memcpy_5

; -----------------------------------------------------------------------------
; void* tlss_avx2_memcpy_6(void* dst, const void* src, size_t size)
;
; System V AMD64 ABI:
;   rdi = dst
;   rsi = src
;   rdx = size
;   rax = original dst
;
; Contract:
;   - memcpy semantics: source and destination must not overlap.
;   - The caller must verify that AVX is enabled by the OS and that AVX2 is
;     available before calling this function.
;   - YMM0-YMM15 are caller-saved under the System V AMD64 ABI.
; -----------------------------------------------------------------------------

align 16
tlss_avx2_memcpy_5:
    mov rax, rdi

    ; Exact power-of-two sizes are routed to the preceding half-width path.
    ; This avoids copying 16/32/64/128/256-byte inputs twice.
    cmp rdx, 16
    jb .small15
    je .size16

    cmp rdx, 32
    jbe .size17_32

    cmp rdx, 64
    jbe .size33_64

    cmp rdx, 128
    jbe .size65_128

    cmp rdx, 256
    jbe .size129_256

    ; -------------------------------------------------------------------------
    ; 257..511 bytes
    ;
    ; At this size the alignment prologue costs more than it saves.  Copy one
    ; unaligned 256-byte block and finish with the exact-boundary tail paths.
    ; -------------------------------------------------------------------------

    cmp rdx, 512
    jb .copy256_unaligned

    ; -------------------------------------------------------------------------
    ; size >= 512
    ;
    ; Align dst to 32 bytes.  The first unaligned 32-byte store may overlap the
    ; first aligned store; overlapping stores of identical bytes are valid for
    ; memcpy and are cheaper than running the whole loop with split stores.
    ; -------------------------------------------------------------------------

    ; Keep the common aligned case as a not-taken branch which falls directly
    ; into the hot loop.  The alignment peel is intentionally out of line.
    test dil, 31
    jnz .align_dst

; Intentional: over-aligning this 264-byte loop to 32 bytes creates a slower
; front-end layout on recent Intel cores.  Eight bytes keeps fall-through
; compact while still giving the loop a stable boundary.
align 8
.loop512:
    vmovdqu ymm0,  [rsi]
    vmovdqu ymm1,  [rsi + 32]
    vmovdqu ymm2,  [rsi + 64]
    vmovdqu ymm3,  [rsi + 96]
    vmovdqu ymm4,  [rsi + 128]
    vmovdqu ymm5,  [rsi + 160]
    vmovdqu ymm6,  [rsi + 192]
    vmovdqu ymm7,  [rsi + 224]
    vmovdqu ymm8,  [rsi + 256]
    vmovdqu ymm9,  [rsi + 288]
    vmovdqu ymm10, [rsi + 320]
    vmovdqu ymm11, [rsi + 352]
    vmovdqu ymm12, [rsi + 384]
    vmovdqu ymm13, [rsi + 416]
    vmovdqu ymm14, [rsi + 448]
    vmovdqu ymm15, [rsi + 480]

    ; rdi is 32-byte aligned on this path.
    vmovdqa [rdi],       ymm0
    vmovdqa [rdi + 32],  ymm1
    vmovdqa [rdi + 64],  ymm2
    vmovdqa [rdi + 96],  ymm3
    vmovdqa [rdi + 128], ymm4
    vmovdqa [rdi + 160], ymm5
    vmovdqa [rdi + 192], ymm6
    vmovdqa [rdi + 224], ymm7
    vmovdqa [rdi + 256], ymm8
    vmovdqa [rdi + 288], ymm9
    vmovdqa [rdi + 320], ymm10
    vmovdqa [rdi + 352], ymm11
    vmovdqa [rdi + 384], ymm12
    vmovdqa [rdi + 416], ymm13
    vmovdqa [rdi + 448], ymm14
    vmovdqa [rdi + 480], ymm15

    add rsi, 512
    add rdi, 512
    sub rdx, 512
    cmp rdx, 512
    jae .loop512
    jmp .tail_aligned_0_511

.tail_aligned_0_511:

    cmp rdx, 256
    jb .tail_aligned_lt256

    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + 64]
    vmovdqu ymm3, [rsi + 96]
    vmovdqu ymm4, [rsi + 128]
    vmovdqu ymm5, [rsi + 160]
    vmovdqu ymm6, [rsi + 192]
    vmovdqu ymm7, [rsi + 224]

    vmovdqa [rdi],       ymm0
    vmovdqa [rdi + 32],  ymm1
    vmovdqa [rdi + 64],  ymm2
    vmovdqa [rdi + 96],  ymm3
    vmovdqa [rdi + 128], ymm4
    vmovdqa [rdi + 160], ymm5
    vmovdqa [rdi + 192], ymm6
    vmovdqa [rdi + 224], ymm7

    add rsi, 256
    add rdi, 256
    sub rdx, 256


.tail_aligned_lt256:

    test rdx, 128
    jz .tail_aligned_lt128

    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + 64]
    vmovdqu ymm3, [rsi + 96]

    vmovdqa [rdi],      ymm0
    vmovdqa [rdi + 32], ymm1
    vmovdqa [rdi + 64], ymm2
    vmovdqa [rdi + 96], ymm3

    add rsi, 128
    add rdi, 128

.tail_aligned_lt128:

    test rdx, 64
    jz .tail_aligned_lt64

    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]

    vmovdqa [rdi],      ymm0
    vmovdqa [rdi + 32], ymm1

    add rsi, 64
    add rdi, 64

.tail_aligned_lt64:

    test rdx, 32
    jz .tail_aligned_lt32

    vmovdqu ymm0, [rsi]
    vmovdqa [rdi], ymm0

    add rsi, 32
    add rdi, 32

.tail_aligned_lt32:

    test rdx, 16
    jz .tail_aligned_lt16

    vmovdqu xmm0, [rsi]
    vmovdqu [rdi], xmm0

    add rsi, 16
    add rdi, 16


.tail_aligned_lt16:

    test rdx, 8
    jz .tail_aligned_lt8

    mov rcx, [rsi]
    mov [rdi], rcx

    add rsi, 8
    add rdi, 8


.tail_aligned_lt8:

    test rdx, 4
    jz .tail_aligned_lt4

    mov ecx, [rsi]
    mov [rdi], ecx

    add rsi, 4
    add rdi, 4


.tail_aligned_lt4:

    test rdx, 2
    jz .tail_aligned_lt2

    mov cx, [rsi]
    mov [rdi], cx

    add rsi, 2
    add rdi, 2


.tail_aligned_lt2:

    test rdx, 1
    jz .done_avx

    mov cl, [rsi]
    mov [rdi], cl

    jmp .done_avx

.tail_0_511:
    cmp rdx, 256
    jbe .tail_0_256

    ; Copy one aligned 256-byte block.  This leaves a 1..255-byte tail.
    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + 64]
    vmovdqu ymm3, [rsi + 96]
    vmovdqu ymm4, [rsi + 128]
    vmovdqu ymm5, [rsi + 160]
    vmovdqu ymm6, [rsi + 192]
    vmovdqu ymm7, [rsi + 224]

    vmovdqa [rdi],       ymm0
    vmovdqa [rdi + 32],  ymm1
    vmovdqa [rdi + 64],  ymm2
    vmovdqa [rdi + 96],  ymm3
    vmovdqa [rdi + 128], ymm4
    vmovdqa [rdi + 160], ymm5
    vmovdqa [rdi + 192], ymm6
    vmovdqa [rdi + 224], ymm7

    add rsi, 256
    add rdi, 256
    sub rdx, 256

.tail_0_256:
    test rdx, rdx
    jz .done_avx

    ; Use strict-greater comparisons so exact boundaries use the preceding
    ; half-width path rather than copying the same range twice.
    cmp rdx, 128
    ja .tail129_256

    cmp rdx, 64
    ja .tail65_128

    cmp rdx, 32
    ja .tail33_64

    cmp rdx, 16
    ja .tail17_32
    je .tail16

    jmp .tail_small15

.copy256_unaligned:
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
    jmp .tail_0_256

; -----------------------------------------------------------------------------
; Direct paths
; -----------------------------------------------------------------------------

.size129_256:
    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + 64]
    vmovdqu ymm3, [rsi + 96]
    vmovdqu ymm4, [rsi + rdx - 128]
    vmovdqu ymm5, [rsi + rdx - 96]
    vmovdqu ymm6, [rsi + rdx - 64]
    vmovdqu ymm7, [rsi + rdx - 32]

    vmovdqu [rdi],             ymm0
    vmovdqu [rdi + 32],        ymm1
    vmovdqu [rdi + 64],        ymm2
    vmovdqu [rdi + 96],        ymm3
    vmovdqu [rdi + rdx - 128], ymm4
    vmovdqu [rdi + rdx - 96],  ymm5
    vmovdqu [rdi + rdx - 64],  ymm6
    vmovdqu [rdi + rdx - 32],  ymm7
    jmp .done_avx

.size65_128:
    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + rdx - 64]
    vmovdqu ymm3, [rsi + rdx - 32]

    vmovdqu [rdi],            ymm0
    vmovdqu [rdi + 32],       ymm1
    vmovdqu [rdi + rdx - 64], ymm2
    vmovdqu [rdi + rdx - 32], ymm3
    jmp .done_avx

.size33_64:
    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + rdx - 32]

    vmovdqu [rdi],            ymm0
    vmovdqu [rdi + rdx - 32], ymm1
    jmp .done_avx

.size17_32:
    vmovdqu xmm0, [rsi]
    vmovdqu xmm1, [rsi + rdx - 16]

    vmovdqu [rdi],            xmm0
    vmovdqu [rdi + rdx - 16], xmm1
    ret

.size16:
    mov rcx, [rsi]
    mov r8, [rsi + 8]
    mov [rdi], rcx
    mov [rdi + 8], r8
    ret

.small15:
    cmp rdx, 8
    ja .size9_15
    je .size8

    cmp rdx, 4
    ja .size5_7
    je .size4

    cmp rdx, 2
    ja .size3
    je .size2

    test rdx, rdx
    jz .done_small

    mov cl, [rsi]
    mov [rdi], cl

.done_small:
    ret

.size9_15:
    mov rcx, [rsi]
    mov r8, [rsi + rdx - 8]
    mov [rdi], rcx
    mov [rdi + rdx - 8], r8
    ret

.size8:
    mov rcx, [rsi]
    mov [rdi], rcx
    ret

.size5_7:
    mov ecx, [rsi]
    mov r8d, [rsi + rdx - 4]
    mov [rdi], ecx
    mov [rdi + rdx - 4], r8d
    ret

.size4:
    mov ecx, [rsi]
    mov [rdi], ecx
    ret

.size3:
    mov cx, [rsi]
    mov r8b, [rsi + 2]
    mov [rdi], cx
    mov [rdi + 2], r8b
    ret

.size2:
    mov cx, [rsi]
    mov [rdi], cx
    ret

; -----------------------------------------------------------------------------
; AVX tail paths
; -----------------------------------------------------------------------------

.tail129_256:
    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + 64]
    vmovdqu ymm3, [rsi + 96]
    vmovdqu ymm4, [rsi + rdx - 128]
    vmovdqu ymm5, [rsi + rdx - 96]
    vmovdqu ymm6, [rsi + rdx - 64]
    vmovdqu ymm7, [rsi + rdx - 32]

    vmovdqu [rdi],             ymm0
    vmovdqu [rdi + 32],        ymm1
    vmovdqu [rdi + 64],        ymm2
    vmovdqu [rdi + 96],        ymm3
    vmovdqu [rdi + rdx - 128], ymm4
    vmovdqu [rdi + rdx - 96],  ymm5
    vmovdqu [rdi + rdx - 64],  ymm6
    vmovdqu [rdi + rdx - 32],  ymm7
    jmp .done_avx

.tail65_128:
    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + 32]
    vmovdqu ymm2, [rsi + rdx - 64]
    vmovdqu ymm3, [rsi + rdx - 32]

    vmovdqu [rdi],            ymm0
    vmovdqu [rdi + 32],       ymm1
    vmovdqu [rdi + rdx - 64], ymm2
    vmovdqu [rdi + rdx - 32], ymm3
    jmp .done_avx

.tail33_64:
    vmovdqu ymm0, [rsi]
    vmovdqu ymm1, [rsi + rdx - 32]

    vmovdqu [rdi],            ymm0
    vmovdqu [rdi + rdx - 32], ymm1
    jmp .done_avx

.tail17_32:
    vmovdqu xmm0, [rsi]
    vmovdqu xmm1, [rsi + rdx - 16]

    vmovdqu [rdi],            xmm0
    vmovdqu [rdi + rdx - 16], xmm1
    jmp .done_avx

.tail16:
    mov rcx, [rsi]
    mov r8, [rsi + 8]
    mov [rdi], rcx
    mov [rdi + 8], r8
    jmp .done_avx

.tail_small15:
    cmp rdx, 8
    ja .tail9_15
    je .tail8

    cmp rdx, 4
    ja .tail5_7
    je .tail4

    cmp rdx, 2
    ja .tail3
    je .tail2

    test rdx, rdx
    jz .done_avx

    mov cl, [rsi]
    mov [rdi], cl
    jmp .done_avx

.tail9_15:
    mov rcx, [rsi]
    mov r8, [rsi + rdx - 8]
    mov [rdi], rcx
    mov [rdi + rdx - 8], r8
    jmp .done_avx

.tail8:
    mov rcx, [rsi]
    mov [rdi], rcx
    jmp .done_avx

.tail5_7:
    mov ecx, [rsi]
    mov r8d, [rsi + rdx - 4]
    mov [rdi], ecx
    mov [rdi + rdx - 4], r8d
    jmp .done_avx

.tail4:
    mov ecx, [rsi]
    mov [rdi], ecx
    jmp .done_avx

.tail3:
    mov cx, [rsi]
    mov r8b, [rsi + 2]
    mov [rdi], cx
    mov [rdi + 2], r8b
    jmp .done_avx

.tail2:
    mov cx, [rsi]
    mov [rdi], cx

.done_avx:
    vzeroupper
    ret

; -----------------------------------------------------------------------------
; Cold destination-alignment path for copies of at least 512 bytes.
; -----------------------------------------------------------------------------

align 16
.align_dst:
    mov ecx, edi
    neg ecx
    and ecx, 31

    vmovdqu ymm0, [rsi]
    vmovdqu [rdi], ymm0

    add rsi, rcx
    add rdi, rcx
    sub rdx, rcx

    ; The alignment peel can reduce an original 512-byte request below one
    ; complete block.
    cmp rdx, 512
    ;jb .tail_0_511
    jb .tail_aligned_0_511
    jmp .loop512

section .note.GNU-stack noalloc noexec nowrite progbits
