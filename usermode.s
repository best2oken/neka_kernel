; void enter_usermode(uint64_t entry, uint64_t user_stack_top)
; System V ABI: entry в RDI, user_stack_top в RSI.
;
; iretq — единственный штатный способ ПОНИЗИТЬ privilege level (перейти
; из ring0 в ring3): кладём на стек ровно то, что CPU сам кладёт при
; входе в прерывание из ring3 (SS, RSP, RFLAGS, CS, RIP), только вручную
; и с user-селекторами (RPL=3 в младших 2 битах CS/SS). iretq читает эти
; 5 значений и одним махом переключает CPL, стек и IP.
bits 64
section .text
global enter_usermode

SEL_UCODE equ 0x18
SEL_UDATA equ 0x20

enter_usermode:
    mov ax, SEL_UDATA | 3     ; RPL=3 — обязательно, иначе #GP при загрузке
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push qword (SEL_UDATA | 3)  ; SS
    push rsi                     ; RSP (user-стек)
    push qword 0x202             ; RFLAGS: IF=1 (бит9), бит1 зарезервирован=1
    push qword (SEL_UCODE | 3)  ; CS
    push rdi                     ; RIP (entry)
    iretq
