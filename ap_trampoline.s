; ap_trampoline.s — код, который выполняет Application Processor сразу
; после INIT-SIPI-SIPI. AP стартует в 16-битном real mode с CS:IP,
; заданным вектором SIPI (физ.адрес = vector << 12), поэтому этот файл
; собирается ОТДЕЛЬНО как raw binary (-f bin, без ELF-обёртки) и
; копируется BSP-процессором на фиксированный низкий физический адрес
; (TRAMPOLINE_ADDR, см. smp.c) ДО отправки SIPI.
;
; Raw binary не хранит символы — поэтому "почтовый ящик" (значения,
; которые BSP кладёт СЮДА перед стартом AP: физ.адрес kernel_pml4,
; указатель на настоящую GDT ядра, стек и entry point для AP) живёт по
; ЗАРАНЕЕ ЗАФИКСИРОВАННОМУ смещению 0x100 от начала блоба — и asm, и C
; код (smp.c) обязаны согласованно знать этот layout руками, раз
; настоящих символов тут нет.

bits 16
org 0x8000  ; должен совпадать с TRAMPOLINE_ADDR в smp.c

ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    lgdt [gdt32_ptr]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:protected_mode_entry  ; far jump — обязателен, сбрасывает prefetch queue и грузит 32-битный CS

bits 32
protected_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; --- PAE ---
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; --- CR3 = физ.адрес kernel_pml4 (положен BSP в почтовый ящик) ---
    mov eax, [mbox_kernel_pml4]
    mov cr3, eax

    ; --- EFER.LME ---
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; --- paging on ---
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; Настоящая 64-битная GDT ядра (не временная выше) — в почтовом
    ; ящике лежит АДРЕС структуры gdt_ptr_t (limit+base), а не сама
    ; структура — нужна косвенная загрузка, иначе lgdt прочитает 10
    ; байт ПРЯМО ИЗ ЯЩИКА (то есть саму 8-байтовую адресную величину
    ; плюс мусор рядом) как будто это и есть {limit,base}. БАГ, пойманный
    ; на практике: без этой косвенности AP триплфолтился прямо на
    ; следующей инструкции (far jump в 64-бит) с #GP из-за мусорной GDT.
    mov eax, [mbox_real_gdt_ptr]
    lgdt [eax]

    jmp 0x08:long_mode_entry

bits 64
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [mbox_ap_stack]
    mov rax, [mbox_ap_entry]
    call rax
.hang:
    cli
    hlt
    jmp .hang

; --- временная 32-битная GDT (только для прыжка real->protected) ---
align 8
gdt32_start:
    dq 0
    dq 0x00CF9A000000FFFF  ; code32, flat 4GB
    dq 0x00CF92000000FFFF  ; data32, flat 4GB
gdt32_end:
gdt32_ptr:
    dw gdt32_end - gdt32_start - 1
    dd gdt32_start

; --- Почтовый ящик: ФИКСИРОВАННОЕ смещение 0x100 от начала блоба.
; Синхронизировано вручную с MBOX_* константами в smp.c — при правке
; этого файла обязательно проверить, что оба места согласованы. ---
times (0x100 - ($ - ap_trampoline_start)) db 0
mbox_kernel_pml4:  dq 0
mbox_real_gdt_ptr: dq 0
mbox_ap_stack:     dq 0
mbox_ap_entry:     dq 0
ap_trampoline_end:
