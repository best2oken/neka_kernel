; boot64.s — переход из 32-бит protected mode (куда нас ставит GRUB по
; мультибут-спеке) в 64-бит long mode. Классическая последовательность:
;   1) построить таблицы страниц (identity map первый 1 ГБ, 2MB-страницы)
;   2) включить PAE (CR4.PAE)
;   3) загрузить CR3 = адрес PML4
;   4) включить LME (EFER MSR, бит 8) — "разрешение" long mode
;   5) включить paging (CR0.PG) — теперь мы в IA-32e compatibility submode
;   6) far jump на 64-битный code segment — вот тут реально "прыгаем" в long mode

section .multiboot
align 4
    dd 0x1BADB002
    dd 0x02                        ; flags: bit1 = запросить memory map у GRUB
    dd -(0x1BADB002 + 0x02)

section .bss
align 4096
global pml4
global pdpt
global pd
pml4:      resb 4096
pdpt:      resb 4096
pd:        resb 4096
align 16
stack_bottom: resb 16384
stack_top:
mbi_ptr:   resd 1

section .text
bits 32
global _start
extern kernel_main64

_start:
    mov esp, stack_top
    mov [mbi_ptr], ebx      ; GRUB кладёт указатель на multiboot info в EBX —
                             ; сохраняем СРАЗУ, до того как ebx/остальные
                             ; регистры будут затёрты кодом ниже (page tables
                             ; используют eax/ecx, но лучше перестраховаться)

    ; --- Построение таблиц страниц: identity map 0..1GiB через 2MB-страницы ---
    ; PML4[0] -> PDPT
    mov eax, pdpt
    or eax, 0x03            ; present | writable
    mov [pml4], eax

    ; PDPT[0] -> PD
    mov eax, pd
    or eax, 0x03
    mov [pdpt], eax

    ; PD[i] -> 2MB-страница физ.адреса i*2MB, для i=0..511 (покрывает 1GiB)
    mov ecx, 0
.fill_pd:
    mov eax, ecx
    shl eax, 21              ; eax = ecx * 2MB
    or eax, 0x83             ; present | writable | page size (2MB)
    mov [pd + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jl .fill_pd

    ; --- Включаем PAE (обязательно для long mode) ---
    mov eax, cr4
    or eax, 1 << 5           ; CR4.PAE
    mov cr4, eax

    ; --- Загружаем CR3 = физ.адрес PML4 ---
    mov eax, pml4
    mov cr3, eax

    ; --- Включаем LME (Long Mode Enable) в EFER MSR (0xC0000080) ---
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; --- Включаем paging (CR0.PG). PE уже включён GRUB'ом. ---
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; --- Загружаем GDT с 64-битным code segment (L-бит=1) ---
    lgdt [gdt64.pointer]

    ; --- Far jump на 64-битный сегмент — точка настоящего перехода ---
    jmp gdt64.code:long_mode_start

section .rodata
align 16
gdt64:
    dq 0                                    ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) ; code: executable, code/data=1, present, long-mode(L)
.data: equ $ - gdt64
    dq (1<<44) | (1<<47) | (1<<41)           ; data: code/data=1, present, writable
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .text
bits 64
long_mode_start:
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, stack_top
    mov edi, [mbi_ptr]   ; 1-й аргумент kernel_main64 по SysV ABI; mov в
                          ; 32-битный edi обнуляет верхние 32 бита rdi сам
    call kernel_main64

.hang:
    cli
    hlt
    jmp .hang
