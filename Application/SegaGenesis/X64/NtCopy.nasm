;------------------------------------------------------------------------------
; NtCopy.nasm
;
; Non-temporal memcpy для записи в UEFI GOP framebuffer на слабых CPU
; без Write-Combining на VRAM. MOVNTI обходит кэш и, если регион в PAT
; отмечен как UC- (а не строгий UC), объединяет записи в WC-буферах CPU,
; что кратно ускоряет MMIO по сравнению с обычным MOV.
;
; VOID EFIAPI NtCopyMem64 (VOID *Dst, CONST VOID *Src, UINTN Bytes);
;
; Windows x64 ABI:
;   RCX = Dst, RDX = Src, R8 = Bytes
; Не требует выравнивания. Дёшево обрабатывает произвольный размер:
;   основной цикл — блоки по 32 байта (4 x MOVNTI qword),
;   хвост — 8 байт за раз через MOVNTI,
;   финальный хвост (0..7 байт) — обычным MOV/rep movsb.
; В конце SFENCE — обязателен, иначе NT-write'ы могут не долететь до VRAM
; до следующей MMIO-транзакции.
;------------------------------------------------------------------------------

    DEFAULT REL
    SECTION .text

global ASM_PFX(NtCopyMem64)
ASM_PFX(NtCopyMem64):
    ; RCX=Dst, RDX=Src, R8=Bytes
    mov     r9,  rcx        ; r9  = Dst (savable)
    mov     r10, rdx        ; r10 = Src (savable)
    mov     r11, r8         ; r11 = Bytes remaining

    ; --- Основной цикл: 32 байта за итерацию ------------------------------
    cmp     r11, 32
    jb      .tail8
.loop32:
    mov     rax, [r10 + 0]
    movnti  [r9 + 0],  rax
    mov     rax, [r10 + 8]
    movnti  [r9 + 8],  rax
    mov     rax, [r10 + 16]
    movnti  [r9 + 16], rax
    mov     rax, [r10 + 24]
    movnti  [r9 + 24], rax
    add     r10, 32
    add     r9,  32
    sub     r11, 32
    cmp     r11, 32
    jae     .loop32

    ; --- Хвост по 8 байт --------------------------------------------------
.tail8:
    cmp     r11, 8
    jb      .tail1
.loop8:
    mov     rax, [r10]
    movnti  [r9], rax
    add     r10, 8
    add     r9,  8
    sub     r11, 8
    cmp     r11, 8
    jae     .loop8

    ; --- Остаток 0..7 байт обычным rep movsb -----------------------------
.tail1:
    test    r11, r11
    jz      .done
    ; rep movsb: RSI=src, RDI=dst, RCX=count. Сохраняем/восстанавливаем RSI,RDI.
    push    rsi
    push    rdi
    mov     rsi, r10
    mov     rdi, r9
    mov     rcx, r11
    cld
    rep movsb
    pop     rdi
    pop     rsi

.done:
    sfence                  ; сериализуем NT-writes перед возвратом
    ret
