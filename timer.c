#include "kernel.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Переводим PIC с дефолтных векторов (конфликтующих с CPU exceptions)
// на 0x20-0x2F — стандартная процедура для любого x86 kernel, EKA2 делает
// то же самое для своего interrupt controller driver под ARM/x86.
void pic_remap(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28); // offset: master->0x20, slave->0x28
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0x0);  outb(0xA1, 0x0);
}

// Программируем PIT (Programmable Interval Timer) на нужную частоту —
// аналог настройки тикающего таймера, который в EKA2 двигает iTime
// через TimesliceTick() на каждый тик.
void timer_init(uint32_t freq) {
    uint32_t divisor = 1193180 / freq;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}
