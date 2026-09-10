; Multiboot header — позволяет GRUB загрузить наше ядро
section .multiboot
align 4
    dd 0x1BADB002              ; magic
    dd 0x00                    ; flags
    dd -(0x1BADB002 + 0x00)    ; checksum

section .bss
align 16
stack_bottom:
    resb 16384                 ; 16 KB стек
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top         ; настроить стек
    push ebx                   ; multiboot info (не используем пока)
    push eax                   ; multiboot magic
    call kernel_main
    cli
.hang:
    hlt
    jmp .hang
