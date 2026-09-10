#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Переводим PIC с дефолтных векторов 0x08-0x0F (конфликтующих с CPU
// exceptions 0-31!) на 0x20-0x2F — обязательно сделать ДО sti, иначе
// первый же аппаратный IRQ будет интерпретирован как случайное CPU
// исключение (классическая ошибка новичков в OS-dev).
void pic_remap(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28); // master -> 0x20..0x27, slave -> 0x28..0x2F
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0x0);  outb(0xA1, 0x0);
}

void pit_init(uint32_t freq) {
    uint32_t divisor = 1193180 / freq;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}
