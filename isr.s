bits 64
section .text

extern interrupt_dispatch

; Часть исключений CPU сама кладёт error_code на стек (8,10,11,12,13,14,17),
; остальные — нет. Чтобы кадр стека был единообразным, для "безerror_code"
; векторов сами кладём фиктивный 0.
%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0        ; фиктивный error_code
    push qword %1        ; номер вектора
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1        ; вектор (error_code уже на стеке от CPU)
    jmp isr_common_stub
%endmacro

ISR_NOERR 0   ; #DE Divide Error
ISR_NOERR 1   ; #DB Debug
ISR_NOERR 2   ; NMI
ISR_NOERR 3   ; #BP Breakpoint
ISR_NOERR 4   ; #OF Overflow
ISR_NOERR 5   ; #BR Bound Range
ISR_NOERR 6   ; #UD Invalid Opcode
ISR_NOERR 7   ; #NM Device Not Available (FPU/SSE!)
ISR_ERR   8   ; #DF Double Fault
ISR_NOERR 9
ISR_ERR   10  ; #TS Invalid TSS
ISR_ERR   11  ; #NP Segment Not Present
ISR_ERR   12  ; #SS Stack Fault
ISR_ERR   13  ; #GP General Protection
ISR_ERR   14  ; #PF Page Fault
ISR_NOERR 15
ISR_NOERR 16  ; #MF x87 FPU error
ISR_ERR   17  ; #AC Alignment Check
ISR_NOERR 18  ; #MC Machine Check
ISR_NOERR 19  ; #XM SIMD FP exception
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; IRQ0 (таймер, PIT) и IRQ1 (клавиатура) — remapped на 32 и 33 (см. pic_remap)
ISR_NOERR 32
ISR_NOERR 33

; int 0x80 — программный syscall gate, единственный vector с DPL=3
; (см. idt.c) — только он ring3-код имеет право вызвать через "int".
; Любая попытка выполнить "int" на векторах 0-33 из ring3 даст #GP,
; так как их дескрипторы остаются DPL=0.
ISR_NOERR 128

isr_common_stub:
    ; Сохраняем регистры общего назначения (System V ABI не гарантирует
    ; их сохранность при асинхронном прерывании, сохраняем всё вручную)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp             ; rdi = указатель на interrupt_frame_t (1-й арг по SysV ABI)
    call interrupt_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16               ; убираем vector + error_code со стека
    iretq
