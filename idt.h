#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// Формат дескриptora IDT в 64-битном режиме (16 байт, а не 8 как в 32-бит)
typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;        // Interrupt Stack Table index (0 = не используем)
    uint8_t  type_attr;  // present | DPL | type (0x8E = present, ring0, interrupt gate)
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;

// Регистры, которые сохраняет наш общий stub перед вызовом C-обработчика.
// Порядок ДОЛЖЕН точно совпадать с последовательностью push в isr.s
// (стек растёт вниз, последний push = самый младший адрес = поле r15).
// rsp/ss сюда не входят: CPU кладёт их сам ТОЛЬКО при смене privilege
// level (ring3->ring0); у нас всё ядро работает в ring0, поэтому CPU
// на прерывании кладёт только rip, cs, rflags (без error_code — 3 поля,
// с error_code для части исключений — см. ISR_ERR/ISR_NOERR в isr.s).
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;   // кладёт наш stub (vector) / CPU (error_code)
    uint64_t rip, cs, rflags;      // кладёт сам CPU при входе в прерывание (ring0->ring0)
} interrupt_frame_t;

void idt_init(void);
void idt_load_current_cpu(void);
void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist, uint8_t dpl);

// Общий обработчик, куда стекаются все исключения/прерывания
void interrupt_dispatch(interrupt_frame_t *frame);

#endif
